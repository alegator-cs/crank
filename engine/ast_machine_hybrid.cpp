#include "ast_machine.hpp"
#include "ast_control.hpp"
#include "ast_equivalence.hpp"
#include "regular_regions.hpp"
#include "predictive_regions.hpp"
#include "bounded_word_regions.hpp"
#include "deterministic_program.hpp"
#include "compiled_exact_programs.hpp"
#include "source_view_portfolio.hpp"
#include "exact_end_spines.hpp"
#include "regional_predictive.hpp"
#include "bounded_boundaries.hpp"
#include "occurrence_analysis.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace crank::detail {
namespace {

// Validate actual nodes, including unused definitions: Ast is a public value
// type, and its convenient has_* flags are not a proof of purity or validity.
bool valid_pure_ast(const Ast& ast) {
    if (ast.root >= ast.nodes.size() || ast.capture_names.empty()) return false;
    for (const auto& node : ast.nodes) {
        if (node.kind == Kind::Reference || node.bind_count != no_id ||
            node.use_count != no_id || node.count_path != no_id) return false;
        for (const auto child : node.children)
            if (child >= ast.nodes.size()) return false;
        switch (node.kind) {
        case Kind::Capture:
            if (node.children.size() != 1 || node.capture_id == no_id ||
                node.capture_id >= ast.capture_names.size()) return false;
            break;
        case Kind::Repeat:
            if (node.children.size() != 1 ||
                (node.upper && *node.upper < node.lower)) return false;
            break;
        case Kind::Call:
            if (node.call_target >= ast.nodes.size() ||
                (node.call_target != ast.root &&
                 ast.nodes[node.call_target].kind != Kind::Capture)) return false;
            break;
        case Kind::Empty: case Kind::CharSet: case Kind::Concat:
        case Kind::Alt: case Kind::Define: break;
        case Kind::Reference: return false;
        default: return false;
        }
    }
    return true;
}

Ast checked_ast(const Ast& ast) {
    if (!valid_pure_ast(ast))
        throw std::invalid_argument("shared AST execution requires a valid context-free AST without references or count observations");
    auto result = ast;
    // The analysis needs no occurrence-path certificate here. In particular,
    // forged summary flags must not enable unrelated recursive extraction.
    result.has_references = result.has_counts = result.has_extended = false;
    result.has_occurrence_paths = false;
    return result;
}

void mix(std::size_t& hash, std::size_t value) {
    hash ^= value + static_cast<std::size_t>(0x9e3779b9U) + (hash << 6U) + (hash >> 2U);
}

struct RequestKey {
    NodeId node = 0;
    std::size_t begin = 0;
    // An exact caller demand is semantic input and MUST be part of the key.
    // no_id means enumerate every endpoint; it never aliases a demanded call.
    std::size_t end = no_id;
    // A closed consumer relation is distinct from unrestricted enumeration.
    // Exact demands already carry their entire endpoint observation in end.
    std::size_t consumer_scope = no_id;
    bool operator==(const RequestKey&) const = default;
};
struct RequestHash {
    std::size_t operator()(const RequestKey& key) const {
        auto hash = key.node; mix(hash, key.begin); mix(hash, key.end); mix(hash, key.consumer_scope); return hash;
    }
};
struct Result { std::size_t end = 0, proof = no_id; };
struct FanoutGroup {
    std::size_t begin = 0, end = 0, word = no_id;
    std::uint64_t mask = 0;
};
struct ScheduledWord {
    std::array<std::size_t, 2> ends{no_id, no_id};
    std::array<std::uint64_t, 2> bits{};
};
struct CallerWord { std::size_t word; std::uint64_t bits; };
struct CallerRun { std::size_t begin, end; std::uint64_t bits; };
struct FrozenRuns { bool checked = false; std::size_t block = no_id; std::vector<CallerRun> runs; };
struct PrefixKey {
    std::size_t begin; std::uint64_t first, fill;
    bool operator==(const PrefixKey&) const = default;
};
struct PrefixHash {
    std::size_t operator()(const PrefixKey& k) const {
        auto hash = k.begin; mix(hash, k.first); mix(hash, k.fill); return hash;
    }
};
struct CallerBlock { PrefixKey key; std::size_t last; std::uint64_t tail; std::size_t coverage = no_id; };
struct PrefixCoverage {
    struct Prefix { std::size_t endpoint = no_id, last = 0; std::uint64_t tail = 0; };
    std::array<Prefix, 2> epochs;
};
struct RunCoverage {
    struct Interval { std::size_t endpoint = no_id, begin = 0, end = 0; };
    std::array<Interval, 2> epochs;
};
struct Request {
    RequestKey key;
    std::vector<Result> results;
    std::unordered_set<std::size_t> endpoints;
    std::vector<std::size_t> returns;
    // Consecutive frozen transparent subscribers are grouped by caller word.
    // A group is skipped in one operation if all its completions are known.
    std::size_t fanout = no_id;
    std::size_t boundary_schedule = no_id;
    std::size_t aggregate = no_id;
};

enum class ReturnKind { Pass, Concat, Repeat, EmptyRepeat };
struct ReturnKey {
    std::size_t callee = 0, caller = 0, boundary = 0;
    ReturnKind kind = ReturnKind::Pass;
    bool operator==(const ReturnKey&) const = default;
};
struct ReturnHash {
    std::size_t operator()(const ReturnKey& key) const {
        auto hash = key.callee; mix(hash, key.caller); mix(hash, key.boundary);
        mix(hash, static_cast<std::size_t>(key.kind)); return hash;
    }
};
struct Return { ReturnKey key; std::size_t prefix = no_id; };

// These immutable records form a DAG: every edge points to an older record.
// Zero-width recursive requests therefore cannot create cyclic witness links.
struct Proof {
    enum class Kind { Join, Capture } kind = Kind::Join;
    std::size_t left = no_id, right = no_id, capture = no_id;
    Span span;
};
struct Task {
    enum class Kind { Down, Up, DirectPass, DirectConcat, DirectRepeat, CompleteWord, SuffixWord, Regular } kind = Kind::Down;
    std::size_t id = 0;
    std::size_t end = 0;
    // A materialized Up carries a proof; a direct terminal continuation carries
    // its boundary/count. Direct tasks are never used by witness execution.
    std::size_t payload = no_id;
    // Requests appended before this task must execute their implicit Down first.
    // This also orders next-frontier tasks correctly without a second counter.
    std::size_t request_watermark = 0;
};

struct SuffixKey {
    std::size_t node, boundary, end, word;
    bool operator==(const SuffixKey&) const = default;
};
struct SuffixHash {
    std::size_t operator()(const SuffixKey& k) const {
        auto h = k.node; mix(h, k.boundary); mix(h, k.end); mix(h, k.word); return h;
    }
};
struct SuffixGroup { SuffixKey key; ScheduledWord pending; };
struct RegularCursor {
    std::size_t caller = 0, state = 0, region = no_id;
};

struct DeferredRegionalResult {
    std::size_t endpoint, request;
};
struct DeferredRegionalLater {
    bool operator()(const DeferredRegionalResult& a, const DeferredRegionalResult& b) const {
        return a.endpoint > b.endpoint || (a.endpoint == b.endpoint && a.request > b.request);
    }
};

struct Forward {
    std::size_t target = no_id, hops = 0;
};

// There is no shared mutable native graph. Assistance only reads complete,
// monotone certificates. The false specialization has no assistance storage
// or atomic operations, preserving the ordinary native execution path.
struct RootExcluded {};
template<bool Assisted>
struct AssistanceState {
    AssistanceState(const AtomicExclusions*, NativeAssistanceStatistics*, const DeferredAssistance*) {}
};
template<>
struct AssistanceState<true> {
    const AtomicExclusions* exclusions;
    NativeAssistanceStatistics* statistics;
    const DeferredAssistance* deferred;
    AssistanceState(const AtomicExclusions* bits, NativeAssistanceStatistics* stats,
                    const DeferredAssistance* admission)
        : exclusions(bits), statistics(stats), deferred(admission) {}
};

template<bool Assisted>
class Execution {
    const Ast& ast_;
    const OccurrenceAnalysis& analysis_;
    const std::vector<NodeId>& control_;
    const RegularRegions& regions_;
    const PredictiveRegion& predictive_;
    const BoundedWordRegionCache& bounded_word_;
    const DeterministicProgramCache& exact_program_;
    const CompiledExactPrograms& compiled_;
    const SourceViewPortfolio& source_views_;
    const SourceViewCandidate* forced_candidate_ = nullptr;
    const ExactEndSpines& spines_;
    const PredictiveRimRegion& rims_;
    const RegionalPredictiveCache& regional_;
    const BoundedBoundaryPlans& boundary_plans_;
    std::string_view subject_;
    OccurrenceMatchOptions options_;
    OccurrenceStatistics& stats_;
    [[no_unique_address]] AssistanceState<Assisted> assistance_;
    bool witness_ = false, stop_on_full_ = false;
    bool ordered_ = false;
    // Set before the first charge that a new optional representation changes.
    bool used_frontier_acceleration_ = false;
    bool used_predictive_ = false;
    bool used_bounded_word_ = false, used_exact_program_ = false;
    bool used_normalized_program_ = false, used_source_candidate_ = false;
    bool used_spines_ = false;
    bool used_predictive_rims_ = false;
    bool used_consumer_guards_ = false;
    bool used_regional_ = false, regional_scope_ = false;
    const std::atomic_bool* regional_cancel_ = nullptr;
    std::vector<DeferredRegionalResult> regional_pending_;
    // Fixed, query-local theorem-search admission. Hash collisions only forgo
    // optional proofs; they never share a semantic answer across consumers.
    std::array<unsigned char, 64> endpoint_probe_fuel_ = [] {
        std::array<unsigned char, 64> result{}; result.fill(16); return result;
    }();
    std::size_t position_ = 0;
    std::size_t root_ = no_id;
    // Seed requests occupy this prefix and must survive return compression.
    std::size_t accepting_count_ = 0;
    std::optional<Result> accepted_;
    std::unordered_map<RequestKey, std::size_t, RequestHash> memo_;
    std::vector<Request> requests_;
    std::unordered_map<ReturnKey, std::size_t, ReturnHash> return_memo_;
    std::vector<Return> returns_;
    std::vector<Proof> proofs_;
    std::deque<Task> pending_;
    std::optional<std::deque<Task>> next_;
    std::vector<Forward> forwards_;
    std::vector<std::size_t> forward_path_;
    // Exactly two endpoint positions can be queued. Each word is independently
    // epoch-stamped, so advancing the agenda never clears the whole bitmap.
    std::vector<ScheduledWord> scheduled_;
    std::vector<std::vector<FanoutGroup>> fanouts_;
    std::vector<ScheduledWord> scheduled_boundaries_;
    std::vector<std::vector<CallerWord>> aggregate_returns_;
    std::vector<FrozenRuns> frozen_runs_;
    std::unordered_map<std::uint64_t, RunCoverage> run_coverage_;
    std::vector<CallerBlock> caller_blocks_;
    std::unordered_map<PrefixKey, std::size_t, PrefixHash> prefix_lookup_;
    std::vector<PrefixCoverage> prefix_coverage_;
    std::vector<ScheduledWord> pending_completion_words_;
    std::unordered_map<SuffixKey, std::size_t, SuffixHash> suffix_memo_;
    std::vector<SuffixGroup> suffix_groups_;
    // A compiled region owns one live deterministic cursor. Crossing a byte
    // updates that cursor; it does not allocate another native agenda message.
    std::vector<RegularCursor> regular_current_, regular_next_;
    std::size_t regular_active_ = 0;
    std::size_t down_cursor_ = 0;
    std::size_t implicit_pending() const {
        return ordered_ && options_.implicit_request_agenda
            ? requests_.size() - down_cursor_ : 0;
    }

