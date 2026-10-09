#include "occurrence_matcher.hpp"
#include "occurrence_analysis.hpp"
#include "ast_machine.hpp"

#include <algorithm>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <utility>

namespace crank {
namespace {

std::size_t add(std::size_t a, std::size_t b) {
    return b > no_id - a ? no_id : a + b;
}
std::size_t multiply(std::size_t a, std::size_t b) {
    return a && b > no_id / a ? no_id : a * b;
}

// Each resolved component is frozen. If a component is absent in an open
// scope, subsequent updates select its first future matching occurrence.
struct PathCursor {
    std::size_t path = no_id;
    std::size_t owner = no_id;
    std::size_t step = 0;
    std::size_t selected_count = no_id;
    auto operator<=>(const PathCursor&) const = default;
};
enum class LookupState { Ready, Pending, Missing };
struct Lookup { LookupState state; std::size_t value = 0; };

std::size_t anchor_scope(const std::vector<OccurrenceRecord>& records,
                         const CountPath& path, std::size_t scope) {
    if (records.empty() || scope >= records.size()) return no_id;
    if (path.anchor == OccurrenceAnchor::Root) return 0;
    if (path.anchor == OccurrenceAnchor::Ancestor) {
        for (std::size_t i = 0; i < path.parents; ++i) {
            scope = records.at(scope).parent;
            if (scope == no_id) return no_id;
        }
    }
    return scope;
}

template<class Step>
Lookup resolve_cursor(const std::vector<OccurrenceRecord>& records,
                      const CountPath& path, PathCursor& cursor, Step&& step) {
    if (cursor.owner == no_id || cursor.owner >= records.size())
        return {LookupState::Missing};
    while (cursor.step < path.steps.size()) {
        step();
        const auto& part = path.steps[cursor.step];
        std::size_t selected = cursor.selected_count;
        if (selected == no_id) {
            const auto& owner = records.at(cursor.owner);
            std::size_t ordinal = 0;
            for (const auto child : owner.children) {
                step();
                const auto& record = records.at(child);
                const bool matches = record.source_id == part.id &&
                    ((part.source == OccurrenceSource::Count && record.kind == OccurrenceKind::Count) ||
                     (part.source == OccurrenceSource::Group && record.kind == OccurrenceKind::Group));
                if (!matches) continue;
                if (part.source == OccurrenceSource::Group && part.index) {
                    if (ordinal++ == *part.index) { selected = child; break; }
                } else {
                    selected = child; // Latest existing occurrence in this scope.
                }
            }
            if (selected == no_id)
                return {owner.closed ? LookupState::Missing : LookupState::Pending};
        }
        if (part.source == OccurrenceSource::Count && part.index) {
            cursor.selected_count = selected;
            const auto& count = records.at(selected);
            std::size_t iteration = no_id;
            for (const auto child : count.children) {
                step();
                const auto& record = records.at(child);
                if (record.kind == OccurrenceKind::Iteration && record.iteration == part.index) {
                    iteration = child;
                    break;
                }
            }
            if (iteration == no_id)
                return {count.closed ? LookupState::Missing : LookupState::Pending};
            selected = iteration;
        }
        cursor.owner = selected;
        cursor.selected_count = no_id;
        ++cursor.step;
    }
    step();
    const auto& record = records.at(cursor.owner);
    switch (path.metric) {
    case OccurrenceMetric::Count:
        if (record.kind != OccurrenceKind::Count) return {LookupState::Missing};
        if (!record.closed) return {LookupState::Pending};
        return record.count ? Lookup{LookupState::Ready, *record.count} : Lookup{LookupState::Missing};
    case OccurrenceMetric::Depth:
        if (record.kind != OccurrenceKind::Group && record.kind != OccurrenceKind::Root)
            return {LookupState::Missing};
        return {LookupState::Ready, record.depth};
    case OccurrenceMetric::Height:
        if (record.kind != OccurrenceKind::Group && record.kind != OccurrenceKind::Root)
            return {LookupState::Missing};
        if (!record.closed) return {LookupState::Pending};
        return {LookupState::Ready, record.height};
    case OccurrenceMetric::Bytes:
        if (!record.closed) return {LookupState::Pending};
        return {LookupState::Ready, record.span.end - record.span.begin};
    }
    return {LookupState::Missing};
}

struct Obligation {
    PathCursor cursor;
    std::size_t expected = 0;
};
struct Environment {
    std::size_t version = 0;
    std::vector<std::optional<Span>> captures;
    std::vector<std::optional<std::size_t>> counts;
    std::vector<std::optional<Span>> pending;
    std::vector<OccurrenceRecord> records;
    std::vector<Obligation> obligations;
};

enum class TaskKind { Eval, CloseGroup, Repeat, AfterIteration };
struct Task {
    Task() = default;
    Task(TaskKind task_kind, NodeId node_id) : kind(task_kind), node(node_id) {}
    TaskKind kind = TaskKind::Eval;
    NodeId node = no_id;
    std::size_t occurrence = no_id;
    std::size_t parent_scope = no_id;
    std::size_t prior_calls = 0;
    std::size_t lower = 0;
    std::optional<std::size_t> upper;
    std::size_t count = 0;
    std::size_t position = 0;
    std::size_t version = 0;
    bool stop = false;
    std::size_t capture = no_id;
    auto operator<=>(const Task&) const = default;
};
struct Frame {
    Task task;
    std::size_t next = no_id;
    std::size_t minimum = 0;
    std::size_t identity = no_id;
};
struct Configuration {
    std::size_t position = 0;
    std::size_t scope = 0;
    std::size_t calls = 0;
    std::shared_ptr<Environment> environment;
    std::size_t continuation = no_id;
};

// A scalar query observes an environment, not the path that produced it.
// Its continuation identity excludes only an unbounded repetition counter once
// its lower bound has been met and the count itself has no observer.
struct ScalarKey {
    std::size_t position, calls, continuation;
    std::vector<std::optional<Span>> captures, pending;
    std::vector<std::optional<std::size_t>> counts;
    auto operator<=>(const ScalarKey&) const = default;
};

class Machine {
public:
    Machine(const Ast& ast, const OccurrenceAnalysis& analysis,
            const std::vector<bool>& callable, const std::vector<bool>& pure,
            const std::vector<bool>& write_only, bool whole_rule,
            std::string_view subject, OccurrenceMatchOptions options,
            bool observe_scalars = true, bool trace = false)
        : ast_(ast), analysis_(analysis), callable_(callable), pure_(pure),
          write_only_(write_only), whole_rule_(whole_rule),
          subject_(subject), options_(options),
          history_(trace || std::any_of(ast.nodes.begin(), ast.nodes.end(),
                    [](const Node& node) { return node.count_path != no_id; })),
          capture_slots_(ast.capture_names.size(), no_id),
          count_slots_(ast.count_names.size(), no_id) {
        if (observe_scalars || history_) {
            std::fill(capture_slots_.begin(), capture_slots_.end(), 0);
            std::fill(count_slots_.begin(), count_slots_.end(), 0);
        } else {
            for (const auto& node : ast.nodes) {
                if (node.kind == Kind::Reference) capture_slots_.at(node.capture_id) = 0;
                if (node.use_count != no_id) count_slots_.at(node.use_count) = 0;
            }
        }
        for (auto& slot : capture_slots_) if (slot != no_id) slot = capture_count_++;
        for (auto& slot : count_slots_) if (slot != no_id) slot = count_count_++;
        if (!history_) {
            for (NodeId id = 0; id < ast.nodes.size(); ++id) {
                const auto& node = ast.nodes[id];
                pure_[id] = node.kind != Kind::Call && node.kind != Kind::Reference &&
                    (node.kind != Kind::Capture || capture_slots_.at(node.capture_id) == no_id) &&
                    (node.bind_count == no_id || count_slots_.at(node.bind_count) == no_id) &&
                    node.use_count == no_id && node.count_path == no_id;
            }
            bool changed;
            do {
                changed = false;
                for (NodeId id = 0; id < ast.nodes.size(); ++id) {
                    if (!pure_[id] || ast.nodes[id].kind == Kind::Define) continue;
                    for (auto child : ast.nodes[id].children)
                        if (!pure_.at(child)) { pure_[id] = false; changed = true; break; }
                }
            } while (changed);
        }
    }

