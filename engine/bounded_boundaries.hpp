#pragma once
#include "occurrence_analysis.hpp"
#include <algorithm>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>

namespace crank::detail {

// Necessary facts only. The caller must still recognize the original children.
// Plans are source-bounded and built lazily; dynamic positions are query-local.
class BoundedBoundaryPlans {
    using Values = std::vector<std::size_t>;
    using Domain = std::optional<Values>;
    struct Plan { std::vector<NodeId> children; std::vector<Domain> widths; };
    const Ast& ast_;
    const OccurrenceAnalysis& analysis_;
    mutable std::mutex mutex_;
    mutable std::map<std::pair<NodeId, std::size_t>, std::shared_ptr<const Plan>> cache_;
    mutable std::size_t retained_bytes_ = 0;
    static constexpr std::size_t max_widths = 16, max_children = 32;
    static constexpr std::size_t max_nodes = 4096, max_depth = 96, max_compile_work = 32768;
    static void normalize(Values& values) {
        std::sort(values.begin(), values.end());
        values.erase(std::unique(values.begin(), values.end()), values.end());
    }
public:
    // Conservative positional-storage allowance for one admitted refinement:
    // 33 domains of at most 16 values, two 256-value temporary support lists,
    // intersection storage and bounded worklist/record capacity. Candidate
    // examinations are separately charged as work, not new native requests.
    static constexpr std::size_t runtime_state_bound = 2048;
    struct Settings {
        bool finite = true, length = true, bytes = true;
        std::size_t max_checks = 4096, max_visits = 128;
    };
    struct Counters {
        std::size_t plan_builds = 0, plan_steps = 0;
        std::size_t visits = 0, checks = 0, refinements = 0, peak_domain = 0;
        bool capped = false;
    };
    struct Result {
        enum class Kind { Unknown, Impossible, Exact } kind = Kind::Unknown;
        std::size_t end = no_id;
    };
    BoundedBoundaryPlans(const Ast& ast, const OccurrenceAnalysis& analysis)
        : ast_(ast), analysis_(analysis) {}

private:
    std::shared_ptr<const Plan> plan(NodeId concat, std::size_t boundary, Counters& counts) const {
        if (concat >= ast_.nodes.size() || ast_.nodes.size() > max_nodes) return {};
        const auto& node = ast_.nodes[concat];
        if (node.kind != Kind::Concat || boundary >= node.children.size() ||
            node.children.size() - boundary < 2 || node.children.size() - boundary > max_children)
            return {};
        // Single-width producers already have one end. Avoid this machinery on
        // ordinary terminals and fixed lexical fields.
        if (analysis_.length(node.children[boundary]).modulus == 0) return {};
        const auto key = std::make_pair(concat, boundary);
        std::lock_guard lock(mutex_);
        if (const auto found = cache_.find(key); found != cache_.end()) return found->second;
        if (cache_.size() >= 128) { counts.capped = true; return {}; }
        ++counts.plan_builds;
        std::size_t work = 0;
        const auto charge = [&]() {
            if (work == max_compile_work) { counts.capped = true; return false; }
            ++work; ++counts.plan_steps; return true;
        };
        // All preparation allocations are optional, including memo tables and
        // the cache record itself. Failure declines this refinement.
        try {
        std::vector<Domain> memo(ast_.nodes.size());
        std::vector<unsigned char> active(ast_.nodes.size()), done(ast_.nodes.size());
        const auto product = [&](const Domain& a, const Domain& b) -> Domain {
            if (!a || !b) return std::nullopt;
            Values result;
            for (const auto x : *a) for (const auto y : *b) {
                if (!charge() || y > no_id - x) return std::nullopt;
                result.push_back(x + y);
            }
            normalize(result);
            if (result.size() > max_widths) return std::nullopt;
            return result;
        };
        std::function<Domain(NodeId, std::size_t)> finite;
        finite = [&](NodeId id, std::size_t depth) -> Domain {
            if (!charge() || depth >= max_depth) return std::nullopt;
            if (done[id]) return memo[id];
            const auto& summary = analysis_.length(id);
            if (!summary.productive) return Values{};
            if (summary.modulus == 0) return Values{summary.offset};
            if (active[id]) return std::nullopt;
            active[id] = 1;
            const auto& n = ast_.nodes[id];
            Domain result;
            switch (n.kind) {
            case Kind::Empty: case Kind::Define: result = Values{0}; break;
            case Kind::CharSet: result = n.characters.any() ? Values{1} : Values{}; break;
            case Kind::Call: result = finite(n.call_target, depth + 1); break;
            case Kind::Capture: result = finite(n.children.front(), depth + 1); break;
            case Kind::Concat:
                result = Values{0};
                for (const auto child : n.children) {
                    result = product(result, finite(child, depth + 1));
                    if (!result) break;
                }
                break;
            case Kind::Alt: {
                Values joined;
                bool complete = true;
                for (const auto child : n.children) {
                    const auto part = finite(child, depth + 1);
                    if (!part) { complete = false; break; }
                    joined.insert(joined.end(), part->begin(), part->end());
                    normalize(joined);
                    if (joined.size() > max_widths) { complete = false; break; }
                }
                if (complete) result = std::move(joined);
                break;
            }
            case Kind::Repeat: {
                if (!n.upper || *n.upper > max_widths) break;
                const auto body = finite(n.children.front(), depth + 1);
                Domain power = Values{0}; Values joined; bool complete = true;
                for (std::size_t count = 0; count <= *n.upper; ++count) {
                    if (count >= n.lower) {
                        if (!power) { complete = false; break; }
                        joined.insert(joined.end(), power->begin(), power->end());
                        normalize(joined);
                        if (joined.size() > max_widths) { complete = false; break; }
                    }
                    if (count < *n.upper) power = product(power, body);
                }
                if (complete) result = std::move(joined);
                break;
            }
            case Kind::Reference: break; // Owner's checked pure AST excludes this.
            }
            active[id] = 0; done[id] = 1; memo[id] = result; return result;
        };
        std::shared_ptr<Plan> result;
        std::size_t admitted_bytes = 0;
            result = std::make_shared<Plan>();
            result->children.assign(node.children.begin() + boundary, node.children.end());
            bool promising = false;
            for (std::size_t i = 0; i < result->children.size(); ++i) {
                auto widths = finite(result->children[i], 0);
                if (i && widths && widths->size() > 1) promising = true;
                result->widths.push_back(std::move(widths));
            }
            if (!promising) result.reset();
            if (result) {
                auto bytes = sizeof(Plan) + result->children.capacity() * sizeof(NodeId) +
                    result->widths.capacity() * sizeof(Domain);
                for (const auto& widths : result->widths)
                    if (widths) bytes += widths->capacity() * sizeof(std::size_t);
                constexpr std::size_t byte_cap = 2 * 1024 * 1024;
                if (bytes > byte_cap - retained_bytes_) {
                    result.reset(); counts.capped = true;
                } else admitted_bytes = bytes;
            }
        cache_.emplace(key, result);
        retained_bytes_ += admitted_bytes;
        return result;
        } catch (const std::bad_alloc&) { counts.capped = true; return {}; }
    }

public:
    template<class Charge>
    Result refine(NodeId concat, std::size_t boundary, std::string_view subject,
                  std::size_t begin, std::size_t end, Settings settings,
                  Counters& counts, Charge&& charge_work) const {
        if (begin > end || end > subject.size()) return {};
        const auto p = plan(concat, boundary, counts);
        if (!p) return {};
        // Admission precedes allocations so the owner can charge the bounded
        // workspace and set its retry marker before any optional runtime work.
        charge_work(0);
        const auto m = p->children.size();
        std::vector<Domain> domains(m + 1);
        domains.front() = Values{begin}; domains.back() = Values{end};
        std::deque<std::size_t> pending;
        std::vector<bool> queued(m, true);
        for (std::size_t i = 0; i < m; ++i) pending.push_back(i);
        bool empty = false, limited = false;
        const auto initial_checks = counts.checks, initial_visits = counts.visits;
        const auto charge = [&]() {
            if (counts.checks - initial_checks >= std::min(settings.max_checks, std::size_t{16384})) {
                limited = true; return false;
            }
            ++counts.checks; charge_work(1); return true;
        };
        const auto schedule = [&](std::size_t b) {
            const auto enqueue = [&](std::size_t child) {
                if (!queued[child]) { queued[child] = true; pending.push_back(child); }
            };
            if (b) enqueue(b - 1);
            if (b < m) enqueue(b);
        };
        const auto intersect = [&](std::size_t b, Values candidates) {
            normalize(candidates);
            if (domains[b]) {
                Values common;
                std::set_intersection(domains[b]->begin(), domains[b]->end(),
                    candidates.begin(), candidates.end(), std::back_inserter(common));
                candidates = std::move(common);
            }
            if (candidates.size() > max_widths) { limited = true; return; }
            if (domains[b] && *domains[b] == candidates) return;
            ++counts.refinements;
            counts.peak_domain = std::max(counts.peak_domain, candidates.size());
            domains[b] = std::move(candidates);
            if (domains[b]->empty()) empty = true;
            schedule(b);
        };
        while (!pending.empty() && !empty && !limited) {
            if (counts.visits - initial_visits >= std::min(settings.max_visits, std::size_t{512})) {
                limited = true; break;
            }
            ++counts.visits;
            const auto i = pending.front(); pending.pop_front(); queued[i] = false;
            const auto& widths = p->widths[i];
            if (widths && (settings.finite || widths->size() <= 1)) {
                if (domains[i]) {
                    Values right;
                    for (const auto a : *domains[i]) for (const auto width : *widths) {
                        if (!charge()) break;
                        if (width <= end - a) right.push_back(a + width);
                    }
                    if (limited) break;
                    intersect(i + 1, std::move(right));
                }
                if (domains[i + 1] && !empty && !limited) {
                    Values left;
                    for (const auto b : *domains[i + 1]) for (const auto width : *widths) {
                        if (!charge()) break;
                        if (width <= b - begin) left.push_back(b - width);
                    }
                    if (limited) break;
                    intersect(i, std::move(left));
                }
            }
            if (limited || empty || !domains[i] || !domains[i + 1]) continue;
            Values left, right;
            const auto& lengths = analysis_.length(p->children[i]);
            const auto& bytes = analysis_.boundary(p->children[i]);
            for (const auto a : *domains[i]) for (const auto b : *domains[i + 1]) {
                if (!charge()) break;
                if (a > b || (settings.length && !lengths.accepts(b - a))) continue;
                if (settings.bytes) {
                    if (a == b) { if (!bytes.nullable) continue; }
                    else if (!bytes.first[static_cast<unsigned char>(subject[a])] ||
                             !bytes.last[static_cast<unsigned char>(subject[b - 1])]) continue;
                }
                left.push_back(a); right.push_back(b);
            }
            if (limited) break;
            intersect(i, std::move(left));
            if (!empty && !limited) intersect(i + 1, std::move(right));
        }
        if (limited) { counts.capped = true; return {}; }
        if (empty) return {Result::Kind::Impossible, no_id};
        if (domains[1] && domains[1]->size() == 1) return {Result::Kind::Exact, domains[1]->front()};
        return {};
    }
};

} // namespace crank::detail