    [[noreturn]] void limit(const char* message) {
        stats_.limit_reached = true;
        throw ResourceLimit(message);
    }
    void allocate(std::size_t amount = 1) {
        if (amount > options_.max_states - stats_.states)
            limit("shared AST state limit exceeded");
        stats_.states += amount;
    }
    void poll_assistance() {
        if constexpr (Assisted) {
            if (assistance_.deferred && stats_.work_steps >= assistance_.deferred->after_steps) {
                const auto* admission = assistance_.deferred;
                assistance_.deferred = nullptr;
                if (admission->activate)
                    assistance_.exclusions = admission->activate(admission->context, stats_.work_steps);
                if (assistance_.exclusions && assistance_.exclusions->node_count() != ast_.nodes.size())
                    throw std::invalid_argument("deferred exclusion table does not match the AST node count");
            }
            if (!assistance_.exclusions) return;
            ++assistance_.statistics->checks;
            if (assistance_.exclusions->root_rejected()) {
                ++assistance_.statistics->root_rejections;
                throw RootExcluded{};
            }
        }
    }
    bool excluded(NodeId node) {
        if constexpr (Assisted) {
            if (!assistance_.exclusions) return false;
            ++assistance_.statistics->checks;
            return assistance_.exclusions->test(node);
        }
        return false;
    }
    void step(std::size_t amount = 1) {
        // Ordinary native work charges one step. Predictive byte batches poll
        // explicitly before their bounded multi-step charge. Poll inside replay
        // and return-compression loops too, not just between agenda tasks. A stale
        // flag costs work but cannot change the recognition result.
        if constexpr (Assisted)
            if ((stats_.work_steps & 63U) == 0) poll_assistance();
        if (amount > options_.max_steps - stats_.work_steps)
            limit("shared AST work limit exceeded");
        stats_.work_steps += amount;
    }
    void enqueue(Task task) {
        if (ordered_ && options_.implicit_request_agenda) {
            if (task.kind == Task::Kind::Down) {
                const auto queued = pending_.size() + (next_ ? next_->size() : 0) +
                    regular_active_ + implicit_pending() + regional_pending_.size();
                stats_.max_pending = std::max(stats_.max_pending, queued);
                return;
            }
            task.request_watermark = requests_.size();
        }
        allocate();
        ++stats_.native_agenda_tasks;
        if (ordered_) {
            const auto position = task.kind == Task::Kind::Down
                ? requests_[task.id].key.begin : task.end;
            // Only CharSet consumes input, exactly one byte. Finish the entire
            // closure at p before processing p+1, including late subscriptions.
            if (position == position_) pending_.push_back(task);
            else if (position > position_ && position - position_ == 1) {
                if (!next_) next_.emplace();
                next_->push_back(task);
            }
            else throw std::logic_error("AST agenda moved outside its two-position window");
        } else pending_.push_back(task);
        const auto queued = pending_.size() + (next_ ? next_->size() : 0) + regular_active_ + implicit_pending() + regional_pending_.size();
        stats_.max_pending = std::max(stats_.max_pending, queued);
    }
    bool completes_caller(const ReturnKey& key) const {
        return key.kind == ReturnKind::Pass || key.kind == ReturnKind::EmptyRepeat ||
            (key.kind == ReturnKind::Concat &&
             key.boundary == ast_.nodes[requests_[key.caller].key.node].children.size());
    }
    static std::uint64_t& epoch_word(ScheduledWord& scheduled, std::size_t end) {
        const auto slot = end & 1U;
        if (scheduled.ends[slot] != end) {
            scheduled.ends[slot] = end;
            scheduled.bits[slot] = 0;
        }
        return scheduled.bits[slot];
    }
    std::uint64_t& scheduled_word(std::size_t word, std::size_t end) {
        return epoch_word(scheduled_[word], end);
    }
    bool schedule_return(const ReturnKey& key, std::size_t end) {
        if (ordered_ && options_.coalesce_returns) {
            if (completes_caller(key)) {
                auto& seen = scheduled_word(key.caller / 64U, end);
                const auto bit = std::uint64_t{1} << (key.caller & 63U);
                if (seen & bit) { ++stats_.coalesced_returns; return false; }
                seen |= bit;
            } else if (key.boundary < 64 &&
                       (key.kind == ReturnKind::Concat || key.kind == ReturnKind::Repeat)) {
                // A pure continuation consumes only its caller, boundary (or
                // normalized repetition count), and endpoint. The identity of
                // the child derivation that reached it is unobservable.
                // Wide concatenations/counts retain ordinary native handling.
                auto& request = requests_[key.caller];
                if (request.boundary_schedule == no_id) {
                    request.boundary_schedule = scheduled_boundaries_.size();
                    scheduled_boundaries_.push_back({});
                }
                auto& seen = epoch_word(scheduled_boundaries_[request.boundary_schedule], end);
                const auto bit = std::uint64_t{1} << key.boundary;
                if (seen & bit) { ++stats_.coalesced_returns; return false; }
                seen |= bit;
            }
        }
        return true;
    }
    void enqueue_up(std::size_t id, Result result) {
        const auto key = returns_[id].key;
        if (!schedule_return(key, result.end)) return;
        if (ordered_ && options_.aggregate_suffix_returns &&
            !aggregate_returns_.empty() && key.kind == ReturnKind::Concat &&
            !completes_caller(key)) {
            const auto& caller = requests_[key.caller].key;
            const SuffixKey group{caller.node, key.boundary, caller.end, key.caller / 64U};
            auto found = suffix_memo_.find(group);
            if (found == suffix_memo_.end()) {
                allocate();
                const auto id = suffix_groups_.size();
                suffix_groups_.push_back({group, {}});
                ++stats_.suffix_groups_created;
                found = suffix_memo_.emplace(group, id).first;
            }
            auto& pending = epoch_word(suffix_groups_[found->second].pending, result.end);
            if (!pending) enqueue({Task::Kind::SuffixWord, found->second, result.end});
            pending |= std::uint64_t{1} << (key.caller & 63U);
        } else enqueue({Task::Kind::Up, id, result.end, result.proof});
    }
    // A transparent pure consumer observes only (caller, endpoint). Dense
    // callee-origin populations are therefore a set of caller bits, not Return
    // records. Pending masks are delta values: a word has at most one queued
    // task, and another broadcast unions into that task without a message.
    void publish_completion_word(std::size_t word, std::uint64_t mask, std::size_t end) {
        ++stats_.aggregate_word_checks;
        auto& seen = scheduled_word(word, end);
        const auto fresh = mask & ~seen;
        stats_.coalesced_returns += std::popcount(mask & seen);
        if (!fresh) return;
        seen |= fresh;
        // Sparse queries never need this second set of epoch words. Allocate
        // it only when a dense consumer actually publishes a fresh result.
        if (pending_completion_words_.empty())
            pending_completion_words_.resize(scheduled_.size());
        auto& pending = epoch_word(pending_completion_words_[word], end);
        if (!pending) enqueue({Task::Kind::CompleteWord, word, end});
        pending |= fresh;
    }
    bool insert_completion_consumer(std::size_t aggregate, std::size_t caller) {
        auto& words = aggregate_returns_[aggregate];
        const auto word = caller / 64U;
        auto found = std::lower_bound(words.begin(), words.end(), word,
            [](const CallerWord& entry, std::size_t key) { return entry.word < key; });
        const auto bit = std::uint64_t{1} << (caller & 63U);
        if (found != words.end() && found->word == word) {
            if (found->bits & bit) return false;
            found->bits |= bit;
        } else {
            allocate();
            words.insert(found, {word, bit});
            ++stats_.aggregate_return_words;
        }
        return true;
    }
    std::uint64_t insert_consumer_word(std::size_t aggregate, std::size_t word, std::uint64_t bits) {
        auto& words = aggregate_returns_[aggregate];
        auto found = std::lower_bound(words.begin(), words.end(), word,
            [](const CallerWord& entry, std::size_t key) { return entry.word < key; });
        if (found != words.end() && found->word == word) {
            const auto fresh = bits & ~found->bits;
            found->bits |= bits;
            return fresh;
        }
        allocate(); words.insert(found, {word, bits}); ++stats_.aggregate_return_words;
        return bits;
    }
    void promote_consumers(std::size_t callee) {
        auto& request = requests_[callee];
        request.aggregate = aggregate_returns_.size();
        aggregate_returns_.emplace_back();
        if (options_.aggregate_interval_runs) frozen_runs_.emplace_back();
        // Retain the sparse prefix. Migrating already charged Return records
        // would spend new budget before avoiding any new record. Every word
        // inserted from here replaces at least one unmaterialized Return.

    }
    // A certificate says every word in [begin,end) has already published bits
    // at this endpoint. It contains only positive scheduling facts, so stale
    // certificates lose opportunities but cannot invent a result. Keep one
    // interval per mask and endpoint; disjoint publications may replace it.
    void publish_completion_run(const CallerRun& run, std::size_t endpoint) {
        if (run.end - run.begin < 4) {
            for (auto word = run.begin; word < run.end; ++word) {
                step(); publish_completion_word(word, run.bits, endpoint);
            }
            return;
        }
        step(); ++stats_.aggregate_run_checks;
        auto found = run_coverage_.find(run.bits);
        if (found == run_coverage_.end() && run_coverage_.size() < 64) {
            allocate(); ++stats_.aggregate_run_cache_entries;
            found = run_coverage_.try_emplace(run.bits).first;
        }
        const auto publish = [&](std::size_t begin, std::size_t end) {
            for (auto word = begin; word < end; ++word) {
                step(); publish_completion_word(word, run.bits, endpoint);
            }
        };
        if (found == run_coverage_.end()) {
            publish(run.begin, run.end); return;
        }
        auto& coverage = found->second.epochs[endpoint & 1U];
        if (coverage.endpoint != endpoint) coverage = {endpoint, run.begin, run.begin};
        const auto begin = std::max(run.begin, coverage.begin);
        const auto end = std::min(run.end, coverage.end);
        if (begin < end) {
            stats_.aggregate_run_words_skipped += end - begin;
            stats_.coalesced_returns += (end - begin) * std::popcount(run.bits);
            publish(run.begin, begin); publish(end, run.end);
        } else publish(run.begin, run.end);
        // Publish before extending the certificate. No operation above can
        // clear scheduled bits for this endpoint, even if agenda tasks run later.
        if (run.end >= coverage.begin && run.begin <= coverage.end) {
            coverage.begin = std::min(run.begin, coverage.begin);
            coverage.end = std::max(run.end, coverage.end);
        } else coverage = {endpoint, run.begin, run.end};
    }
    // Fixed first word + uniform interior + a partial final word is closed
    // under union when begin/first/fill agree. Its exact union is the farther
    // last word, or the union of tails when the last words agree. A consumer
    // does not observe which frozen row supplied an already-published bit.
    bool publish_completion_block(CallerBlock& block, std::size_t endpoint) {
        step(); ++stats_.aggregate_prefix_checks;
        constexpr auto declined = no_id - 1;
        if (block.coverage == declined) return false;
        if (block.coverage == no_id) {
            auto found = prefix_lookup_.find(block.key);
            if (found == prefix_lookup_.end()) {
                if (prefix_lookup_.size() == 64) { block.coverage = declined; return false; }
                allocate(); ++stats_.aggregate_prefix_cache_entries;
                const auto index = prefix_coverage_.size();
                prefix_coverage_.push_back({});
                found = prefix_lookup_.emplace(block.key, index).first;
            }
            block.coverage = found->second;
        }
        auto& coverage = prefix_coverage_[block.coverage].epochs[endpoint & 1U];
        const auto publish = [&](std::size_t word, std::uint64_t mask) {
            step(); publish_completion_word(word, mask, endpoint);
        };
        if (coverage.endpoint != endpoint) {
            publish(block.key.begin, block.key.first);
            for (auto word = block.key.begin + 1; word < block.last; ++word)
                publish(word, block.key.fill);
            publish(block.last, block.tail);
            coverage = {endpoint, block.last, block.tail};
            return true;
        }
        const auto shared_last = std::min(block.last, coverage.last);
        stats_.aggregate_prefix_words_skipped += shared_last - block.key.begin;
        stats_.coalesced_returns += std::popcount(block.key.first) +
            (shared_last - block.key.begin - 1) * std::popcount(block.key.fill);
        if (block.last < coverage.last ||
            (block.last == coverage.last && !(block.tail & ~coverage.tail))) {
            ++stats_.aggregate_prefix_words_skipped;
            stats_.coalesced_returns += std::popcount(block.tail);
            return true;
        }
        if (block.last == coverage.last) {
            publish(block.last, block.tail);
            coverage.tail |= block.tail;
            return true;
        }
        // The previous final word becomes an interior word. Its previously
        // partial tail must be filled before extending the covered prefix.
        publish(coverage.last, block.key.fill);
        for (auto word = coverage.last + 1; word < block.last; ++word)
            publish(word, block.key.fill);
        publish(block.last, block.tail);
        coverage = {endpoint, block.last, block.tail};
        return true;
    }
    void classify_prefix_block(FrozenRuns& frozen) {
        const auto& runs = frozen.runs;
        if (!options_.aggregate_prefix_blocks || runs.empty() || runs.size() > 3) return;
        const auto fill = std::max_element(runs.begin(), runs.end(), [](const auto& a, const auto& b) {
            return a.end - a.begin < b.end - b.begin;
        });
        const auto begin = runs.front().begin, last = runs.back().end - 1;
        if (fill->begin > begin + 1 || fill->end < last ||
            (runs.back().bits & ~fill->bits)) return;
        // A hole between runs would invalidate the interval interpretation.
        for (std::size_t i = 1; i < runs.size(); ++i)
            if (runs[i - 1].end != runs[i].begin) return;
        allocate(); ++stats_.aggregate_prefix_blocks;
        frozen.block = caller_blocks_.size();
        caller_blocks_.push_back({{begin, runs.front().bits, fill->bits}, last, runs.back().bits});
    }
    const std::vector<CallerRun>& freeze_runs(std::size_t aggregate) {
        auto& frozen = frozen_runs_[aggregate];
        if (frozen.checked) return frozen.runs;
        frozen.checked = true;
        const auto& words = aggregate_returns_[aggregate];
        if (words.size() < 8 || stats_.aggregate_runs >= 65536) return frozen.runs;
        used_frontier_acceleration_ = true;
        std::vector<CallerRun> candidate;
        candidate.reserve(8);
        for (const auto& word : words) {
            step(); ++stats_.aggregate_run_build_words;
            if (!candidate.empty() && candidate.back().end == word.word &&
                candidate.back().bits == word.bits) ++candidate.back().end;
            else {
                if (candidate.size() == 64) return frozen.runs;
                candidate.push_back({word.word, word.word + 1, word.bits});
            }
        }
        // Sparse/irregular rows retain the ordinary word representation. The
        // strict ratio amortizes one frozen scan against repeated broadcasts.
        if (candidate.size() * 4 > words.size() ||
            candidate.size() > 65536 - stats_.aggregate_runs) return frozen.runs;
        allocate(candidate.size());
        stats_.aggregate_runs += candidate.size();
        frozen.runs = std::move(candidate);
        classify_prefix_block(frozen);
        return frozen.runs;
    }
    void broadcast(std::size_t request_id, Result result) {
        auto& request = requests_[request_id];
        const auto count = request.returns.size();
        if (request.aggregate != no_id) {
            const std::vector<CallerRun>* runs = nullptr;
            if (options_.aggregate_interval_runs && request.key.begin < position_) {
                const auto& candidate = freeze_runs(request.aggregate);
                if (!candidate.empty()) runs = &candidate;
            }
            const auto block = runs ? frozen_runs_[request.aggregate].block : no_id;
            if (block != no_id && publish_completion_block(caller_blocks_[block], result.end)) {}
            else if (runs) for (const auto& run : *runs) publish_completion_run(run, result.end);
            else for (const auto& word : aggregate_returns_[request.aggregate]) {
                step();
                publish_completion_word(word.word, word.bits, result.end);
            }
        }
        if (ordered_ && options_.coalesce_returns && request.fanout != no_id) {
            for (const auto& group : fanouts_[request.fanout]) {
                ++stats_.fanout_word_checks;
                if (group.word != no_id &&
                    !(group.mask & ~scheduled_word(group.word, result.end))) {
                    step();
                    stats_.fanout_links_skipped += group.end - group.begin;
                    stats_.coalesced_returns += group.end - group.begin;
                    continue;
                }
                for (auto index = group.begin; index < group.end; ++index) {
                    step(); ++stats_.replay_steps;
                    enqueue_up(request.returns[index], result);
                }
            }
            return;
        }
        // At p > begin, no new subscriber can be created for this request.
        // Build run-length word groups during the first ordinary broadcast;
        // no second pass, sorting, or change in publication order is needed.
        const bool freeze = ordered_ && options_.coalesce_returns &&
            request.key.begin < position_ && count >= 16;
        if (freeze) {
            request.fanout = fanouts_.size();
            fanouts_.emplace_back();
        }
        for (std::size_t index = 0; index < count; ++index) {
            step(); ++stats_.replay_steps;
            const auto id = request.returns[index];
            if (freeze) {
                const auto& key = returns_[id].key;
                const auto word = completes_caller(key) ? key.caller / 64U : no_id;
                const auto mask = word == no_id ? 0 : std::uint64_t{1} << (key.caller & 63U);
                auto& fanout = fanouts_[request.fanout];
                if (fanout.empty() || fanout.back().word != word)
                    fanout.push_back({index, index + 1, word, mask});
                else {
                    fanout.back().end = index + 1;
                    fanout.back().mask |= mask;
                }
            }
            enqueue_up(id, result);
        }
    }
    void reject(bool arithmetic = false) {
        ++stats_.pruned;
        if (arithmetic) ++stats_.arithmetic_pruned;
    }