    const OccurrenceStatistics& statistics() const { return stats_; }

    // The visitor returns true only when a witnessed answer is sufficient.
    bool run(std::size_t start, bool full,
             const std::function<bool(const Configuration&)>& visitor) {
        if (start > subject_.size())
            throw std::out_of_range("occurrence matcher: start position exceeds subject length");
        start_ = start;
        const auto& root_length = analysis_.length(ast_.root);
        if (!root_length.productive || root_length.minimum > subject_.size() - start) {
            ++stats_.pruned;
            return false;
        }
        if (options_.prune && full) {
            if (!root_length.accepts(subject_.size() - start)) {
                ++stats_.pruned;
                ++stats_.arithmetic_pruned;
                return false;
            }
            if (analysis_.height_equation()) {
                expected_height_ = analysis_.height_equation()->height_for_length(subject_.size() - start);
                if (!expected_height_) {
                    ++stats_.pruned;
                    ++stats_.arithmetic_pruned;
                    return false;
                }
            }
        }
        Configuration initial;
        initial.position = start;
        initial.environment = std::make_shared<Environment>();
        auto& environment = *initial.environment;
        state_units(add(1, add(multiply(2, capture_count_), count_count_)));
        environment.captures.resize(capture_count_);
        environment.counts.resize(count_count_);
        environment.pending.resize(capture_count_);
        if (history_) {
            occurrence_units(1);
            OccurrenceRecord root;
            root.id = 0;
            root.kind = OccurrenceKind::Root;
            root.source_id = 0;
            root.source_node = ast_.root;
            root.name = "$0";
            root.span = {start, start};
            root.source_rule = whole_rule_;
            environment.records.push_back(std::move(root));
        }
        if (whole_rule_) {
            if (!enter_rule(initial)) { finish_incomplete(); return false; }
        }
        push(initial, Task{TaskKind::Eval, ast_.root});
        enqueue(std::move(initial));
        while (!queue_.empty()) {
            step();
            Configuration configuration = std::move(queue_.front());
            queue_.pop_front();
            if (configuration.continuation == no_id) {
                if (full && configuration.position != subject_.size()) continue;
                if (std::any_of(configuration.environment->pending.begin(),
                                configuration.environment->pending.end(),
                                [](const auto& pending) { return pending.has_value(); })) continue;
                if (history_) close_record(configuration, 0);
                if (!check_obligations(configuration) ||
                    !configuration.environment->obligations.empty()) continue;
                if (visitor(configuration)) return true;
                continue;
            }
            if (frames_.at(configuration.continuation).minimum >
                subject_.size() - configuration.position) {
                ++stats_.pruned;
                continue;
            }
            const Task task = frames_.at(configuration.continuation).task;
            configuration.continuation = frames_.at(configuration.continuation).next;
            switch (task.kind) {
            case TaskKind::Eval: evaluate(std::move(configuration), task.node); break;
            case TaskKind::CloseGroup: close_group(std::move(configuration), task); break;
            case TaskKind::Repeat: repeat(std::move(configuration), task); break;
            case TaskKind::AfterIteration: after_iteration(std::move(configuration), task); break;
            }
        }
        finish_incomplete();
        return false;
    }