    std::size_t request(NodeId node, std::size_t begin, std::size_t end = no_id) {
        step();
        if constexpr (Assisted) {
            if (excluded(node)) {
                ++assistance_.statistics->culled_requests;
                reject(); return no_id;
            }
        }
        if (ordered_ && begin != position_)
            throw std::logic_error("AST request created outside the current input position");
        if (!witness_ && options_.collapse_control && control_[node] != node) {
            node = control_[node];
            ++stats_.control_redirects;
            if constexpr (Assisted) {
                if (excluded(node)) {
                    ++assistance_.statistics->culled_requests;
                    reject(); return no_id;
                }
            }
        }
        if (begin > subject_.size() ||
            (end != no_id && (end < begin || end > subject_.size()))) {
            reject(); return no_id;
        }
        const auto& length = analysis_.length(node);
        const auto available = (end == no_id ? subject_.size() : end) - begin;
        if (!length.productive || length.minimum > available) {
            reject(); return no_id;
        }
        if (options_.prune && end != no_id && !length.accepts(available)) {
            reject(true); return no_id;
        }
        // A failed byte condition rules out this whole request before memo or
        // agenda allocation. Nullable unrestricted calls must retain epsilon;
        // their FIRST set cannot reject the request. LAST needs an exact end,
        // never the end of the available subject. Witnesses retain FIFO order.
        if (!witness_ && options_.prune) {
            const auto& conditions = analysis_.byte_conditions(node);
            const auto& boundary = conditions.boundary;
            if (end != no_id) {
                if (available == 0 ? !boundary.nullable :
                    (!boundary.first[static_cast<unsigned char>(subject_[begin])] ||
                     !boundary.last[static_cast<unsigned char>(subject_[end - 1])])) {
                    reject(); return no_id;
                }
            } else if (!boundary.nullable &&
                       (available == 0 ||
                        !boundary.first[static_cast<unsigned char>(subject_[begin])])) {
                reject(); return no_id;
            }
            // Pair fingerprints preserve ordering information that independent
            // FIRST/LAST sets lose. A short unrestricted success must remain
            // possible: only calls with minimum >= 2 can use this prefix guard.
            if (available >= 2 &&
                (end != no_id || length.minimum >= 2)) {
                const auto& pairs = conditions.pairs;
                constexpr auto all_pairs = ~std::uint64_t{0};
                const auto admits = [&](std::uint64_t mask, std::size_t position) {
                    const auto first = static_cast<unsigned char>(subject_[position]);
                    const auto second = static_cast<unsigned char>(subject_[position + 1]);
                    return (mask & (std::uint64_t{1} << ((17U * first + second) & 63U))) != 0;
                };
                if ((pairs.first != all_pairs && !admits(pairs.first, begin)) ||
                    (end != no_id && pairs.last != all_pairs && !admits(pairs.last, end - 2))) {
                    reject(); return no_id;
                }
            }
        }
        const auto scope = regional_scope_ && options_.regional_execution && options_.regional_consumers && end == no_id
            ? regional_.contract_id(node) : no_id;
        const RequestKey key{node, begin, end, scope};
        if (const auto found = memo_.find(key); found != memo_.end()) {
            ++stats_.memo_hits; return found->second;
        }
        allocate();
        const auto id = requests_.size();
        requests_.push_back({key, {}, {}, {}, no_id, no_id});
        if (ordered_) {
            allocate(); forwards_.push_back({});
            if (options_.coalesce_returns && id / 64U == scheduled_.size()) {
                scheduled_.push_back({});
                if (!pending_completion_words_.empty())
                    pending_completion_words_.push_back({});
            }
        }
        memo_.emplace(key, id);
        ++stats_.memo_entries;
        enqueue({Task::Kind::Down, id});
        return id;
    }

    std::size_t join(std::size_t left, std::size_t right) {
        if (!witness_ || right == no_id) return left;
        if (left == no_id) return right;
        allocate(); ++stats_.proof_nodes;
        proofs_.push_back({Proof::Kind::Join, left, right, no_id, {}});
        return proofs_.size() - 1;
    }
    std::size_t capture(std::size_t child, std::size_t slot, Span span) {
        if (!witness_) return no_id;
        allocate(); ++stats_.proof_nodes;
        proofs_.push_back({Proof::Kind::Capture, child, no_id, slot, span});
        return proofs_.size() - 1;
    }

    void subscribe(std::size_t callee, std::size_t caller, ReturnKind kind,
                   std::size_t boundary = 0, std::size_t prefix = no_id,
                   std::size_t extra = no_id) {
        if (callee == no_id) return;
        step();
        if (ordered_ && requests_[callee].key.begin != position_)
            throw std::logic_error("subscriber added after its start-position closure");
        const ReturnKey key{callee, caller, boundary, kind};
        if (return_memo_.contains(key)) return;
        if (!witness_ && ordered_ && options_.coalesce_returns &&
            options_.aggregate_returns && completes_caller(key)) {
            if (requests_[callee].aggregate == no_id && requests_[callee].returns.size() >= 16)
                promote_consumers(callee);
            const auto aggregate = requests_[callee].aggregate;
            if (aggregate != no_id) {
                if (!insert_completion_consumer(aggregate, caller)) return;
                ++stats_.aggregate_return_links;
                for (const auto& result : requests_[callee].results) {
                    step(); ++stats_.replay_steps;
                    publish_completion_word(caller / 64U,
                        std::uint64_t{1} << (caller & 63U), result.end);
                }
                return;
            }
        }
        allocate();
        const auto id = returns_.size();
        const auto proof = join(prefix, extra);
        returns_.push_back({key, proof});
        return_memo_.emplace(key, id);
        requests_[callee].returns.push_back(id);
        ++stats_.return_links;
        // Subscribe before replay. Each old result is replayed once, and each
        // subsequent result sees this link once. Neither direction is optional:
        // epsilon and left-recursive bases may complete before a caller arrives.
        const auto count = requests_[callee].results.size();
        for (std::size_t index = 0; index < count; ++index) {
            step(); ++stats_.replay_steps;
            enqueue_up(id, requests_[callee].results[index]);
        }
    }

    void invoke(NodeId node, std::size_t begin, std::size_t end,
                std::size_t caller, ReturnKind kind, std::size_t boundary = 0,
                std::size_t prefix = no_id, std::size_t extra = no_id) {
        const ReturnKey continuation{no_id, caller, boundary, kind};
        // Direct transitions use the same finite continuation identity as
        // coalesced Up tasks. Wider controls retain the materialized path.
        if (!witness_ && ordered_ && (options_.inline_terminals || options_.inline_nullable_frontiers) &&
            options_.coalesce_returns && (completes_caller(continuation) || boundary < 64)) {
            const auto target = options_.collapse_control ? control_[node] : node;
            const auto& atom = ast_.nodes[target];
            const bool terminal = atom.kind == Kind::CharSet || atom.kind == Kind::Empty || atom.kind == Kind::Define;
            bool epsilon = false;
            if (!terminal && options_.inline_nullable_frontiers && begin <= subject_.size()) {
                const auto& conditions = analysis_.byte_conditions(target).boundary;
                // This is the exact one-element endpoint relation, not greedy
                // preference: no nonempty derivation can begin with this byte.
                // An exact zero-width demand also observes only epsilon.
                epsilon = conditions.nullable && ((end != no_id && end == begin) ||
                    begin == subject_.size() ||
                    !conditions.first[static_cast<unsigned char>(subject_[begin])]);
            }
            if ((terminal && options_.inline_terminals) || epsilon) {
                if (epsilon) used_frontier_acceleration_ = true;
                step();
                if constexpr (Assisted) {
                    if (excluded(node) || (target != node && excluded(target))) {
                        ++assistance_.statistics->culled_requests;
                        reject(); return;
                    }
                }
                if (target != node) ++stats_.control_redirects;
                if (begin > subject_.size()) { reject(); return; }
                auto endpoint = begin;
                if (atom.kind == Kind::CharSet) {
                    if (begin == subject_.size() ||
                        !atom.characters.test(static_cast<unsigned char>(subject_[begin]))) {
                        reject(); return;
                    }
                    endpoint = begin + 1;
                }
                if (end != no_id && end != endpoint) { reject(); return; }
                if (!schedule_return(continuation, endpoint)) return;
                if (epsilon) ++stats_.inline_nullable_transitions;
                else ++stats_.inline_terminal_transitions;
                const auto task_kind = kind == ReturnKind::Concat ? Task::Kind::DirectConcat
                    : kind == ReturnKind::Repeat ? Task::Kind::DirectRepeat : Task::Kind::DirectPass;
                enqueue({task_kind, caller, endpoint, boundary});
                return;
            }
        }
        subscribe(request(node, begin, end), caller, kind, boundary, prefix, extra);
    }

    std::size_t completion_target(std::size_t id) {
        // Future subscribers can only be created at the callee's begin. A
        // CharSet may publish p+1 while still at p, so strict inequality matters.
        if (!ordered_ || id < accepting_count_ || requests_[id].key.begin >= position_) return id;
        constexpr auto resolving = no_id - 1;
        forward_path_.clear();
        auto current = id;
        std::size_t hops = 0;
        for (;;) {
            step(); ++stats_.compression_lookups;
            auto& summary = forwards_[current];
            if (summary.target == resolving) {
                // Nullable unit cycles keep ordinary materialized results and
                // endpoint deduplication. Never chase a cyclic summary forever.
                ++stats_.compression_cycles;
                for (const auto member : forward_path_) forwards_[member] = {member, 0};
                return id;
            }
            if (summary.target != no_id) {
                hops = summary.hops;
                current = summary.target;
                break;
            }
            const auto& request = requests_[current];
            if (current < accepting_count_ || request.aggregate != no_id || request.returns.size() != 1) {
                summary = {current, 0}; break;
            }
            const auto& link = returns_[request.returns.front()].key;
            const auto& caller = requests_[link.caller].key;
            const bool transparent = link.kind == ReturnKind::Pass ||
                (link.kind == ReturnKind::Concat &&
                 link.boundary == ast_.nodes[caller.node].children.size());
            if (!transparent || caller.end != request.key.end ||
                caller.begin > request.key.begin) {
                summary = {current, 0}; break;
            }
            // Pass and a final Concat return preserve the exact-end demand.
            // Their completed prefix plus child is already a caller derivation,
            // so sound length summaries cannot reject the skipped completion.
            // Every ancestor begins no later than this already frozen request.
            summary.target = resolving;
            forward_path_.push_back(current);
            current = link.caller;
        }
        for (auto it = forward_path_.rbegin(); it != forward_path_.rend(); ++it)
            forwards_[*it] = {current, ++hops};
        if (current != id) {
            ++stats_.compressed_completions;
            stats_.compression_hops += forwards_[id].hops;
        }
        return current;
    }

    void complete(std::size_t id, std::size_t end,
                  std::size_t prefix = no_id, std::size_t extra = no_id) {
        step();
        auto key = requests_[id].key;
        if (key.end != no_id && key.end != end) { reject(); return; }
        if (options_.prune && !analysis_.length(key.node).accepts(end - key.begin)) {
            reject(true); return;
        }
        const auto target = completion_target(id);
        if (target != id) {
            id = target;
            key = requests_[id].key;
            if (key.end != no_id && key.end != end) { reject(); return; }
            if (options_.prune && !analysis_.length(key.node).accepts(end - key.begin)) {
                reject(true); return;
            }
        }
        // Most demanded calls have one result. Avoid allocating a hash table
        // for that common case; build it only when a second endpoint arrives.
        const bool ordered_endpoints = ordered_ && options_.coalesce_returns;
        if (ordered_endpoints) {
            ++stats_.ordered_endpoint_checks;
            // Only terminals publish p+1 while closing p, and each terminal
            // request has one endpoint. All other completions arrive at p.
            // Endpoint order is consequently monotone within every request.
            if (!requests_[id].results.empty()) {
                if (requests_[id].results.back().end > end)
                    throw std::logic_error("AST completion endpoints moved backwards");
                if (requests_[id].results.back().end == end) {
                    ++stats_.duplicate_completions; return;
                }
            }
        } else {
            if (requests_[id].results.size() == 1 && requests_[id].results[0].end == end) {
                ++stats_.duplicate_completions; return;
            }
            if (requests_[id].results.size() > 1 && requests_[id].endpoints.contains(end)) {
                ++stats_.duplicate_completions; return;
            }
        }
        // After start closure, subscription/replay can no longer observe an
        // internal request's old endpoints. Ordered deduplication consumes only
        // the latest endpoint. Test the destination AFTER completion forwarding:
        // an otherwise internal completion may have become an exported root.
        const bool reuse_result = options_.compact_closed_results && ordered_endpoints &&
            id >= accepting_count_ && key.begin < position_ && !requests_[id].results.empty();
        if (!reuse_result) allocate();
        auto proof = join(prefix, extra);
        const auto& node = ast_.nodes[key.node];
        if (node.kind == Kind::Capture)
            proof = capture(proof, node.capture_id, {key.begin, end});
        const Result result{end, proof};
        if (!ordered_endpoints) {
            if (requests_[id].results.size() == 1)
                requests_[id].endpoints.insert(requests_[id].results[0].end);
            if (!requests_[id].results.empty()) requests_[id].endpoints.insert(end);
        }
        if (reuse_result) {
            requests_[id].results.back() = result;
            ++stats_.native_reused_result_slots;
        } else {
            requests_[id].results.push_back(result);
            ++stats_.native_result_slots;
        }
        ++stats_.completions;
        if (id < accepting_count_ && stop_on_full_ && end == subject_.size()) {
            accepted_ = result;
            return;
        }
        broadcast(id, result);
    }

    BoundedBoundaryPlans::Result regional_boundary(NodeId node, std::size_t boundary,
                                                   std::size_t begin, std::size_t end) {
        if (!regional_scope_ || !options_.regional_constraints) return {};
        BoundedBoundaryPlans::Counters counts;
        BoundedBoundaryPlans::Settings settings;
        settings.length = options_.regional_length_facts;
        settings.bytes = options_.regional_byte_facts;
        bool admitted = false;
        const auto charge = [&](std::size_t amount) {
            if (!admitted) {
                used_regional_ = true;
                admitted = true;
                ++stats_.regional_constraint_probes;
                allocate(BoundedBoundaryPlans::runtime_state_bound);
            }
            if (regional_cancel_ && (stats_.regional_constraint_steps & 63U) == 0 &&
                regional_cancel_->load(std::memory_order_relaxed)) throw AstExecutionCancelled{};
            step(amount); stats_.regional_constraint_steps += amount;
        };
        const auto result = boundary_plans_.refine(node, boundary, subject_, begin, end,
                                                  settings, counts, charge);
        stats_.regional_constraint_plan_builds += counts.plan_builds;
        stats_.regional_constraint_plan_steps += counts.plan_steps;
        stats_.regional_constraint_refinements += counts.refinements;
        stats_.regional_constraint_visits += counts.visits;
        stats_.regional_constraint_capped += counts.capped;
        stats_.max_regional_boundary_domain = std::max(stats_.max_regional_boundary_domain, counts.peak_domain);
        if (result.kind == BoundedBoundaryPlans::Result::Kind::Impossible)
            ++stats_.regional_constraint_impossible;
        return result;
    }

    void concat(std::size_t caller, std::size_t boundary, std::size_t position,
                std::size_t prefix = no_id, std::size_t extra = no_id) {
        const auto key = requests_[caller].key;
        const auto& node = ast_.nodes[key.node];
        if (boundary == node.children.size()) {
            complete(caller, position, prefix, extra); return;
        }
        const auto available_end = key.end == no_id ? subject_.size() : key.end;
        if (position > available_end ||
            analysis_.suffix_minimum(key.node)[boundary] > available_end - position) {
            reject(); return;
        }
        if (!witness_ && ordered_ && options_.coalesce_returns && options_.local_regular_regions &&
            !regions_.concatenations.empty() && !regions_.concatenations[key.node].empty()) {
            const auto region = regions_.concatenations[key.node][boundary];
            if (region != no_id) {
                ++stats_.regular_region_requests;
                regular_continuation(caller, region, position, 0);
                return;
            }
        }
        auto demand = no_id;
        if (key.end != no_id) {
            const auto& tail = analysis_.suffix_length(key.node, boundary + 1);
            if (tail.productive && tail.modulus == 0) {
                // A singleton tail fixes this child's end, so request() below
                // already checks the equivalent child-length constraint.
                const auto suffix = tail.offset;
                if (suffix > key.end || key.end - suffix < position) { reject(); return; }
                demand = key.end - suffix;
            } else if (!witness_ && options_.prune && boundary != 0 &&
                       !analysis_.suffix_length(key.node, boundary).accepts(key.end - position)) {
                // An exact parent demand constrains the WHOLE remaining
                // suffix, even when both this child and its tail vary.
                // Boundary zero was checked by request(). An unrestricted
                // subject tail is capacity, not a required length. Keep this
                // obligation local; never narrow a shared unrestricted child.
                // Preserve scalar witness discovery order: even a dead caller
                // can create shared work later reused by a live caller.
                reject(true); return;
            }
        }
        if (demand == no_id && key.end != no_id) {
            const auto fact = regional_boundary(key.node, boundary, position, key.end);
            if (fact.kind == BoundedBoundaryPlans::Result::Kind::Impossible) { reject(); return; }
            if (fact.kind == BoundedBoundaryPlans::Result::Kind::Exact) demand = fact.end;
        }
        invoke(node.children[boundary], position, demand, caller,
               ReturnKind::Concat, boundary + 1, prefix, extra);
    }

    void resume_suffix_word(const SuffixKey& group, std::uint64_t bits, std::size_t position) {
        ++stats_.suffix_word_tasks;
        stats_.suffix_members += std::popcount(bits);
        stats_.up_steps += std::popcount(bits);
        const auto representative = group.word * 64U + std::countr_zero(bits);
        const auto key = requests_[representative].key;
        const auto& node = ast_.nodes[key.node];
        const auto boundary = group.boundary;
        const auto child = node.children[boundary];
        const auto target = options_.collapse_control ? control_[child] : child;
        const auto kind = ast_.nodes[target].kind;
        if (std::popcount(bits) < 4 || kind == Kind::CharSet ||
            kind == Kind::Empty || kind == Kind::Define) {
            // A tiny class has no useful relation product. In particular,
            // retain invoke()'s direct terminal transition rather than adding
            // a request cell merely because another region became dense.
            while (bits) {
                const auto caller = group.word * 64U + std::countr_zero(bits);
                bits &= bits - 1;
                concat(caller, boundary, position);
                if (accepted_) break;
            }
            return;
        }
        const auto available_end = key.end == no_id ? subject_.size() : key.end;
        if (position > available_end || analysis_.suffix_minimum(key.node)[boundary] > available_end - position) {
            reject(); return;
        }
        auto demand = no_id;
        if (key.end != no_id) {
            const auto& tail = analysis_.suffix_length(key.node, boundary + 1);
            if (tail.productive && tail.modulus == 0) {
                const auto suffix = tail.offset;
                if (suffix > key.end || key.end - suffix < position) { reject(); return; }
                demand = key.end - suffix;
            } else if (options_.prune && boundary != 0 &&
                       !analysis_.suffix_length(key.node, boundary).accepts(key.end - position)) {
                reject(true); return;
            }
        }
        if (demand == no_id && key.end != no_id) {
            const auto fact = regional_boundary(key.node, boundary, position, key.end);
            if (fact.kind == BoundedBoundaryPlans::Result::Kind::Impossible) { reject(); return; }
            if (fact.kind == BoundedBoundaryPlans::Result::Kind::Exact) demand = fact.end;
        }
        // The whole class asks one child question. Only the destination caller
        // set differs; its origin correlation is carried intact to the return.
        const auto callee = request(node.children[boundary], position, demand);
        if (callee == no_id) return;
        if (boundary + 1 != node.children.size()) {
            while (bits) {
                const auto caller = group.word * 64U + std::countr_zero(bits);
                bits &= bits - 1;
                subscribe(callee, caller, ReturnKind::Concat, boundary + 1);
            }
            return;
        }
        if (requests_[callee].aggregate == no_id) promote_consumers(callee);
        const auto fresh = insert_consumer_word(requests_[callee].aggregate, group.word, bits);
        stats_.aggregate_return_links += std::popcount(fresh);
        if (!fresh) return;
        for (const auto& result : requests_[callee].results) {
            step(); ++stats_.replay_steps;
            publish_completion_word(group.word, fresh, result.end);
        }
    }

    struct RepeatBounds {
        std::size_t lower;
        std::optional<std::size_t> upper;
    };
    RepeatBounds repetition_bounds(const RequestKey& key) const {
        const auto& node = ast_.nodes[key.node];
        RepeatBounds bounds{node.lower, node.upper};
        if (witness_ || (bounds.upper && *bounds.upper == 0) ||
            (!bounds.upper && bounds.lower == 0) ||
            !analysis_.byte_conditions(node.children.front()).boundary.nullable)
            return bounds;
        // For a pure nullable language L, epsilon padding makes L^{lo..hi}
        // equal L^{0..hi}. At most capacity iterations can consume a byte, so
        // an upper bound >= capacity is observationally equivalent to L*.
        // This uses completed CFG nullability, never length.accepts(0) alone.
        bounds.lower = 0;
        const auto capacity = (key.end == no_id ? subject_.size() : key.end) - key.begin;
        if (capacity == 0) bounds.upper = 0;
        else if (bounds.upper && *bounds.upper >= capacity) bounds.upper.reset();
        return bounds;
    }

    void repeat(std::size_t caller, std::size_t count, std::size_t position,
                std::size_t prefix = no_id, std::size_t extra = no_id) {
        const auto key = requests_[caller].key;
        const auto& node = ast_.nodes[key.node];
        const auto body = node.children.front();
        const auto bounds = repetition_bounds(key);
        const auto available_end = key.end == no_id ? subject_.size() : key.end;
        if (position > available_end) { reject(); return; }
        const auto remaining = count < bounds.lower ? bounds.lower - count : 0;
        const auto minimum = analysis_.length(body).minimum;
        if (remaining && (minimum > (available_end - position) / remaining)) {
            reject(); return;
        }
        if (count >= bounds.lower) complete(caller, position, prefix, extra);
        if (accepted_ || (bounds.upper && count == *bounds.upper)) return;
        // Once an unbounded repetition has met its lower bound, its remaining
        // language is independent of the count. This makes nullable loops a
        // finite fixed point without deleting any consuming derivation.
        const auto control_count = bounds.upper ? count : std::min(count, bounds.lower);
        auto demand = no_id;
        if (key.end != no_id && bounds.upper && *bounds.upper == bounds.lower &&
            count < bounds.lower && bounds.lower - count == 1) demand = key.end;
        invoke(body, position, demand, caller, ReturnKind::Repeat, control_count, prefix, extra);
    }

    void queue_regular(RegularCursor cursor, bool fresh) {
        if (!options_.regular_cursor_frontiers) {
            // At most 128 DFA states with 256 columns: opaque offsets fit in
            // 16 bits. Zero region tag means a request-owned subtree cursor.
            const auto tag = cursor.region == no_id ? 0 : cursor.region + 1;
            enqueue({Task::Kind::Regular, cursor.caller, position_ + 1,
                     (tag << 16U) | cursor.state});
            return;
        }
        if (fresh) { allocate(); ++stats_.regular_region_cursors; }
        regular_next_.push_back(cursor);
        ++regular_active_;
        stats_.max_regular_cursors = std::max(stats_.max_regular_cursors, regular_active_);
        stats_.max_pending = std::max(stats_.max_pending,
            pending_.size() + (next_ ? next_->size() : 0) + regular_active_ + implicit_pending() + regional_pending_.size());
    }

    // A local cursor's endpoint is useful only if at least one of its frozen
    // consumers can continue. The guard answers an overapproximation: unknown,
    // repetition controls, dense subscribers and budget exhaustion all admit.
    // No request key or reusable endpoint relation is narrowed for a new caller.
    bool consumer_accepts(std::size_t caller, std::size_t boundary,
                          std::size_t position, std::size_t& remaining) {
        if (!remaining) return true;
        --remaining;
        step(); ++stats_.endpoint_consumer_checks;
        const auto& request = requests_[caller];
        const auto& key = request.key;
        if (boundary != no_id) {
            const auto& children = ast_.nodes[key.node].children;
            for (auto i = boundary; i < children.size(); ++i) {
                if (!remaining) return true;
                --remaining;
                step(); ++stats_.endpoint_consumer_checks;
                const auto& first = analysis_.boundary(children[i]);
                if (position < subject_.size() &&
                    first.first[static_cast<unsigned char>(subject_[position])]) return true;
                if (!first.nullable) return false;
            }
        }
        if (key.end != no_id && key.end != position) return false;
        if (caller < accepting_count_) return true;
        // Late zero-width subscribers must retain replay. All ancestors of a
        // frozen request begin no later than it, but check each visited node.
        if (key.begin >= position_ || request.aggregate != no_id) return true;
        for (const auto return_id : request.returns) {
            const auto& link = returns_[return_id].key;
            if (link.kind == ReturnKind::Repeat) return true;
            const auto suffix = link.kind == ReturnKind::Concat ? link.boundary : no_id;
            if (consumer_accepts(link.caller, suffix, position, remaining)) return true;
        }
        return false;
    }
    bool useful_regular_endpoint(std::size_t caller, std::size_t boundary,
                                 std::size_t position) {
        if (!options_.endpoint_consumer_guards) return true;
        auto hash = requests_[caller].key.node; mix(hash, boundary);
        mix(hash, static_cast<unsigned char>(subject_[position]));
        auto& fuel = endpoint_probe_fuel_[hash % endpoint_probe_fuel_.size()];
        if (options_.endpoint_consumer_admission && fuel == 0) {
            ++stats_.endpoint_consumer_declines;
            return true;
        }
        used_consumer_guards_ = true;
        std::size_t remaining = 64;
        if (consumer_accepts(caller, boundary, position, remaining)) {
            if (fuel) --fuel;
            return true;
        }
        fuel = 16;
        ++stats_.endpoint_consumer_rejections;
        return false;
    }

    void regular_continuation(std::size_t caller, std::size_t region,
                              std::size_t position, std::size_t state, bool fresh = true) {
        const auto key = requests_[caller].key;
        const auto& transform = regions_.continuations[region];
        auto available_end = key.end == no_id ? subject_.size() : key.end;
        auto demand = transform.boundary == no_id ? key.end : no_id;
        if (transform.boundary != no_id) {
            const auto minimum = analysis_.suffix_minimum(key.node)[transform.boundary];
            if (minimum > available_end || position > available_end - minimum) { reject(); return; }
            available_end -= minimum;
            if (key.end != no_id) {
                const auto& tail = analysis_.suffix_length(key.node, transform.boundary);
                if (tail.productive && tail.modulus == 0) demand = key.end - tail.offset;
            }
        }
        const bool may_advance = position < available_end && (demand == no_id || position < demand);
        const auto next = may_advance
            ? transform.program.transition(state, static_cast<unsigned char>(subject_[position])) : no_id;
        const bool continuing = may_advance && !transform.program.dead(next);
        if (transform.program.accepting(state) && (demand == no_id || position == demand) &&
            (!continuing || useful_regular_endpoint(caller, transform.boundary, position))) {
            ++stats_.regular_region_endpoints;
            const ReturnKey continuation{no_id, caller,
                transform.boundary == no_id ? 0 : transform.boundary,
                transform.boundary == no_id ? ReturnKind::Pass : ReturnKind::Concat};
            if (schedule_return(continuation, position))
                enqueue({transform.boundary == no_id ? Task::Kind::DirectPass : Task::Kind::DirectConcat,
                         caller, position, continuation.boundary});
        }
        if (accepted_ || !may_advance) return;
        ++stats_.regular_region_transitions;
        if (continuing) queue_regular({caller, next, region}, fresh);
    }

    void regular_step(std::size_t id, std::size_t position, std::size_t state, bool fresh = true) {
        const auto key = requests_[id].key;
        const auto& program = *regions_.programs[key.node];
        const bool may_advance = position < subject_.size() && (key.end == no_id || position < key.end);
        const auto next = may_advance ? program.transition(state, static_cast<unsigned char>(subject_[position])) : no_id;
        const bool continuing = may_advance && !program.dead(next);
        if (program.accepting(state) && (key.end == no_id || key.end == position) &&
            (!continuing || useful_regular_endpoint(id, no_id, position))) {
            ++stats_.regular_region_endpoints;
            complete(id, position);
        }
        if (accepted_ || !may_advance) return;
        ++stats_.regular_region_transitions;
        if (continuing) queue_regular({id, next, no_id}, fresh);
    }

    void publish_regional(std::size_t id, std::size_t endpoint) {
        if (endpoint < position_ || endpoint > subject_.size())
            throw std::logic_error("regional completion outside its admitted span");
        if (endpoint == position_) {
            ++stats_.regional_published_completions;
            complete(id, endpoint);
            return;
        }
        allocate();
        regional_pending_.push_back({endpoint, id});
        std::push_heap(regional_pending_.begin(), regional_pending_.end(), DeferredRegionalLater{});
        ++stats_.regional_deferred_completions;
        stats_.max_regional_pending = std::max(stats_.max_regional_pending, regional_pending_.size());
        stats_.max_pending = std::max(stats_.max_pending, pending_.size() +
            (next_ ? next_->size() : 0) + regular_active_ + implicit_pending() + regional_pending_.size());
    }