    Outcome outcome(const Configuration& configuration) {
        state_units(1 + configuration.environment->captures.size() + configuration.environment->counts.size());
        Outcome result;
        result.end = configuration.position;
        result.captures.resize(ast_.capture_names.size());
        result.counts.resize(ast_.count_names.size());
        for (std::size_t i = 0; i < capture_slots_.size(); ++i)
            if (capture_slots_[i] != no_id)
                result.captures[i] = configuration.environment->captures[capture_slots_[i]];
        for (std::size_t i = 0; i < count_slots_.size(); ++i)
            if (count_slots_[i] != no_id)
                result.counts[i] = configuration.environment->counts[count_slots_[i]];
        if (result.captures.empty()) result.captures.resize(1);
        result.captures[0] = Span{start_, configuration.position};
        return result;
    }

    TracedOutcome traced_outcome(const Configuration& configuration) {
        auto scalar = outcome(configuration);
        occurrence_units(configuration.environment->records.size());
        return {std::move(scalar), configuration.environment->records};
    }

    void state_units(std::size_t amount = 1) {
        if (amount > options_.max_states - stats_.states) limit("state allocation limit exceeded");
        stats_.states += amount;
    }

private:
    [[noreturn]] void limit(const char* message) {
        stats_.limit_reached = true;
        throw ResourceLimit(std::string("occurrence matcher: ") + message);
    }
    void step(std::size_t amount = 1) {
        if (amount > options_.max_steps - stats_.work_steps) limit("work limit exceeded");
        stats_.work_steps += amount;
    }
    void occurrence_units(std::size_t amount) {
        if (amount > options_.max_occurrences - stats_.occurrences)
            limit("occurrence history allocation limit exceeded");
        stats_.occurrences += amount;
    }
    void deferred(const char* reason) {
        stats_.limit_reached = true;
        if (deferred_reason_.empty()) deferred_reason_ = reason;
    }
    void finish_incomplete() {
        if (!deferred_reason_.empty()) limit(deferred_reason_.c_str());
    }
    Environment& mutate(Configuration& configuration) {
        auto& pointer = configuration.environment;
        if (!pointer.unique()) {
            state_units(1 + pointer->captures.size() + pointer->counts.size() +
                        pointer->pending.size() + pointer->obligations.size());
            occurrence_units(pointer->records.size());
            pointer = std::make_shared<Environment>(*pointer);
        }
        pointer->version = ++version_;
        return *pointer;
    }

    std::size_t task_minimum(const Task& task) const {
        switch (task.kind) {
        case TaskKind::Eval: return analysis_.length(task.node).minimum;
        case TaskKind::Repeat:
        case TaskKind::AfterIteration: {
            const auto remaining = task.count < task.lower ? task.lower - task.count : 0;
            return multiply(remaining, analysis_.length(ast_.nodes.at(task.node).children.at(0)).minimum);
        }
        case TaskKind::CloseGroup: return 0;
        }
        return 0;
    }
    void push(Configuration& configuration, Task task) {
        state_units();
        const auto minimum = add(task_minimum(task), configuration.continuation != no_id ?
                                 frames_.at(configuration.continuation).minimum : 0);
        const auto frame = frames_.size();
        std::size_t identity = frame;
        if (!history_) {
            Task semantic = task;
            semantic.version = 0; // Optimization hint, never a language observation.
            if (semantic.kind == TaskKind::Repeat) semantic.position = 0;
            if (semantic.kind == TaskKind::Repeat && !semantic.upper &&
                (ast_.nodes.at(semantic.node).bind_count == no_id ||
                 count_slots_.at(ast_.nodes.at(semantic.node).bind_count) == no_id))
                semantic.count = std::min(semantic.count, semantic.lower);
            const auto next = configuration.continuation == no_id ? no_id :
                              frames_.at(configuration.continuation).identity;
            const auto [entry, inserted] = continuation_ids_.try_emplace(
                std::make_pair(semantic, next), continuation_ids_.size());
            identity = entry->second;
            if (inserted) state_units();
        }
        frames_.push_back(Frame{std::move(task), configuration.continuation, minimum, identity});
        configuration.continuation = frame;
    }