    bool regional_down(std::size_t id) {
        if (!regional_scope_ || !options_.regional_execution || id == root_) return false;
        const auto key = requests_[id].key;
        const auto& kind = ast_.nodes[key.node].kind;
        if (kind == Kind::CharSet || kind == Kind::Empty || kind == Kind::Define) return false;
        const bool exact = key.end != no_id;
        if (!exact && (!options_.regional_consumers || key.consumer_scope == no_id)) return false;
        // Certificates are immutable, shared source objects. Runtime admission
        // retains only the request and, on success, one future boundary effect.
        const auto spine = exact ? regional_.spine(key.node) : nullptr;
        const auto predictor = spine ? nullptr : regional_.certificate(key.node, exact);
        if (!spine && !predictor) { ++stats_.regional_declines; return false; }
        used_regional_ = true; // Set before the first resource charge.
        ++stats_.regional_attempts;
        ++stats_.regional_certificate_uses;
        const auto before = stats_.work_steps;
        std::optional<std::size_t> endpoint;
        try {
            if (spine) {
                ++stats_.regional_spine_attempts;
                const bool matched = regional_cancel_
                    ? execute_spine<true>(*spine, key.begin, key.end, regional_cancel_)
                    : execute_spine<false>(*spine, key.begin, key.end, nullptr);
                if (matched) endpoint = key.end;
            } else {
                ++stats_.regional_predictive_attempts;
                const auto end = exact ? key.end : subject_.size();
                endpoint = regional_cancel_
                    ? execute_predictive<true>(key.begin, regional_cancel_, *predictor, end, exact)
                    : execute_predictive<false>(key.begin, nullptr, *predictor, end, exact);
                if (endpoint && !exact && !regional_.contract(key.node)->accepts(subject_, *endpoint))
                    endpoint.reset();
            }
        } catch (...) {
            stats_.regional_steps += stats_.work_steps - before;
            throw;
        }
        stats_.regional_steps += stats_.work_steps - before;
        if (endpoint) {
            ++stats_.regional_successes;
            publish_regional(id, *endpoint);
        } else ++stats_.regional_failures;
        // Failure is the exact empty admitted relation. Never expand a second
        // producer after an admitted machine has already decided the request.
        return true;
    }

    void down(std::size_t id) {
        ++stats_.down_steps;
        const auto key = requests_[id].key;
        const auto& node = ast_.nodes[key.node];
        if (regional_down(id)) return;
        if (!witness_ && ordered_ && options_.coalesce_returns && options_.local_regular_regions &&
            !regions_.programs.empty() && regions_.programs[key.node]) {
            ++stats_.regular_region_requests;
            regular_step(id, key.begin, 0);
            return;
        }
        switch (node.kind) {
        case Kind::Empty: case Kind::Define:
            complete(id, key.begin); break;
        case Kind::CharSet:
            if (key.begin < subject_.size() &&
                node.characters.test(static_cast<unsigned char>(subject_[key.begin])))
                complete(id, key.begin + 1);
            break;
        case Kind::Concat: concat(id, 0, key.begin); break;
        case Kind::Alt: {
            const auto region = !witness_ && ordered_ && options_.coalesce_returns && options_.local_regular_regions &&
                !regions_.alternations.empty() ? regions_.alternations[key.node] : no_id;
            if (region != no_id) {
                ++stats_.regular_region_requests;
                regular_continuation(id, region, key.begin, 0);
            }
            for (std::size_t child = 0; child < node.children.size(); ++child)
                if (region == no_id || !regions_.alternate_members[key.node][child])
                    invoke(node.children[child], key.begin, key.end, id, ReturnKind::Pass);
            break;
        }
        case Kind::Capture:
            invoke(node.children.front(), key.begin, key.end, id, ReturnKind::Pass);
            break;
        case Kind::Call:
            // The target Capture executes its own close exactly once. Calls do
            // not clone its AST body or add a second capture wrapper.
            invoke(node.call_target, key.begin, key.end, id, ReturnKind::Pass);
            break;
        case Kind::Repeat: {
            const auto& length = analysis_.length(node.children.front());
            if (length.productive && length.modulus == 0 && length.offset == 0) {
                // A zero-width-only body can be repeated at the same boundary
                // arbitrarily often. For an output-only witness, replaying one
                // finite body proof has exactly the final capture effect of
                // replaying it lower times; no count/history is observable.
                if (node.lower == 0 || !witness_) complete(id, key.begin);
                else subscribe(request(node.children.front(), key.begin, key.begin),
                               id, ReturnKind::EmptyRepeat);
            } else repeat(id, 0, key.begin);
            break;
        }
        case Kind::Reference: break; // Rejected at construction.
        }
    }

    void resume(Return link, Result result) {
        ++stats_.up_steps;
        switch (link.key.kind) {
        case ReturnKind::Pass: case ReturnKind::EmptyRepeat:
            complete(link.key.caller, result.end, result.proof); break;
        case ReturnKind::Concat:
            concat(link.key.caller, link.key.boundary, result.end, link.prefix, result.proof);
            break;
        case ReturnKind::Repeat: {
            const auto bounds = repetition_bounds(requests_[link.key.caller].key);
            const auto count = !bounds.upper && link.key.boundary >= bounds.lower
                ? bounds.lower : link.key.boundary + 1;
            repeat(link.key.caller, count, result.end, link.prefix, result.proof);
            break;
        }
        }
    }

    template<bool Cancellable>
    void drain(const std::atomic_bool* cancel) {
        if (ordered_) ++stats_.ordered_positions;
        while ((!pending_.empty() || implicit_pending() || (next_ && !next_->empty()) || !regular_next_.empty() || !regional_pending_.empty()) && !accepted_) {
            if constexpr (Cancellable)
                if (cancel->load(std::memory_order_relaxed)) throw AstExecutionCancelled{};
            if (pending_.empty() && !implicit_pending()) {
                // No request or subscription can be interned at an old start.
                // Do not clear the stable request/return arenas or replay data.
                if (options_.frontier_indexes) {
                    if (!memo_.empty()) memo_.clear();
                    if (!return_memo_.empty()) return_memo_.clear();
                }
                const bool ordinary_next = (next_ && !next_->empty()) || !regular_next_.empty();
                auto next_position = position_ + 1;
                if (!ordinary_next && !regional_pending_.empty()) {
                    next_position = regional_pending_.front().endpoint;
                    if (next_position > position_ + 1) ++stats_.regional_frontier_jumps;
                }
                if (next_position <= position_)
                    throw std::logic_error("regional frontier failed to advance");
                if (next_) pending_.swap(*next_);
                regular_current_.swap(regular_next_);
                position_ = next_position;
                ++stats_.ordered_positions;
                while (!regional_pending_.empty() && regional_pending_.front().endpoint == position_) {
                    if constexpr (Cancellable)
                        if (cancel->load(std::memory_order_relaxed)) throw AstExecutionCancelled{};
                    std::pop_heap(regional_pending_.begin(), regional_pending_.end(), DeferredRegionalLater{});
                    const auto ready = regional_pending_.back(); regional_pending_.pop_back();
                    ++stats_.regional_published_completions;
                    complete(ready.request, ready.endpoint);
                    if (accepted_) break;
                }
                if (accepted_) break;
                for (const auto cursor : regular_current_) {
                    if constexpr (Cancellable)
                        if (cancel->load(std::memory_order_relaxed)) throw AstExecutionCancelled{};
                    --regular_active_;
                    step();
                    if constexpr (Assisted) {
                        if (excluded(requests_[cursor.caller].key.node)) {
                            ++assistance_.statistics->culled_tasks;
                            reject(); continue;
                        }
                    }
                    if (cursor.region == no_id)
                        regular_step(cursor.caller, position_, cursor.state, false);
                    else regular_continuation(cursor.caller, cursor.region, position_, cursor.state, false);
                    if (accepted_) break;
                }
                regular_current_.clear();
                if (accepted_ || (pending_.empty() && !implicit_pending())) continue;
            }
            Task task;
            if (implicit_pending() && (pending_.empty() ||
                down_cursor_ < pending_.front().request_watermark)) {
                task = {Task::Kind::Down, down_cursor_++};
                ++stats_.implicit_down_steps;
            } else {
                task = pending_.front(); pending_.pop_front();
            }
            step();
            if (task.kind == Task::Kind::SuffixWord) {
                auto& group = suffix_groups_[task.id];
                auto& pending = epoch_word(group.pending, task.end);
                auto bits = pending;
                pending = 0;
                const auto key = group.key;
                if constexpr (Assisted) {
                    // The grouping key contains the caller node, so one
                    // exclusion certifies the whole correlated caller set.
                    if (excluded(key.node)) {
                        assistance_.statistics->culled_tasks += std::popcount(bits);
                        reject(); bits = 0;
                    }
                }
                if (bits) resume_suffix_word(key, bits, task.end);
                continue;
            }
            if (task.kind == Task::Kind::CompleteWord) {
                ++stats_.aggregate_word_tasks;
                auto& pending = epoch_word(pending_completion_words_[task.id], task.end);
                auto bits = pending;
                pending = 0;
                while (bits) {
                    const auto caller = task.id * 64U + std::countr_zero(bits);
                    bits &= bits - 1;
                    if constexpr (Assisted) {
                        if (excluded(requests_[caller].key.node)) {
                            ++assistance_.statistics->culled_tasks; reject(); continue;
                        }
                    }
                    ++stats_.up_steps;
                    ++stats_.aggregate_completion_members;
                    complete(caller, task.end);
                    if (accepted_) break;
                }
                continue;
            }
            if constexpr (Assisted) {
                const auto owner = task.kind == Task::Kind::Up
                    ? returns_[task.id].key.caller : task.id;
                if (excluded(requests_[owner].key.node)) {
                    ++assistance_.statistics->culled_tasks;
                    reject(); continue;
                }
            }
            if (task.kind == Task::Kind::Down) down(task.id);
            else if (task.kind == Task::Kind::Up)
                resume(returns_[task.id], {task.end, task.payload});
            else if (task.kind == Task::Kind::Regular) {
                const auto tag = task.payload >> 16U;
                const auto state = task.payload & 0xffffU;
                if (tag == 0) regular_step(task.id, task.end, state);
                else regular_continuation(task.id, tag - 1, task.end, state);
            }
            else {
                const auto kind = task.kind == Task::Kind::DirectConcat ? ReturnKind::Concat
                    : task.kind == Task::Kind::DirectRepeat ? ReturnKind::Repeat : ReturnKind::Pass;
                resume({{no_id, task.id, task.payload, kind}, no_id}, {task.end, no_id});
            }
        }
    }

    template<bool Cancellable>
    bool execute_spine(const ExactEndSpines& certificate, std::size_t left, std::size_t right,
                       const std::atomic_bool* cancel, const Ast* source = nullptr) {
        const auto& source_ast = source ? *source : ast_;
        // Exact-span membership is preserved by each transition. The already
        // checked suffix is never observable again, so neither a return frame
        // nor an endpoint cell is materialized for the recursive interior.
        allocate();
        stats_.max_spine_state_slots = 1;
        std::size_t current = 0;
        const auto tick = [&] {
            if constexpr (Cancellable)
                if ((stats_.spine_steps & 63U) == 0 && cancel->load(std::memory_order_relaxed))
                    throw AstExecutionCancelled{};
            step(); ++stats_.spine_steps;
        };
        const auto matches = [&](const ExactEndSpines::Clause& clause) {
            const auto width = right - left;
            if (width < clause.minimum || (clause.child == no_id && width != clause.width())) return false;
            for (std::size_t i = 0; i < clause.prefix.size(); ++i) {
                tick(); ++stats_.spine_boundary_bytes;
                if (!source_ast.nodes[clause.prefix[i]].characters[static_cast<unsigned char>(subject_[left + i])]) return false;
            }
            for (std::size_t i = 0; i < clause.suffix.size(); ++i) {
                tick(); ++stats_.spine_boundary_bytes;
                if (!source_ast.nodes[clause.suffix[i]].characters[static_cast<unsigned char>(
                        subject_[right - clause.suffix.size() + i])]) return false;
            }
            return true;
        };
        for (;;) {
            tick();
            const auto& state = certificate.states[current];
            const ExactEndSpines::Clause* chosen = nullptr;
            if (state.axis != ExactEndSpines::State::Axis::None) {
                const auto choice = left == right ? state.empty : state.choice[static_cast<unsigned char>(
                    subject_[state.axis == ExactEndSpines::State::Axis::First ? left : right - 1])];
                if (choice != no_id && matches(state.clauses[choice])) chosen = &state.clauses[choice];
            } else {
                for (const auto& clause : state.clauses) {
                    tick();
                    if (matches(clause)) { chosen = &clause; break; }
                }
            }
            if (!chosen) return false;
            if (chosen->child == no_id) {
                return true;
            }
            left += chosen->prefix.size(); right -= chosen->suffix.size();
            current = chosen->child; ++stats_.spine_transitions;
        }
    }

    template<bool Cancellable>
    void run_spine(std::size_t left, const std::atomic_bool* cancel, const SourceViewCandidate* candidate = nullptr) {
        const auto& certificate = candidate ? *candidate->inward() : spines_;
        const auto& source_ast = candidate ? candidate->source_view().ast() : ast_;
        used_spines_ = true;
        ++stats_.spine_attempts;
        const LoweredSpineProgram* lowered = nullptr;
        if constexpr (!Assisted)
            if (options_.compiled_exact_programs && options_.lowered_spines)
                lowered = candidate ? candidate->lowered_inward.get(source_ast, certificate) : compiled_.spine.get(ast_, spines_);
        bool matched;
        if (lowered) {
            ++stats_.lowered_spine_attempts;
            allocate(); stats_.max_spine_state_slots = 1;
            const auto tick = [&] {
                if constexpr (Cancellable)
                    if ((stats_.spine_steps & 63U) == 0 && cancel->load(std::memory_order_relaxed))
                        throw AstExecutionCancelled{};
                step(); ++stats_.spine_steps;
            };
            matched = lowered->execute(subject_, left, subject_.size(), tick,
                stats_.spine_boundary_bytes, stats_.spine_transitions);
        } else matched = execute_spine<Cancellable>(certificate, left, subject_.size(), cancel, &source_ast);
        if (matched) {
            position_ = subject_.size(); complete(root_, position_);
        }
    }

    template<bool Cancellable, class Analysis>
    std::optional<std::size_t> execute_predictive(std::size_t position, const std::atomic_bool* cancel,
                        const PredictiveRegion& certificate, std::size_t end, bool exact,
                        const Ast& source_ast, const Analysis& source_analysis) {
        // Each frame is only an AST continuation: a concatenation boundary or
        // repetition count. No request key, subscriber, endpoint or agenda
        // object exists for the certified interior. The caller supplies a
        // certified exact or scoped-singleton observer; witness queries never
        // enter this executor. Publication belongs to the separate adapter.
        struct Frame { NodeId node; std::size_t next, copies = 1; };
        std::vector<Frame> stack;
        std::size_t depth = 0;
        const auto push = [&](NodeId node, std::size_t next, bool logical_push = true) {
            stats_.predictive_frames += logical_push; ++depth;
            stats_.max_predictive_frames = std::max(stats_.max_predictive_frames, depth);
            if (options_.predictive_continuation_runs && !stack.empty() &&
                stack.back().node == node && stack.back().next == next) {
                ++stack.back().copies;
                return;
            }
            allocate(); ++stats_.predictive_frame_runs;
            stack.push_back({node, next});
            stats_.max_predictive_frame_runs = std::max(stats_.max_predictive_frame_runs, stack.size());
        };
        const auto pop = [&](std::size_t count = 1) {
            depth -= count;
            if (stack.back().copies == count) stack.pop_back();
            else stack.back().copies -= count;
        };
        const auto first = [&](NodeId node) {
            return position < end &&
                source_analysis.boundary(node).first[static_cast<unsigned char>(subject_[position])];
        };
        const auto tick = [&] {
            if constexpr (Cancellable)
                if ((stats_.predictive_steps & 63U) == 0 && cancel->load(std::memory_order_relaxed))
                    throw AstExecutionCancelled{};
            step(); ++stats_.predictive_steps;
        };
        const auto child_count = [&](NodeId id) {
            return id == certificate.entry && certificate.root_child_count != no_id
                ? certificate.root_child_count : source_ast.nodes[id].children.size();
        };
        NodeId current = certificate.entry;
        // A terminal prefix changes only the cursor. Delay creating a return
        // until the first nontrivial child; adjacent equal pending continuations
        // can then be represented by a count, before transient frames arise.
        const auto concat_prefix = [&](NodeId id, std::size_t& boundary) {
            const auto& children = source_ast.nodes[id].children;
            while (boundary < child_count(id)) {
                const auto target = certificate.targets[children[boundary]];
                const auto& terminal = source_ast.nodes[target];
                if (terminal.kind != Kind::CharSet && terminal.kind != Kind::Empty &&
                    terminal.kind != Kind::Define) break;
                tick();
                if (terminal.kind == Kind::CharSet) {
                    if (position == end ||
                        !terminal.characters[static_cast<unsigned char>(subject_[position])]) return false;
                    ++position;
                }
                ++boundary;
            }
            return true;
        };
        for (;;) {
            tick();
            // Transparent source wrappers carry no observable Boolean effect.
            current = certificate.targets[current];
            const auto& node = source_ast.nodes[current];
            switch (node.kind) {
            case Kind::Empty: case Kind::Define: break;
            case Kind::CharSet:
                if (position == end ||
                    !node.characters[static_cast<unsigned char>(subject_[position])]) return std::nullopt;
                ++position; break;
            case Kind::Capture: current = node.children.front(); continue;
            case Kind::Call: current = node.call_target; continue;
            case Kind::Alt: {
                const auto& choice = certificate.choices[certificate.choice_index[current]];
                current = position < end
                    ? choice.byte[static_cast<unsigned char>(subject_[position])] : no_id;
                if (current == no_id) current = choice.empty;
                if (current == no_id) return std::nullopt;
                continue;
            }
            case Kind::Concat: {
                std::size_t boundary = 0;
                if (options_.predictive_continuation_runs && !concat_prefix(current, boundary)) return std::nullopt;
                if (boundary == child_count(current)) break;
                if (boundary + 1 < child_count(current)) push(current, boundary + 1);
                current = node.children[boundary]; continue;
            }
            case Kind::Repeat:
                if (node.upper && *node.upper == 0) break;
                if (!first(node.children.front())) {
                    if (node.lower) return std::nullopt;
                    break;
                }
                push(current, 1); current = node.children.front(); continue;
            case Kind::Reference: throw std::logic_error("stateful node in predictive region");
            }
            bool resumed = false;
            while (!stack.empty()) {
                tick();
                auto& frame = stack.back();
                const auto& parent = source_ast.nodes[frame.node];
                if (parent.kind == Kind::Concat) {
                    if (options_.predictive_continuation_runs && frame.copies > 1 &&
                        frame.next + 1 == child_count(frame.node)) {
                        const auto& terminal = source_ast.nodes[certificate.targets[parent.children[frame.next]]];
                        if (terminal.kind == Kind::CharSet) {
                            // A run of identical one-byte returns denotes the
                            // exact power of this character set. Consume it as
                            // a bounded batch, without individual pop/dispatch.
                            const auto count = std::min<std::size_t>(frame.copies, 64);
                            if (end - position < count) return std::nullopt;
                            if constexpr (Cancellable)
                                if (cancel->load(std::memory_order_relaxed)) throw AstExecutionCancelled{};
                            if constexpr (Assisted) poll_assistance();
                            step(count); stats_.predictive_steps += count;
                            stats_.predictive_terminal_run_bytes += count;
                            for (std::size_t i = 0; i < count; ++i)
                                if (!terminal.characters[static_cast<unsigned char>(subject_[position + i])]) return std::nullopt;
                            position += count; pop(count); continue;
                        }
                    }
                    auto boundary = frame.next;
                    if (options_.predictive_continuation_runs && !concat_prefix(frame.node, boundary)) return std::nullopt;
                    if (boundary == child_count(frame.node)) { pop(); continue; }
                    current = parent.children[boundary++];
                    if (boundary == child_count(frame.node)) pop();
                    else if (frame.copies == 1) frame.next = boundary;
                    else { const auto owner = frame.node; pop(); push(owner, boundary, false); }
                    resumed = true; break;
                }
                if ((!parent.upper || frame.next < *parent.upper) && first(parent.children.front())) {
                    current = parent.children.front();
                    if (frame.copies == 1) ++frame.next;
                    else { const auto owner = frame.node, count = frame.next + 1; pop(); push(owner, count, false); }
                    resumed = true; break;
                }
                if (frame.next < parent.lower) return std::nullopt;
                pop();
            }
            if (resumed) continue;
            return !exact || position == end ? std::optional<std::size_t>{position} : std::nullopt;
        }
    }

    template<bool Cancellable>
    std::optional<std::size_t> execute_predictive(std::size_t position, const std::atomic_bool* cancel,
            const PredictiveRegion& certificate, std::size_t end, bool exact) {
        return execute_predictive<Cancellable>(position, cancel, certificate, end, exact, ast_, analysis_);
    }

    template<bool Cancellable, class Analysis>
    void run_predictive(std::size_t position, const std::atomic_bool* cancel,
                        const PredictiveRegion& certificate, std::size_t end,
                        const Ast& source_ast, const Analysis& source_analysis, const SourceViewCandidate* candidate) {
        used_predictive_ = true;
        ++stats_.predictive_attempts;
        const LoweredPredictiveProgram* lowered = nullptr;
        if constexpr (!Assisted) {
            if (options_.compiled_exact_programs && options_.lowered_predictive) {
                if (candidate)
                    lowered = candidate->lowered_predictive.get(source_ast, source_analysis, certificate);
                else if (&certificate == &predictive_)
                    lowered = compiled_.predictive.get(ast_, analysis_, certificate);
                else if (rims_.certificate && &certificate == &*rims_.certificate)
                    lowered = compiled_.predictive_rim.get(ast_, analysis_, certificate);
            }
        }
        std::optional<std::size_t> endpoint;
        if (lowered) {
            ++stats_.lowered_predictive_attempts;
            LoweredPredictiveProgram::Runtime runtime;
            const auto tick = [&] {
                if constexpr (Cancellable)
                    if ((stats_.predictive_steps & 63U) == 0 && cancel->load(std::memory_order_relaxed))
                        throw AstExecutionCancelled{};
                step(); ++stats_.predictive_steps;
            };
            const auto charge_frame = [&] { allocate(); };
            const auto charge_batch = [&](std::size_t count) {
                if constexpr (Cancellable)
                    if (cancel->load(std::memory_order_relaxed)) throw AstExecutionCancelled{};
                step(count); stats_.predictive_steps += count;
            };
            const auto record = [&] {
                stats_.predictive_frames += runtime.frames;
                stats_.max_predictive_frames = std::max(stats_.max_predictive_frames, runtime.maximum_frames);
                stats_.predictive_frame_runs += runtime.frame_runs;
                stats_.max_predictive_frame_runs = std::max(stats_.max_predictive_frame_runs, runtime.maximum_frame_runs);
                stats_.predictive_terminal_run_bytes += runtime.terminal_run_bytes;
            };
            try {
                endpoint = options_.predictive_observation_reuse
                    ? lowered->execute<false, true>(subject_, position, end, true,
                        options_.predictive_continuation_runs, tick, charge_frame, charge_batch, &runtime)
                    : lowered->execute<false, false>(subject_, position, end, true,
                        options_.predictive_continuation_runs, tick, charge_frame, charge_batch, &runtime);
                record();
            } catch (...) { record(); throw; }
        } else endpoint = execute_predictive<Cancellable>(position, cancel, certificate, end, true, source_ast, source_analysis);
        if (endpoint) {
            position_ = subject_.size(); complete(root_, position_);
        }
    }

    template<bool Cancellable>
    void run_predictive(std::size_t position, const std::atomic_bool* cancel,
            const PredictiveRegion& certificate, std::size_t end) {
        run_predictive<Cancellable>(position, cancel, certificate, end, ast_, analysis_, nullptr);
    }

    template<bool Cancellable>
    void run_predictive_rim(std::size_t position, const std::atomic_bool* cancel) {
        used_predictive_rims_ = true;
        ++stats_.predictive_rim_attempts;
        const auto width = rims_.suffix.size();
        if (width > subject_.size() - position) { step(); return; }
        const auto end = subject_.size() - width;
        for (std::size_t i = 0; i < width; ++i) {
            if constexpr (Cancellable)
                if ((i & 63U) == 0 && cancel->load(std::memory_order_relaxed)) throw AstExecutionCancelled{};
            step(); ++stats_.predictive_rim_bytes;
            if (!ast_.nodes[rims_.suffix[i]].characters[static_cast<unsigned char>(subject_[end + i])]) return;
        }
        run_predictive<Cancellable>(position, cancel, *rims_.certificate, end);
    }

    template<bool Cancellable>
    bool run_bounded_word(std::size_t begin, const std::atomic_bool* cancel, const SourceViewCandidate* candidate = nullptr) {
        const auto* program = candidate ? candidate->bounded_word() : bounded_word_.certificate();
        const auto& source_ast = candidate ? candidate->source_view().ast() : ast_;
        if (!program) return false;
        used_bounded_word_ = true;
        ++stats_.bounded_word_attempts;
        BoundedWordRegion::Runtime runtime;
        const auto tick = [&] {
            if constexpr (Cancellable)
                if ((stats_.bounded_word_steps & 63U) == 0 && cancel->load(std::memory_order_relaxed))
                    throw AstExecutionCancelled{};
            step(); ++stats_.bounded_word_steps;
        };
        const auto charge_frame = [&] { allocate(); ++stats_.bounded_word_frames; };
        const auto record = [&] {
            stats_.bounded_word_max_frames = std::max(stats_.bounded_word_max_frames, runtime.maximum_frames);
        };
        try {
            const LoweredBoundedWordProgram* lowered = nullptr;
            if (options_.compiled_exact_programs && options_.lowered_predictive)
                lowered = candidate ? candidate->lowered_bounded_word.get(source_ast, *program) : compiled_.bounded_word.get(ast_, *program);
            if (lowered) ++stats_.lowered_bounded_word_attempts;
            const auto endpoint = lowered
                ? lowered->execute(subject_, begin, subject_.size(), tick, charge_frame, &runtime)
                : program->execute(source_ast, subject_, begin, subject_.size(), tick, charge_frame, &runtime);
            record();
            if (endpoint) { position_ = *endpoint; complete(root_, position_); }
        } catch (...) { record(); throw; }
        return true;
    }

    template<bool Cancellable>
    bool run_exact_program(std::size_t begin, const std::atomic_bool* cancel, const SourceViewCandidate* candidate = nullptr) {
        const auto* program = candidate ? candidate->slr() : exact_program_.certificate();
        if (!program) return false;
        used_exact_program_ = true;
        ++stats_.exact_program_attempts;
        DeterministicProgram::Runtime runtime;
        const auto tick = [&] {
            if constexpr (Cancellable)
                if ((stats_.exact_program_actions & 63U) == 0 && cancel->load(std::memory_order_relaxed))
                    throw AstExecutionCancelled{};
            step(); ++stats_.exact_program_actions;
        };
        const auto charge_frame = [&] { allocate(); };
        const auto record = [&] {
            stats_.exact_program_shifts += runtime.shifts;
            stats_.exact_program_reductions += runtime.reductions;
            stats_.exact_program_max_stack = std::max(stats_.exact_program_max_stack, runtime.peak_stack);
        };
        try {
            const NativeSlrProgram* native = nullptr;
            if (options_.compiled_exact_programs && options_.native_slr)
                native = candidate ? candidate->native_slr.get(*program) : compiled_.native_slr.get(*program);
            const LoweredDeterministicProgram* lowered = nullptr;
            if (!native && options_.compiled_exact_programs && options_.lowered_slr)
                lowered = candidate ? candidate->lowered_slr.get(*program) : compiled_.slr.get(*program);
            bool matched = false;
            if (native) {
                ++stats_.native_slr_attempts;
                NativeSlrProgram::Meter meter{stats_.work_steps, stats_.states,
                    stats_.exact_program_actions, options_.max_steps, options_.max_states,
                    Cancellable ? cancel : nullptr};
                const auto synchronize = [&] {
                    stats_.work_steps = meter.work; stats_.states = meter.states;
                    stats_.exact_program_actions = meter.actions;
                };
                NativeSlrProgram::Outcome outcome;
                try { outcome = native->execute_metered(subject_, begin, subject_.size(), meter, runtime); }
                catch (...) { synchronize(); throw; }
                synchronize();
                switch (outcome) {
                case NativeSlrProgram::Outcome::match: matched = true; break;
                case NativeSlrProgram::Outcome::reject: break;
                case NativeSlrProgram::Outcome::work_limit: limit("shared AST work limit exceeded");
                case NativeSlrProgram::Outcome::state_limit: limit("shared AST state limit exceeded");
                case NativeSlrProgram::Outcome::cancelled: throw AstExecutionCancelled{};
                default: throw std::logic_error("invalid native SLR result");
                }
            } else {
                if (lowered) ++stats_.lowered_slr_attempts;
                matched = lowered
                    ? lowered->execute(subject_, begin, subject_.size(), tick, charge_frame, runtime)
                    : program->execute(subject_, begin, subject_.size(), tick, charge_frame, runtime);
            }
            record();
            if (matched) { position_ = subject_.size(); complete(root_, position_); }
        } catch (...) { record(); throw; }
        return true;
    }