    bool check_obligations(Configuration& configuration) {
        std::size_t index = 0;
        while (index < configuration.environment->obligations.size()) {
            step();
            const auto original = configuration.environment->obligations[index];
            auto cursor = original.cursor;
            ++stats_.path_checks;
            const auto& path = ast_.count_paths.at(cursor.path);
            const auto result = resolve_cursor(configuration.environment->records,
                path, cursor, [&] { step(); });
            if (result.state == LookupState::Missing ||
                (result.state == LookupState::Ready && result.value != original.expected) ||
                (result.state == LookupState::Pending && cursor.step == path.steps.size() &&
                 path.metric == OccurrenceMetric::Height &&
                 configuration.environment->records.at(cursor.owner).height > original.expected)) {
                ++stats_.pruned;
                return false;
            }
            if (result.state == LookupState::Ready) {
                auto& obligations = mutate(configuration).obligations;
                obligations.erase(obligations.begin() + static_cast<std::ptrdiff_t>(index));
                continue;
            }
            if (cursor != original.cursor) mutate(configuration).obligations[index].cursor = cursor;
            ++index;
        }
        return true;
    }
    void enqueue(Configuration configuration) {
        if (!check_obligations(configuration)) return;
        if (configuration.continuation != no_id &&
            frames_.at(configuration.continuation).minimum > subject_.size() - configuration.position) {
            ++stats_.pruned;
            return;
        }
        if (!history_) {
            const auto& environment = *configuration.environment;
            ScalarKey key{configuration.position, configuration.calls,
                configuration.continuation == no_id ? no_id :
                    frames_.at(configuration.continuation).identity,
                environment.captures, environment.pending, environment.counts};
            step();
            if (seen_.contains(key)) { ++stats_.memo_hits; return; }
            state_units(1 + key.captures.size() + key.pending.size() + key.counts.size());
            seen_.insert(std::move(key));
            ++stats_.memo_entries;
        }
        state_units();
        queue_.push_back(std::move(configuration));
    }

    bool enter_rule(Configuration& configuration) {
        if (configuration.calls >= options_.max_call_depth) {
            deferred("source-rule depth limit left unexplored histories");
            return false;
        }
        ++configuration.calls;
        stats_.max_call_depth = std::max(stats_.max_call_depth, configuration.calls);
        return true;
    }

    std::size_t open_record(Configuration& configuration, OccurrenceKind kind,
                            std::size_t source, NodeId node, bool rule = false,
                            std::optional<std::size_t> iteration = {}) {
        occurrence_units(1);
        auto& environment = mutate(configuration);
        OccurrenceRecord record;
        record.id = environment.records.size();
        record.parent = configuration.scope;
        record.kind = kind;
        record.source_id = source;
        record.source_node = node;
        record.span = {configuration.position, configuration.position};
        record.depth = configuration.calls ? configuration.calls - 1 : 0;
        record.source_rule = rule;
        record.iteration = iteration;
        if (kind == OccurrenceKind::Group) {
            record.name = ast_.capture_names.at(source);
            if (record.name.empty()) record.name = "$" + std::to_string(source);
        } else if (source != no_id) record.name = ast_.count_names.at(source);
        const auto id = record.id;
        environment.records.at(configuration.scope).children.push_back(id);
        environment.records.push_back(std::move(record));
        configuration.scope = id;
        if (rule) {
            // Every successful continuation contains this source invocation.
            // Its ancestors already have at least this many source-rule edges
            // below them, even before any return. Height remains unavailable
            // to scalar lookup until close, but this monotone lower bound can
            // refute a guessed forward height immediately.
            auto ancestor = environment.records.at(id).parent;
            std::size_t minimum_height = 1;
            while (ancestor != no_id) {
                step();
                auto& owner = environment.records.at(ancestor);
                owner.height = std::max(owner.height, minimum_height);
                minimum_height = add(minimum_height, owner.source_rule ? 1 : 0);
                ancestor = owner.parent;
            }
        }
        return id;
    }

    void close_record(Configuration& configuration, std::size_t id,
                      std::optional<std::size_t> count = {}) {
        auto& environment = mutate(configuration);
        auto& record = environment.records.at(id);
        record.span.end = configuration.position;
        record.count = count;
        record.closed = true;
        if (record.parent != no_id) {
            auto& parent = environment.records.at(record.parent);
            parent.height = std::max(parent.height, add(record.height, record.source_rule ? 1 : 0));
        }
    }

    void enter_group(Configuration configuration, NodeId node_id,
                     std::size_t capture_id, NodeId body, bool rule) {
        const auto prior_calls = configuration.calls;
        if (rule && !enter_rule(configuration)) return;
        const auto parent = configuration.scope;
        const auto occurrence = history_ ? open_record(configuration, OccurrenceKind::Group,
                                             capture_id, node_id, rule) : no_id;
        if (!history_ && !rule && capture_slots_.at(capture_id) == no_id) {
            push(configuration, Task{TaskKind::Eval, body});
            enqueue(std::move(configuration));
            return;
        }
        Task close;
        close.kind = TaskKind::CloseGroup;
        close.node = node_id;
        close.occurrence = occurrence;
        close.parent_scope = parent;
        close.prior_calls = prior_calls;
        close.capture = capture_id;
        close.position = configuration.position;
        push(configuration, close);
        push(configuration, Task{TaskKind::Eval, body});
        enqueue(std::move(configuration));
    }

    bool bytes_equal(Span left, Span right) {
        step();
        const auto length = left.end - left.begin;
        if (length != right.end - right.begin) return false;
        step(length);
        return subject_.substr(left.begin, length) == subject_.substr(right.begin, length);
    }
    void close_group(Configuration configuration, const Task& task) {
        ++stats_.up_steps;
        if (history_) {
            const auto& record = configuration.environment->records.at(task.occurrence);
            if (expected_height_ && record.parent == 0 &&
                record.source_id == analysis_.height_equation()->capture_id &&
                record.height != *expected_height_) {
                ++stats_.pruned;
                ++stats_.arithmetic_pruned;
                return;
            }
        }
        const auto slot = capture_slots_.at(task.capture);
        const Span span{task.position, configuration.position};
        if (slot != no_id) {
            const auto obligation = configuration.environment->pending.at(slot);
            if (obligation && !bytes_equal(*obligation, span)) { ++stats_.pruned; return; }
            if (configuration.environment->captures.at(slot) != span || obligation) {
                auto& environment = mutate(configuration);
                environment.captures.at(slot) = span;
                environment.pending.at(slot).reset();
            }
        }
        if (history_) close_record(configuration, task.occurrence);
        configuration.scope = task.parent_scope;
        configuration.calls = task.prior_calls;
        enqueue(std::move(configuration));
    }

    void reference(Configuration configuration, const Node& node) {
        const auto slot = capture_slots_.at(node.capture_id);
        const auto remaining = subject_.size() - configuration.position;
        const auto span = node.forward ? configuration.environment->pending.at(slot) :
                                         configuration.environment->captures.at(slot);
        if (span) {
            const auto length = span->end - span->begin;
            if (length > remaining || !bytes_equal(*span,
                Span{configuration.position, configuration.position + length})) return;
            configuration.position += length;
            enqueue(std::move(configuration));
            return;
        }
        if (!node.forward) return;
        const auto suffix = configuration.continuation != no_id ? frames_.at(configuration.continuation).minimum : 0;
        const auto upper = suffix <= remaining ? remaining - suffix : remaining;
        for (std::size_t length = 0;; ++length) {
            step();
            Configuration next = configuration;
            const auto begin = next.position;
            next.position += length;
            mutate(next).pending.at(slot) = Span{begin, next.position};
            enqueue(std::move(next));
            if (length == upper) break;
        }
    }

    void evaluate(Configuration configuration, NodeId id) {
        ++stats_.down_steps;
        const auto& node = ast_.nodes.at(id);
        if (!analysis_.length(id).productive) { ++stats_.pruned; return; }
        switch (node.kind) {
        case Kind::Empty:
        case Kind::Define: enqueue(std::move(configuration)); break;
        case Kind::CharSet:
            if (configuration.position < subject_.size() &&
                node.characters.test(static_cast<unsigned char>(subject_[configuration.position]))) {
                ++configuration.position;
                enqueue(std::move(configuration));
            }
            break;
        case Kind::Concat:
            for (auto it = node.children.rbegin(); it != node.children.rend(); ++it)
                push(configuration, Task{TaskKind::Eval, *it});
            enqueue(std::move(configuration));
            break;
        case Kind::Alt:
            for (const auto child : node.children) {
                Configuration next = configuration;
                push(next, Task{TaskKind::Eval, child});
                enqueue(std::move(next));
            }
            break;
        case Kind::Capture:
            enter_group(std::move(configuration), id, node.capture_id,
                        node.children.at(0), callable_.at(id));
            break;
        case Kind::Call: {
            const auto& target = ast_.nodes.at(node.call_target);
            if (node.capture_id != 0 && target.kind == Kind::Capture)
                enter_group(std::move(configuration), id, target.capture_id, target.children.at(0), true);
            else enter_group(std::move(configuration), id, 0, node.call_target, true);
            break;
        }
        case Kind::Reference: reference(std::move(configuration), node); break;
        case Kind::Repeat: enter_repeat(std::move(configuration), id); break;
        }
    }

    void start_repeat(Configuration configuration, NodeId id, std::size_t lower,
                      std::optional<std::size_t> upper) {
        const auto& node = ast_.nodes.at(id);
        if (upper && lower > *upper) return;
        const auto& child = analysis_.length(node.children.at(0));
        if (lower && !child.productive) { ++stats_.pruned; return; }
        if (options_.bound_scalar_repetitions && !history_ && child.minimum == 0 &&
            write_only_.at(node.children.at(0)) &&
            (node.bind_count == no_id || count_slots_.at(node.bind_count) == no_id)) {
            // Keep every consuming iteration (at most the remaining bytes),
            // plus the first and last writer of each live capture, plus one
            // epsilon iteration. Removed epsilon iterations can neither read
            // an earlier binding nor alter a retained write. The first writers
            // preserve incoming forward-reference obligations; the last writers
            // preserve outputs. Duplicating a retained epsilon iteration at its
            // original position is idempotent, so every count above this bound
            // has exactly the same scalar transition relation. This contracts
            // whole derivations, not a single unchanged-environment epsilon edge.
            const auto bound = add(subject_.size() - configuration.position,
                                   add(multiply(2, capture_count_), 1));
            if ((upper && *upper > bound) || lower > bound) {
                const auto previous = upper.value_or(lower);
                lower = std::min(lower, bound);
                upper = bound;
                ++stats_.scalar_repetition_clamps;
                stats_.scalar_iteration_bound_reduction = add(
                    stats_.scalar_iteration_bound_reduction, previous - bound);
            }
        }
        Task task;
        task.kind = TaskKind::Repeat;
        task.node = id;
        task.lower = lower;
        task.upper = upper;
        task.parent_scope = configuration.scope;
        if (history_ && node.bind_count != no_id)
            task.occurrence = open_record(configuration, OccurrenceKind::Count, node.bind_count, id);
        push(configuration, task);
        enqueue(std::move(configuration));
    }