    template<bool Cancellable>
    bool run_source_candidate(std::size_t begin, const std::atomic_bool* cancel, const SourceViewCandidate& candidate) {
        if (!candidate.ready()) throw std::invalid_argument("source-view candidate not admitted");
        used_source_candidate_ = true;
        const auto id = candidate.statistics().id;
        used_normalized_program_ = id.view != 0;
        stats_.normalized_exact_attempts += id.view != 0;
        stats_.selected_source_view = id.view;
        stats_.selected_source_representation = static_cast<std::size_t>(id.representation);
        switch (id.representation) {
        case SourceRepresentation::Predictive:
            run_predictive<Cancellable>(begin, cancel, *candidate.predictive(), subject_.size(),
                candidate.source_view().ast(), candidate.source_view().analysis(), &candidate); break;
        case SourceRepresentation::BoundedWord: run_bounded_word<Cancellable>(begin, cancel, &candidate); break;
        case SourceRepresentation::Slr: run_exact_program<Cancellable>(begin, cancel, &candidate); break;
        case SourceRepresentation::Inward: run_spine<Cancellable>(begin, cancel, &candidate); break;
        case SourceRepresentation::Regular: {
            // The established DFA transition API supplies the exact machine;
            // this root adapter supplies the ordinary query meter and polling.
            allocate(); std::size_t state = 0;
            for (std::size_t position = begin; position < subject_.size(); ++position) {
                if constexpr (Cancellable)
                    if ((position & 63U) == 0 && cancel->load(std::memory_order_relaxed)) throw AstExecutionCancelled{};
                step(); ++stats_.source_view_regular_steps;
                state = candidate.regular()->transition(state, static_cast<unsigned char>(subject_[position]));
                if (candidate.regular()->dead(state)) return true;
            }
            if (candidate.regular()->accepting(state)) { position_ = subject_.size(); complete(root_, position_); }
            break;
        }
        }
        return true;
    }
    template<bool Cancellable>
    bool run_source_views(std::size_t begin, const std::atomic_bool* cancel, bool prediction_only) {
        // This facility is never entered from an assisted, endpoint, witness,
        // nonroot or multiple-entry demand. Flags are policy, not certificates.
        if constexpr (Assisted) return false;
        const unsigned enabled = unsigned(options_.normalize_repetition_partitions) |
            (unsigned(options_.normalize_transparent_calls) << 1U) |
            (unsigned(options_.normalize_arithmetic_chains) << 2U);
        if (!enabled) return false;
        const bool repetition_prediction = (enabled & 1U) && source_views_.repetition_applicable();
        if (prediction_only && !repetition_prediction && !(enabled & 4U)) return false;
        unsigned considered = 0;
        bool combined_checked = false, skip_combined = false;
        for (const auto id : SourceViewPortfolio::default_schedule()) {
            if (!id.view || (id.view & enabled) != id.view) continue;
            const bool predictor = id.representation == SourceRepresentation::Predictive;
            const bool bounded = id.representation == SourceRepresentation::BoundedWord;
            if (prediction_only && ((id.view != 1 && id.view != 4) || (!predictor && !bounded))) continue;
            if (prediction_only && id.view == 1 && !repetition_prediction) continue;
            // Regular eligibility is offered in exhaustive research only: the
            // existing whole-root DFA pipeline handles original acyclic source.
            if (id.representation == SourceRepresentation::Regular) continue;
            if (predictor && !options_.predictive_recursion) continue;
            if (bounded && !options_.bounded_word_prediction) continue;
            if (id.representation == SourceRepresentation::Inward && !options_.exact_end_spines) continue;
            if (id.representation == SourceRepresentation::Slr && !options_.deterministic_programs) continue;
            if (id.view == 3) {
                if (!combined_checked) {
                    skip_combined = source_views_.skip_redundant_combined();
                    combined_checked = true;
                }
                if (skip_combined) continue;
            }
            considered |= SourceViewPortfolio::candidate_bit(id);
            if (!source_views_.prepare(id)) continue;
            const auto selected = source_views_.select(31, SourceSelectionPolicy::Default, considered);
            if (!selected) throw std::logic_error("admitted source candidate missing from selector");
            return run_source_candidate<Cancellable>(begin, cancel, *source_views_.cached(*selected));
        }
        return false;
    }

    bool used_acceleration() const {
        return used_source_candidate_ || used_bounded_word_ || used_exact_program_ || used_regional_ || used_spines_ || used_predictive_rims_ || used_predictive_ || used_consumer_guards_ || used_frontier_acceleration_ ||
            stats_.regular_region_requests ||
            !aggregate_returns_.empty() || !suffix_groups_.empty();
    }

    void discard_accelerated_attempt() {
        const auto discarded_states = stats_.native_acceleration_discarded_states + stats_.states;
        const auto discarded_steps = stats_.native_acceleration_discarded_steps + stats_.work_steps;
        const auto bounded_word_fallbacks = stats_.native_bounded_word_fallbacks + used_bounded_word_;
        const auto exact_program_fallbacks = stats_.native_exact_program_fallbacks + used_exact_program_;
        const auto normalized_program_fallbacks = stats_.native_normalized_program_fallbacks + used_normalized_program_;
        const auto regional_fallbacks = stats_.native_regional_fallbacks + used_regional_;
        const auto regional_discarded_states = stats_.regional_discarded_states + (used_regional_ ? stats_.states : 0);
        const auto regional_discarded_steps = stats_.regional_discarded_steps + (used_regional_ ? stats_.work_steps : 0);
        const auto spine_fallbacks = stats_.native_spine_fallbacks + (!used_regional_ && used_spines_);
        const auto rim_fallbacks = stats_.native_predictive_rim_fallbacks + (!used_regional_ && !used_spines_ && used_predictive_rims_);
        const auto predictive_fallbacks = stats_.native_predictive_fallbacks +
            (!used_regional_ && !used_spines_ && !used_predictive_rims_ && used_predictive_);
        const auto consumer_fallbacks = stats_.native_consumer_fallbacks +
            (!used_bounded_word_ && !used_exact_program_ && !used_regional_ && !used_spines_ && !used_predictive_rims_ && !used_predictive_ && used_consumer_guards_);
        const auto frontier_fallbacks = stats_.native_frontier_fallbacks +
            (!used_bounded_word_ && !used_exact_program_ && !used_regional_ && !used_spines_ && !used_predictive_rims_ && !used_predictive_ && !used_consumer_guards_ && used_frontier_acceleration_);
        const auto ordinary_fallbacks = stats_.native_ordinary_fallbacks +
            (!used_bounded_word_ && !used_exact_program_ && !used_regional_ && !used_spines_ && !used_predictive_rims_ && !used_predictive_ && !used_consumer_guards_ && !used_frontier_acceleration_);
        // Each optional layer retries its preceding configuration with a fresh
        // allowance. The whole-root predictor never runs local cursor guards;
        // keeping their tiers separate also protects their individual controls.
        if (used_source_candidate_) { options_.normalized_exact_programs = false; forced_candidate_ = nullptr; }
        else if (used_bounded_word_) options_.bounded_word_prediction = false;
        else if (used_exact_program_) {
            options_.deterministic_programs = false;
            // Preserve the preceding fallback contract: a source SLR program
            // that exhausted does not reopen equivalent-source exploration.
            options_.normalized_exact_programs = false;
        }
        else if (used_regional_) {
            options_.regional_execution = false;
            options_.regional_constraints = false;
        }
        else if (used_spines_) options_.exact_end_spines = false;
        else if (used_predictive_rims_) options_.predictive_exact_rims = false;
        else if (used_predictive_) options_.predictive_recursion = false;
        else if (used_consumer_guards_) options_.endpoint_consumer_guards = false;
        else {
            if (!used_frontier_acceleration_) {
                options_.local_regular_regions = false;
                options_.aggregate_returns = false;
                options_.aggregate_suffix_returns = false;
            }
            options_.inline_nullable_frontiers = false;
            options_.aggregate_interval_runs = false;
            options_.aggregate_prefix_blocks = false;
        }
        used_bounded_word_ = used_exact_program_ = false;
        used_normalized_program_ = used_source_candidate_ = false;
        used_regional_ = regional_scope_ = false;
        used_spines_ = used_predictive_rims_ = used_predictive_ = used_consumer_guards_ = used_frontier_acceleration_ = false;
        endpoint_probe_fuel_.fill(16);
        stats_ = {};
        stats_.native_bounded_word_fallbacks = bounded_word_fallbacks;
        stats_.native_exact_program_fallbacks = exact_program_fallbacks;
        stats_.native_normalized_program_fallbacks = normalized_program_fallbacks;
        stats_.native_regional_fallbacks = regional_fallbacks;
        stats_.regional_discarded_states = regional_discarded_states;
        stats_.regional_discarded_steps = regional_discarded_steps;
        stats_.native_spine_fallbacks = spine_fallbacks;
        stats_.native_predictive_rim_fallbacks = rim_fallbacks;
        stats_.native_predictive_fallbacks = predictive_fallbacks;
        stats_.native_consumer_fallbacks = consumer_fallbacks;
        stats_.native_frontier_fallbacks = frontier_fallbacks;
        stats_.native_ordinary_fallbacks = ordinary_fallbacks;
        stats_.native_acceleration_fallbacks = bounded_word_fallbacks + exact_program_fallbacks + regional_fallbacks + spine_fallbacks + rim_fallbacks + predictive_fallbacks + consumer_fallbacks +
            frontier_fallbacks + ordinary_fallbacks;
        stats_.native_acceleration_discarded_states = discarded_states;
        stats_.native_acceleration_discarded_steps = discarded_steps;
        root_ = no_id; accepting_count_ = 0; accepted_.reset();
        decltype(memo_){}.swap(memo_);
        decltype(requests_){}.swap(requests_);
        decltype(return_memo_){}.swap(return_memo_);
        decltype(returns_){}.swap(returns_);
        decltype(proofs_){}.swap(proofs_);
        decltype(pending_){}.swap(pending_);
        next_.reset();
        decltype(forwards_){}.swap(forwards_);
        decltype(forward_path_){}.swap(forward_path_);
        decltype(scheduled_){}.swap(scheduled_);
        decltype(fanouts_){}.swap(fanouts_);
        decltype(scheduled_boundaries_){}.swap(scheduled_boundaries_);
        decltype(aggregate_returns_){}.swap(aggregate_returns_);
        decltype(frozen_runs_){}.swap(frozen_runs_);
        decltype(run_coverage_){}.swap(run_coverage_);
        decltype(caller_blocks_){}.swap(caller_blocks_);
        decltype(prefix_coverage_){}.swap(prefix_coverage_);
        decltype(prefix_lookup_){}.swap(prefix_lookup_);
        decltype(pending_completion_words_){}.swap(pending_completion_words_);
        decltype(suffix_memo_){}.swap(suffix_memo_);
        decltype(suffix_groups_){}.swap(suffix_groups_);
        decltype(regular_current_){}.swap(regular_current_);
        decltype(regular_next_){}.swap(regular_next_);
        regular_active_ = 0;
        decltype(regional_pending_){}.swap(regional_pending_);
        down_cursor_ = 0;
    }
    template<class Function>
    void with_native_fallback(Function&& run) {
        for (;;) {
            try { run(); return; }
            catch (const ResourceLimit&) {
                if (!used_acceleration()) throw;
                // At most ten retries: source-view selection, bounded words, exact programs, regional execution, exact spines,
                // predictive rims, predictor, consumer guards, frontier representations,
                // then aggregate/local summaries. Whole-root and regional tiers
                // are mutually exclusive within an individual attempt.
                // Unused tiers are skipped. Each fresh graph receives the full
                // configured allowance. Discarded charges
                // accumulate separately; sound assistance remains available.
                discard_accelerated_attempt();
            }
        }
    }

public:
    Execution(const Ast& ast, const OccurrenceAnalysis& analysis,
              const std::vector<NodeId>& control, const RegularRegions& regions,
              const PredictiveRegion& predictive, const BoundedWordRegionCache& bounded_word,
              const DeterministicProgramCache& exact_program, const CompiledExactPrograms& compiled, const SourceViewPortfolio& source_views, const ExactEndSpines& spines,
              const PredictiveRimRegion& rims, const RegionalPredictiveCache& regional, const BoundedBoundaryPlans& boundary_plans,
              std::string_view subject, OccurrenceMatchOptions options,
              OccurrenceStatistics& stats, bool witness, bool full,
              const AtomicExclusions* exclusions = nullptr,
              NativeAssistanceStatistics* assistance_statistics = nullptr,
              const DeferredAssistance* deferred = nullptr, const SourceViewCandidate* forced_candidate = nullptr)
        : ast_(ast), analysis_(analysis), control_(control), regions_(regions), predictive_(predictive), bounded_word_(bounded_word), exact_program_(exact_program), compiled_(compiled), source_views_(source_views), forced_candidate_(forced_candidate), spines_(spines), rims_(rims), regional_(regional), boundary_plans_(boundary_plans),
          subject_(subject), options_(options), stats_(stats),
          assistance_(exclusions, assistance_statistics, deferred), witness_(witness),
          stop_on_full_(full), ordered_(options.compress_returns && !witness) {}

    template<bool Cancellable = false>
    void run(std::size_t start, NodeId entry = no_id, const std::atomic_bool* cancel = nullptr) {
        if (start > subject_.size()) throw std::out_of_range("match start exceeds subject size");
        with_native_fallback([&] {
            position_ = start;
            if constexpr (Cancellable)
                if (cancel->load(std::memory_order_relaxed)) throw AstExecutionCancelled{};
            root_ = request(entry == no_id ? ast_.root : entry, start, stop_on_full_ ? subject_.size() : no_id);
            accepting_count_ = requests_.size();
            if (root_ != no_id) {
                if (forced_candidate_) run_source_candidate<Cancellable>(start, cancel, *forced_candidate_);
                else if (!witness_ && stop_on_full_ && (entry == no_id || entry == ast_.root) &&
                    options_.exact_end_spines && spines_.certified)
                    run_spine<Cancellable>(start, cancel);
                else if (!witness_ && stop_on_full_ && (entry == no_id || entry == ast_.root) &&
                    options_.predictive_recursion && predictive_.certified)
                    run_predictive<Cancellable>(start, cancel, predictive_, subject_.size());
                else if (!witness_ && stop_on_full_ && (entry == no_id || entry == ast_.root) &&
                    options_.predictive_exact_rims && rims_.certified)
                    run_predictive_rim<Cancellable>(start, cancel);
                else if (!Assisted && !witness_ && stop_on_full_ && (entry == no_id || entry == ast_.root) &&
                    options_.bounded_word_prediction && run_bounded_word<Cancellable>(start, cancel)) {}
                else if (!Assisted && !witness_ && stop_on_full_ && (entry == no_id || entry == ast_.root) &&
                    options_.normalized_exact_programs && run_source_views<Cancellable>(start, cancel, true)) {}
                else if (!Assisted && !witness_ && stop_on_full_ && (entry == no_id || entry == ast_.root) &&
                    options_.deterministic_programs && run_exact_program<Cancellable>(start, cancel)) {}
                else if (!Assisted && !witness_ && stop_on_full_ && (entry == no_id || entry == ast_.root) &&
                    options_.normalized_exact_programs && run_source_views<Cancellable>(start, cancel, false)) {}
                else {
                    // This fixed observation scope never escapes into endpoint,
                    // witness, multi-entry or nonroot query APIs.
                    regional_scope_ = !Assisted && !witness_ && ordered_ && options_.coalesce_returns &&
                        stop_on_full_ && (entry == no_id || entry == ast_.root) &&
                        (options_.regional_execution || options_.regional_constraints);
                    regional_cancel_ = Cancellable ? cancel : nullptr;
                    drain<Cancellable>(cancel);
                }
            }
        });
    }