    std::optional<std::size_t> scalar_upper(const CountPath& path, const PathCursor& cursor,
                                           const Configuration& configuration) const {
        if (path.metric == OccurrenceMetric::Bytes) return subject_.size();
        if (path.metric != OccurrenceMetric::Count) return {};
        std::size_t count = no_id;
        if (!path.steps.empty() && path.steps.back().source == OccurrenceSource::Count &&
            !path.steps.back().index) count = path.steps.back().id;
        else if (path.steps.empty() && cursor.owner != no_id &&
                 configuration.environment->records.at(cursor.owner).kind == OccurrenceKind::Count)
            count = configuration.environment->records.at(cursor.owner).source_id;
        if (count == no_id || count >= ast_.count_nodes.size()) return {};
        const auto& binder = ast_.nodes.at(ast_.count_nodes.at(count));
        auto upper = binder.upper;
        const auto minimum = analysis_.length(binder.children.at(0)).minimum;
        if (minimum) {
            const auto input_upper = subject_.size() / minimum;
            upper = upper ? std::min(*upper, input_upper) : input_upper;
        }
        return upper;
    }

    void enter_repeat(Configuration configuration, NodeId id) {
        const auto& node = ast_.nodes.at(id);
        if (node.count_path == no_id) {
            if (node.use_count != no_id) {
                const auto count = configuration.environment->counts.at(count_slots_.at(node.use_count));
                if (count) start_repeat(std::move(configuration), id, *count, *count);
            } else start_repeat(std::move(configuration), id, node.lower, node.upper);
            return;
        }
        const auto& path = ast_.count_paths.at(node.count_path);
        PathCursor cursor;
        cursor.path = node.count_path;
        cursor.owner = anchor_scope(configuration.environment->records, path, configuration.scope);
        ++stats_.path_checks;
        const auto lookup = resolve_cursor(configuration.environment->records, path, cursor, [&] { step(); });
        if (lookup.state == LookupState::Missing) { ++stats_.pruned; return; }
        if (lookup.state == LookupState::Ready) {
            start_repeat(std::move(configuration), id, lookup.value, lookup.value);
            return;
        }
        const auto minimum = analysis_.length(node.children.at(0)).minimum;
        std::optional<std::size_t> maximum = scalar_upper(path, cursor, configuration);
        if (minimum) {
            const auto remaining = subject_.size() - configuration.position;
            const auto suffix = configuration.continuation != no_id ? frames_.at(configuration.continuation).minimum : 0;
            const auto allowed = suffix <= remaining ? remaining - suffix : remaining;
            const auto input_maximum = allowed / minimum;
            maximum = maximum ? std::min(*maximum, input_maximum) : input_maximum;
        }
        if (!maximum || *maximum > options_.max_repeat) {
            deferred("forward occurrence value budget left unexplored histories");
            maximum = options_.max_repeat;
        }
        for (std::size_t count = 0;; ++count) {
            step();
            Configuration next = configuration;
            state_units();
            ++stats_.forward_obligations;
            mutate(next).obligations.push_back(Obligation{cursor, count});
            start_repeat(std::move(next), id, count, count);
            if (count == *maximum) break;
        }
    }

    void repeat(Configuration configuration, const Task& task) {
        const auto& node = ast_.nodes.at(task.node);
        if (task.count >= task.lower) {
            Configuration accepted = configuration;
            if (task.occurrence != no_id) {
                close_record(accepted, task.occurrence, task.count);
                mutate(accepted).counts.at(count_slots_.at(node.bind_count)) = task.count;
                accepted.scope = task.parent_scope;
            } else if (node.bind_count != no_id && count_slots_.at(node.bind_count) != no_id) {
                const auto slot = count_slots_.at(node.bind_count);
                if (accepted.environment->counts.at(slot) != task.count)
                    mutate(accepted).counts.at(slot) = task.count;
            }
            ++stats_.up_steps;
            enqueue(std::move(accepted));
        }
        if (task.stop || (task.upper && task.count == *task.upper)) return;
        const auto& body = analysis_.length(node.children.at(0));
        if (!body.productive) { ++stats_.pruned; return; }
        const auto suffix = configuration.continuation != no_id ? frames_.at(configuration.continuation).minimum : 0;
        if (add(body.minimum, suffix) > subject_.size() - configuration.position) {
            ++stats_.pruned;
            return;
        }
        if (task.count >= options_.max_repeat || task.count == no_id) {
            deferred("repetition limit left unexplored histories");
            return;
        }
        Task after = task;
        after.kind = TaskKind::AfterIteration;
        ++after.count;
        after.position = configuration.position;
        after.version = configuration.environment->version;
        if (task.occurrence != no_id)
            open_record(configuration, OccurrenceKind::Iteration, node.bind_count, task.node, false, task.count);
        push(configuration, after);
        push(configuration, Task{TaskKind::Eval, node.children.at(0)});
        enqueue(std::move(configuration));
    }

    void after_iteration(Configuration configuration, Task task) {
        if (task.occurrence != no_id) {
            close_record(configuration, configuration.scope);
            configuration.scope = task.occurrence;
        } else if (pure_.at(ast_.nodes.at(task.node).children.at(0)) &&
                   (ast_.nodes.at(task.node).bind_count == no_id ||
                    count_slots_.at(ast_.nodes.at(task.node).bind_count) == no_id) &&
                   configuration.position == task.position &&
                   configuration.environment->version == task.version) {
            // Only an environment-independent body permits moving an epsilon
            // copy to the end: a later effectful alternative might invalidate
            // today's epsilon branch or change observable occurrence ordinals.
            task.count = std::max(task.count, task.lower);
            task.stop = true;
        }
        task.kind = TaskKind::Repeat;
        push(configuration, task);
        enqueue(std::move(configuration));
    }

    const Ast& ast_;
    const OccurrenceAnalysis& analysis_;
    const std::vector<bool>& callable_;
    std::vector<bool> pure_;
    const std::vector<bool>& write_only_;
    bool whole_rule_;
    std::string_view subject_;
    OccurrenceMatchOptions options_;
    OccurrenceStatistics stats_;
    std::size_t start_ = 0;
    std::size_t version_ = 0;
    std::deque<Configuration> queue_;
    // Integer-linked frames make both execution and destruction independent
    // of native stack depth, including when a large search hits its budget.
    std::vector<Frame> frames_;
    std::string deferred_reason_;
    std::optional<std::size_t> expected_height_;
    bool history_;
    std::vector<std::size_t> capture_slots_, count_slots_;
    std::size_t capture_count_ = 0, count_count_ = 0;
    std::map<std::pair<Task, std::size_t>, std::size_t> continuation_ids_;
    std::set<ScalarKey> seen_;
};

template<class Function>
auto with_statistics(Machine& machine, OccurrenceStatistics* statistics, Function&& function) {
    try {
        auto result = function();
        if (statistics) *statistics = machine.statistics();
        return result;
    } catch (...) {
        if (statistics) *statistics = machine.statistics();
        throw;
    }
}
} // namespace

std::optional<std::size_t> resolve_occurrence_value(const TracedOutcome& trace,
                                                   const CountPath& path,
                                                   std::size_t scope) {
    PathCursor cursor;
    cursor.owner = anchor_scope(trace.occurrences, path, scope);
    const auto value = resolve_cursor(trace.occurrences, path, cursor, [] {});
    if (value.state == LookupState::Ready) return value.value;
    return {};
}

struct OccurrenceProgram::Impl {
    Ast ast;
    std::shared_ptr<const OccurrenceAnalysis> analysis;
    std::vector<bool> callable;
    std::vector<bool> pure;
    std::vector<bool> write_only;
    bool whole_rule = false;
    std::unique_ptr<detail::AstMachineProgram> shared;
    explicit Impl(const Ast& source) : ast(source), callable(ast.nodes.size(), false),
                                       pure(ast.nodes.size(), true),
                                       write_only(ast.nodes.size(), true) {
        if (detail::AstMachineProgram::eligible(ast)) {
            shared = std::make_unique<detail::AstMachineProgram>(ast);
            // The checked AST retains the same nodes and IDs. Its cleared
            // convenience flags cannot remove a height certificate: eligible
            // syntax has no count-path node, which that certificate requires.
            // So history queries can reuse exactly these immutable length facts.
            analysis = shared->shared_analysis();
        } else {
            analysis = std::make_shared<const OccurrenceAnalysis>(ast);
        }
        for (const auto& node : ast.nodes) {
            if (node.kind != Kind::Call) continue;
            if (node.call_target >= ast.nodes.size())
                throw std::invalid_argument("occurrence matcher: invalid recursive call target");
            if (node.capture_id != 0) callable.at(node.call_target) = true;
            whole_rule = whole_rule || node.capture_id == 0;
        }
        // Pure is an effect certificate, independent of optional AST metadata.
        // DEFINE is epsilon even though its nonexecuting declarations contain
        // captures. Propagate actual effects through structural children.
        for (NodeId id = 0; id < ast.nodes.size(); ++id) {
            const auto& node = ast.nodes[id];
            pure[id] = node.kind != Kind::Capture && node.kind != Kind::Call &&
                node.kind != Kind::Reference && node.bind_count == no_id &&
                node.use_count == no_id && node.count_path == no_id;
            write_only[id] = node.kind != Kind::Call && node.kind != Kind::Reference &&
                node.bind_count == no_id && node.use_count == no_id && node.count_path == no_id;
        }
        bool changed;
        do {
            changed = false;
            for (NodeId id = 0; id < ast.nodes.size(); ++id) {
                if (ast.nodes[id].kind == Kind::Define) continue;
                for (const auto child : ast.nodes[id].children) {
                    if (pure[id] && !pure.at(child)) { pure[id] = false; changed = true; }
                    if (write_only[id] && !write_only.at(child)) {
                        write_only[id] = false; changed = true;
                    }
                }
            }
        } while (changed);
    }
};

OccurrenceProgram::OccurrenceProgram(const Ast& ast) : impl_(std::make_shared<Impl>(ast)) {}

bool OccurrenceProgram::supports_shared_calls() const { return bool(impl_->shared); }
detail::SourcePortfolioSnapshot OccurrenceProgram::source_portfolio_snapshot() const {
    return impl_->shared ? impl_->shared->source_portfolio_snapshot() : detail::SourcePortfolioSnapshot{};
}

bool OccurrenceProgram::full_match(std::string_view subject, OccurrenceMatchOptions options,
                                   OccurrenceStatistics* statistics) const {
    if (options.share_calls && impl_->shared)
        return impl_->shared->full_match(subject, options, statistics);
    Machine machine(impl_->ast, *impl_->analysis, impl_->callable, impl_->pure, impl_->write_only, impl_->whole_rule, subject, options, false);
    return with_statistics(machine, statistics, [&] {
        return machine.run(0, true, [](const Configuration&) { return true; });
    });
}

bool OccurrenceProgram::exists_from(std::string_view subject, std::size_t start,
        bool full, OccurrenceMatchOptions options, OccurrenceStatistics* statistics) const {
    if (start == 0 && full) return full_match(subject, options, statistics);
    Machine machine(impl_->ast, *impl_->analysis, impl_->callable, impl_->pure,
                    impl_->write_only, impl_->whole_rule, subject, options, false);
    return with_statistics(machine, statistics, [&] {
        return machine.run(start, full, [](const Configuration&) { return true; });
    });
}

std::optional<Outcome> OccurrenceProgram::first_from(std::string_view subject,
        std::size_t start, bool full, OccurrenceMatchOptions options,
        OccurrenceStatistics* statistics) const {
    if (start == 0 && full) return first_match(subject, options, statistics);
    Machine machine(impl_->ast, *impl_->analysis, impl_->callable, impl_->pure,
                    impl_->write_only, impl_->whole_rule, subject, options);
    return with_statistics(machine, statistics, [&] {
        std::optional<Outcome> result;
        machine.run(start, full, [&](const Configuration& configuration) {
            result = machine.outcome(configuration);
            return true;
        });
        return result;
    });
}

std::vector<std::size_t> OccurrenceProgram::endpoints(std::string_view subject,
        std::size_t start, OccurrenceMatchOptions options, OccurrenceStatistics* statistics) const {
    if (options.share_calls && impl_->shared)
        return impl_->shared->endpoints(subject, start, options, statistics);
    Machine machine(impl_->ast, *impl_->analysis, impl_->callable, impl_->pure, impl_->write_only, impl_->whole_rule, subject, options, false);
    return with_statistics(machine, statistics, [&] {
        std::set<std::size_t> positions;
        machine.run(start, false, [&](const Configuration& configuration) {
            if (!positions.contains(configuration.position)) {
                machine.state_units();
                positions.insert(configuration.position);
            }
            return false;
        });
        machine.state_units(positions.size());
        return std::vector<std::size_t>(positions.begin(), positions.end());
    });
}

std::optional<Outcome> OccurrenceProgram::first_match(std::string_view subject,
        OccurrenceMatchOptions options, OccurrenceStatistics* statistics) const {
    if (options.share_calls && impl_->shared)
        return impl_->shared->first_match(subject, options, statistics);
    Machine machine(impl_->ast, *impl_->analysis, impl_->callable, impl_->pure, impl_->write_only, impl_->whole_rule, subject, options);
    return with_statistics(machine, statistics, [&] {
        std::optional<Outcome> result;
        machine.run(0, true, [&](const Configuration& configuration) {
            result = machine.outcome(configuration);
            return true;
        });
        return result;
    });
}

std::vector<Outcome> OccurrenceProgram::match_outcomes(std::string_view subject,
        std::size_t start, bool full, OccurrenceMatchOptions options,
        OccurrenceStatistics* statistics) const {
    Machine machine(impl_->ast, *impl_->analysis, impl_->callable, impl_->pure, impl_->write_only, impl_->whole_rule, subject, options);
    return with_statistics(machine, statistics, [&] {
        std::set<Outcome> outcomes;
        machine.run(start, full, [&](const Configuration& configuration) {
            outcomes.insert(machine.outcome(configuration));
            return false;
        });
        machine.state_units(outcomes.size());
        return std::vector<Outcome>(outcomes.begin(), outcomes.end());
    });
}

std::optional<TracedOutcome> OccurrenceProgram::first_traced_match(std::string_view subject,
        OccurrenceMatchOptions options, OccurrenceStatistics* statistics) const {
    Machine machine(impl_->ast, *impl_->analysis, impl_->callable, impl_->pure, impl_->write_only, impl_->whole_rule, subject, options, true, true);
    return with_statistics(machine, statistics, [&] {
        std::optional<TracedOutcome> result;
        machine.run(0, true, [&](const Configuration& configuration) {
            result = machine.traced_outcome(configuration);
            return true;
        });
        return result;
    });
}
} // namespace crank