    template<bool Cancellable = false>
    void run_any(std::span<const NodeId> entries, const std::atomic_bool* cancel = nullptr) {
        with_native_fallback([&] {
            position_ = 0;
            // All requests seeded before agenda work are accepting entries.
            for (const auto entry : entries) {
                if constexpr (Cancellable)
                    if (cancel->load(std::memory_order_relaxed)) throw AstExecutionCancelled{};
                request(entry, 0, subject_.size());
            }
            accepting_count_ = requests_.size();
            if (accepting_count_) drain<Cancellable>(cancel);
        });
    }

    bool accepted() const { return accepted_.has_value(); }

    std::vector<std::size_t> endpoints() {
        for (;;) {
            try { return export_endpoints(); }
            catch (const ResourceLimit&) {
                if (!used_acceleration()) throw;
                const auto key = requests_[root_].key;
                discard_accelerated_attempt();
                run(key.begin, key.node);
            }
        }
    }

    std::vector<std::size_t> export_endpoints() {
        std::vector<std::size_t> result;
        if (root_ != no_id) {
            allocate(requests_[root_].results.size());
            result.reserve(requests_[root_].results.size());
            for (const auto& completion : requests_[root_].results) {
                step();
                result.push_back(completion.end);
            }
            std::sort(result.begin(), result.end(), [&](auto left, auto right) {
                step(); return left < right;
            });
        }
        return result;
    }

    std::optional<Outcome> outcome() {
        if (!accepted_) return {};
        allocate(ast_.capture_names.size());
        allocate(ast_.count_names.size());
        step(ast_.capture_names.size());
        step(ast_.count_names.size());
        Outcome result{accepted_->end,
            std::vector<std::optional<Span>>(ast_.capture_names.size()),
            std::vector<std::optional<std::size_t>>(ast_.count_names.size())};
        struct Visit { std::size_t proof; bool close; };
        std::vector<Visit> stack;
        const auto push = [&](Visit visit) {
            if (visit.proof != no_id) { allocate(); stack.push_back(visit); }
        };
        push({accepted_->proof, false});
        while (!stack.empty()) {
            step();
            const auto visit = stack.back(); stack.pop_back();
            const auto& proof = proofs_[visit.proof];
            if (proof.kind == Proof::Kind::Join) {
                push({proof.right, false}); push({proof.left, false});
            } else if (visit.close) {
                result.captures[proof.capture] = proof.span;
            } else {
                push({visit.proof, true}); push({proof.left, false});
            }
        }
        result.captures[0] = Span{0, accepted_->end};
        return result;
    }
};

} // namespace

struct AstMachineProgram::Impl {
    Ast ast;
    OccurrenceAnalysis analysis;
    std::vector<NodeId> control;
    EquivalenceStatistics equivalence_statistics;
    std::vector<NodeId> equivalent_control;
    RegularRegions regions;
    PredictiveRegion predictive;
    ExactEndSpines spines;
    PredictiveRimRegion rims;
    BoundedWordRegionCache bounded_word;
    DeterministicProgramCache exact_program;
    CompiledExactPrograms compiled;
    SourceViewPortfolio source_views;
    RegionalPredictiveCache regional;
    BoundedBoundaryPlans boundary_plans;
    explicit Impl(const Ast& source, SourcePortfolioLimits source_limits = {}) : ast(checked_ast(source)), analysis(ast),
        control(pure_control_targets(ast)),
        equivalent_control(pure_ast_equivalent_targets(ast, control, &equivalence_statistics)),
        regions(ast, control, equivalent_control), predictive(ast, analysis), spines(ast, analysis), rims(ast, analysis, predictive.certified), bounded_word(ast, analysis), exact_program(ast, analysis), source_views(ast, SourceViewPortfolio::BorrowedSource{}, source_limits), regional(ast, analysis, control, equivalent_control), boundary_plans(ast, analysis) {}
    const std::vector<NodeId>& execution_control(OccurrenceMatchOptions options) const {
        return options.merge_equivalent_nodes ? equivalent_control : control;
    }
};

AstMachineProgram::AstMachineProgram(const Ast& ast) : impl_(std::make_shared<Impl>(ast)) {}
AstMachineProgram::AstMachineProgram(const Ast& ast, SourcePortfolioLimits limits) : impl_(std::make_shared<Impl>(ast, limits)) {}
bool AstMachineProgram::eligible(const Ast& ast) { return valid_pure_ast(ast); }

std::shared_ptr<const OccurrenceAnalysis> AstMachineProgram::shared_analysis() const {
    // The handle retains the owning Impl; it never borrows a caller's AST.
    return {impl_, &impl_->analysis};
}

const Ast& AstMachineProgram::ast() const { return impl_->ast; }
const std::vector<NodeId>& AstMachineProgram::control_targets() const { return impl_->control; }
const EquivalenceStatistics& AstMachineProgram::equivalence_statistics() const {
    return impl_->equivalence_statistics;
}
const RegularRegionStatistics& AstMachineProgram::regular_region_statistics() const {
    return impl_->regions.statistics;
}
RegionalPreparationStatistics AstMachineProgram::regional_preparation_statistics() const {
    return impl_->regional.statistics();
}
ExactProgramPreparationStatistics AstMachineProgram::exact_program_preparation_statistics() const {
    return impl_->exact_program.statistics();
}
BoundedWordPreparationStatistics AstMachineProgram::bounded_word_preparation_statistics() const {
    return impl_->bounded_word.statistics();
}
CompiledProgramPreparationStatistics AstMachineProgram::compiled_program_preparation_statistics() const {
    auto result = impl_->compiled.statistics();
    const auto snapshot = impl_->source_views.snapshot();
    for (unsigned view = 0; view < 4; ++view) {
        const auto& slr = snapshot.views[view].candidates[static_cast<unsigned>(SourceRepresentation::Slr)];
        result.normalized_slr[view] = slr.lowered_slr;
        result.normalized_native_slr[view] = slr.native_slr;
    }
    return result;
}
std::array<NormalizedProgramPreparationStatistics, 4> AstMachineProgram::normalized_program_preparation_statistics() const {
    std::array<NormalizedProgramPreparationStatistics, 4> result;
    const auto snapshot = impl_->source_views.snapshot();
    for (unsigned view = 0; view < 4; ++view) {
        const auto& source = snapshot.views[view].preparation; auto& to = result[view];
        to.attempted = source.attempted; to.limited = source.limited; to.changed = source.changed;
        to.nullable_complete = source.nullable_complete;
        to.normalization_steps = source.normalization_steps; to.nullable_steps = source.nullable_steps;
        to.repetition_rewrites = source.repetition_rewrites; to.redirected_calls = source.redirected_calls;
        to.cloned_nodes = source.cloned_nodes; to.source_nodes = source.source_nodes;
        to.normalized_nodes = source.normalized_nodes; to.retained_bytes = source.retained_bytes;
        if (const auto* candidate = impl_->source_views.cached({view, SourceRepresentation::Slr})) {
            to.slr = candidate->slr()->statistics(); to.certified = to.slr.certified;
        }
    }
    return result;
}

std::shared_ptr<const SourceViewPortfolio> AstMachineProgram::shared_source_portfolio() const {
    return {impl_, &impl_->source_views};
}
SourcePortfolioSnapshot AstMachineProgram::source_portfolio_snapshot() const { return impl_->source_views.snapshot(); }
bool AstMachineProgram::full_match_candidate(SourceCandidateId id, std::string_view subject,
        OccurrenceMatchOptions options, OccurrenceStatistics* statistics, const std::atomic_bool* cancel) const {
    const auto* candidate = impl_->source_views.prepare(id);
    if (!candidate) throw std::invalid_argument("forced source-view candidate is unavailable");
    OccurrenceStatistics local; auto& stats = statistics ? *statistics : local; stats = {};
    Execution<false> execution(impl_->ast, impl_->analysis, impl_->execution_control(options), impl_->regions,
        impl_->predictive, impl_->bounded_word, impl_->exact_program, impl_->compiled, impl_->source_views,
        impl_->spines, impl_->rims, impl_->regional, impl_->boundary_plans, subject, options, stats, false, true,
        nullptr, nullptr, nullptr, candidate);
    if (cancel) execution.run<true>(0, no_id, cancel); else execution.run(0);
    return execution.accepted();
}

bool AstMachineProgram::full_match_node(NodeId node, std::string_view subject,
    OccurrenceMatchOptions options, OccurrenceStatistics* statistics,
    const std::atomic_bool* cancel) const {
    if (node >= impl_->ast.nodes.size()) throw std::out_of_range("invalid AST task entry");
    OccurrenceStatistics local;
    auto& stats = statistics ? *statistics : local;
    stats = {};
    Execution<false> execution(impl_->ast, impl_->analysis, impl_->execution_control(options), impl_->regions, impl_->predictive, impl_->bounded_word, impl_->exact_program, impl_->compiled, impl_->source_views, impl_->spines, impl_->rims, impl_->regional, impl_->boundary_plans,
                        subject, options, stats, false, true);
    if (cancel) execution.run<true>(0, node, cancel);
    else execution.run(0, node);
    return execution.accepted();
}

bool AstMachineProgram::full_match_any(std::span<const NodeId> entries, std::string_view subject,
    OccurrenceMatchOptions options, OccurrenceStatistics* statistics,
    const std::atomic_bool* cancel) const {
    for (const auto entry : entries)
        if (entry >= impl_->ast.nodes.size()) throw std::out_of_range("invalid AST task entry");
    OccurrenceStatistics local;
    auto& stats = statistics ? *statistics : local;
    stats = {};
    if (entries.empty()) return false;
    Execution<false> execution(impl_->ast, impl_->analysis, impl_->execution_control(options), impl_->regions, impl_->predictive, impl_->bounded_word, impl_->exact_program, impl_->compiled, impl_->source_views, impl_->spines, impl_->rims, impl_->regional, impl_->boundary_plans,
                        subject, options, stats, false, true);
    if (cancel) execution.run_any<true>(entries, cancel);
    else execution.run_any(entries);
    return execution.accepted();
}

bool AstMachineProgram::full_match(std::string_view subject, OccurrenceMatchOptions options,
                                  OccurrenceStatistics* statistics) const {
    OccurrenceStatistics local;
    auto& stats = statistics ? *statistics : local;
    stats = {};
    Execution<false> execution(impl_->ast, impl_->analysis,
                        impl_->execution_control(options), impl_->regions, impl_->predictive, impl_->bounded_word, impl_->exact_program, impl_->compiled, impl_->source_views, impl_->spines, impl_->rims, impl_->regional, impl_->boundary_plans, subject, options, stats, false, true);
    execution.run(0);
    return execution.accepted();
}

bool AstMachineProgram::full_match_assisted(std::string_view subject,
    OccurrenceMatchOptions options, const AtomicExclusions& exclusions,
    OccurrenceStatistics* statistics, NativeAssistanceStatistics* assistance_statistics) const {
    if (exclusions.node_count() != impl_->ast.nodes.size())
        throw std::invalid_argument("exclusion table does not match the AST node count");
    OccurrenceStatistics local;
    auto& stats = statistics ? *statistics : local;
    stats = {};
    NativeAssistanceStatistics local_assistance;
    auto& assistance = assistance_statistics ? *assistance_statistics : local_assistance;
    assistance = {};
    try {
        Execution<true> execution(impl_->ast, impl_->analysis, impl_->execution_control(options), impl_->regions, impl_->predictive, impl_->bounded_word, impl_->exact_program, impl_->compiled, impl_->source_views, impl_->spines, impl_->rims, impl_->regional, impl_->boundary_plans,
                                 subject, options, stats, false, true,
                                 &exclusions, &assistance);
        execution.run(0);
        return execution.accepted();
    } catch (const RootExcluded&) {
        // A complete query-level impossibility proof is a genuine rejection.
        // Ordinary work/state exhaustion retains its ResourceLimit outcome.
        return false;
    }
}

bool AstMachineProgram::full_match_deferred(std::string_view subject,
    OccurrenceMatchOptions options, const DeferredAssistance& deferred,
    OccurrenceStatistics* statistics, NativeAssistanceStatistics* assistance_statistics) const {
    OccurrenceStatistics local;
    auto& stats = statistics ? *statistics : local;
    stats = {};
    NativeAssistanceStatistics local_assistance;
    auto& assistance = assistance_statistics ? *assistance_statistics : local_assistance;
    assistance = {};
    try {
        Execution<true> execution(impl_->ast, impl_->analysis, impl_->execution_control(options), impl_->regions, impl_->predictive, impl_->bounded_word, impl_->exact_program, impl_->compiled, impl_->source_views, impl_->spines, impl_->rims, impl_->regional, impl_->boundary_plans,
                                 subject, options, stats, false, true,
                                 nullptr, &assistance, &deferred);
        execution.run(0);
        return execution.accepted();
    } catch (const RootExcluded&) {
        return false;
    }
}

std::vector<std::size_t> AstMachineProgram::endpoints(std::string_view subject,
    std::size_t start, OccurrenceMatchOptions options, OccurrenceStatistics* statistics) const {
    OccurrenceStatistics local;
    auto& stats = statistics ? *statistics : local;
    stats = {};
    Execution<false> execution(impl_->ast, impl_->analysis,
                        impl_->execution_control(options), impl_->regions, impl_->predictive, impl_->bounded_word, impl_->exact_program, impl_->compiled, impl_->source_views, impl_->spines, impl_->rims, impl_->regional, impl_->boundary_plans, subject, options, stats, false, false);
    execution.run(start);
    return execution.endpoints();
}

std::optional<Outcome> AstMachineProgram::first_match(std::string_view subject,
    OccurrenceMatchOptions options, OccurrenceStatistics* statistics) const {
    OccurrenceStatistics local;
    auto& stats = statistics ? *statistics : local;
    stats = {};
    Execution<false> execution(impl_->ast, impl_->analysis,
                        impl_->control, impl_->regions, impl_->predictive, impl_->bounded_word, impl_->exact_program, impl_->compiled, impl_->source_views, impl_->spines, impl_->rims, impl_->regional, impl_->boundary_plans, subject, options, stats, true, true);
    execution.run(0);
    return execution.outcome();
}

} // namespace crank::detail
