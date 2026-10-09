#pragma once
#include "occurrence_analysis.hpp"
#include "exact_program_statistics.hpp"
#include "pure_alias_source_view.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>

namespace crank::detail {



// A bounded, grammar-static SLR certificate for the complete pure Boolean
// language. LR item sets exist only during compilation. The executable program
// contains deterministic shift/reduce/goto tables; no input-indexed item set,
// memo, endpoint relation, speculative parse, or observer history is retained.
// Optional bounded repeats are expanded, while unbounded pure repetitions use
// language-equivalent left-recursive lists. This lets a shared prefix be read
// before its source alternative is known, even across unbounded nesting.
class DeterministicProgram {
    friend class LoweredDeterministicProgram;
    friend class NativeSlrProgram;
    friend struct DeterministicProgramResearch;
    using Symbol = std::uint16_t;
    static constexpr Symbol nt_base = 256;
    static constexpr std::int32_t accept_action = std::numeric_limits<std::int32_t>::max();
    struct Production { Symbol lhs; std::vector<Symbol> rhs; };
    struct Reduction { Symbol lhs, length; };
    struct Item {
        std::uint16_t production, dot;
        auto operator<=>(const Item&) const = default;
    };
    struct Limit {};
    static constexpr std::size_t max_nodes = 4096, max_edges = 16384, max_nonterminals = 128;
    static constexpr std::size_t max_productions = 1024, max_rhs = 64;
    static constexpr std::size_t max_states = 512, max_items = 16384;
    static constexpr std::size_t max_work = 1'048'576;
    std::array<Symbol, 256> byte_class_{};
    std::size_t terminals_ = 0, nonterminals_ = 0;
    std::vector<std::int32_t> actions_;
    std::vector<std::uint16_t> gotos_;
    std::vector<Reduction> reductions_;
    ExactProgramPreparationStatistics statistics_;

    void charge(std::size_t n = 1) {
        if (n > max_work - statistics_.analysis_steps) throw Limit{};
        statistics_.analysis_steps += n;
    }
    template<class Analysis>
    void build(const Ast& ast, const Analysis& analysis) {
        if (ast.nodes.size() > max_nodes) throw Limit{};
        if (ast.root >= ast.nodes.size()) return;
        std::size_t source_edges = 0;
        for (const auto& node : ast.nodes) {
            charge();
            if (node.children.size() > max_edges - source_edges) throw Limit{};
            source_edges += node.children.size();
            switch (node.kind) {
            case Kind::Empty: case Kind::CharSet: case Kind::Concat: case Kind::Alt:
            case Kind::Repeat: case Kind::Capture: case Kind::Call: case Kind::Define: break;
            default: return;
            }
            if (node.bind_count != no_id || node.use_count != no_id || node.count_path != no_id) return;
            if ((node.kind == Kind::Repeat || node.kind == Kind::Capture) && node.children.size() != 1) return;
            if (node.kind == Kind::Repeat && node.upper && *node.upper < node.lower) return;
            if (node.kind == Kind::Call && node.call_target >= ast.nodes.size()) return;
            for (const auto child : node.children) { charge(); if (child >= ast.nodes.size()) return; }
        }
        std::vector<unsigned char> seen(ast.nodes.size());
        std::vector<NodeId> reachable, pending{ast.root}, character_nodes;
        bool has_call = false;
        while (!pending.empty()) {
            charge();
            auto id = pending.back(); pending.pop_back();
            if (id >= ast.nodes.size()) return;
            if (seen[id]) continue;
            seen[id] = 1; reachable.push_back(id);
            const auto& node = ast.nodes[id];
            if (node.kind == Kind::Reference || node.bind_count != no_id ||
                node.use_count != no_id || node.count_path != no_id) return;
            if (node.kind == Kind::CharSet) character_nodes.push_back(id);
            else if (node.kind == Kind::Call) { has_call = true; pending.push_back(node.call_target); }
            else if (node.kind != Kind::Define)
                pending.insert(pending.end(), node.children.begin(), node.children.end());
        }
        // Existing regular programs and cheap predictive certificates own their
        // established routes. This optional search is for recursive sources.
        if (!has_call) return;
        std::vector<unsigned> representatives;
        for (unsigned byte = 0; byte < 256; ++byte) {
            std::size_t token = 0;
            for (; token < representatives.size(); ++token) {
                bool same = true;
                for (const auto node : character_nodes) {
                    charge();
                    if (ast.nodes[node].characters[byte] != ast.nodes[node].characters[representatives[token]]) {
                        same = false; break;
                    }
                }
                if (same) break;
            }
            if (token == representatives.size()) representatives.push_back(byte);
            byte_class_[byte] = static_cast<Symbol>(token);
        }
        terminals_ = representatives.size(); // Distinct EOF is terminals_.
        statistics_.terminal_classes = terminals_;
        std::vector<Symbol> designated(ast.nodes.size(), std::numeric_limits<Symbol>::max());
        const auto designate = [&](NodeId id) {
            if (designated[id] != std::numeric_limits<Symbol>::max()) return;
            if (nonterminals_ == max_nonterminals) throw Limit{};
            designated[id] = static_cast<Symbol>(nt_base + nonterminals_++);
        };
        designate(ast.root);
        for (const auto id : reachable) {
            const auto& node = ast.nodes[id];
            if (node.kind == Kind::Call) designate(node.call_target);
            if (node.kind == Kind::Repeat && !node.upper) {
                // Repeating a nullable body can create nonprogress reductions;
                // native recognition retains these fixed-point grammars.
                if (analysis.boundary(node.children.front()).nullable) return;
                designate(id);
            }
        }
        if (nonterminals_ == max_nonterminals) throw Limit{};
        const auto augmented = static_cast<Symbol>(nt_base + nonterminals_++);
        std::vector<Production> productions{{augmented, {designated[ast.root]}}};
        using Alternatives = std::vector<std::vector<Symbol>>;
        const auto product = [&](const Alternatives& left, const Alternatives& right) {
            Alternatives output;
            if (right.size() && left.size() > max_productions / right.size()) throw Limit{};
            for (const auto& a : left) for (const auto& b : right) {
                charge();
                if (a.size() + b.size() > max_rhs) throw Limit{};
                auto row = a; row.insert(row.end(), b.begin(), b.end()); output.push_back(std::move(row));
            }
            return output;
        };
        const auto expand = [&](auto&& self, NodeId id, bool definition, std::size_t depth) -> Alternatives {
            charge(); if (depth > 128) throw Limit{};
            if (!definition && designated[id] != std::numeric_limits<Symbol>::max()) return {{designated[id]}};
            const auto& node = ast.nodes[id];
            switch (node.kind) {
            case Kind::Empty: case Kind::Define: return {{}};
            case Kind::CharSet: {
                Alternatives output;
                for (std::size_t c = 0; c < terminals_; ++c)
                    if (node.characters[representatives[c]]) output.push_back({static_cast<Symbol>(c)});
                return output;
            }
            case Kind::Call: return {{designated[node.call_target]}};
            case Kind::Capture: return self(self, node.children.front(), false, depth + 1);
            case Kind::Concat: {
                Alternatives output{{}};
                for (const auto child : node.children) output = product(output, self(self, child, false, depth + 1));
                return output;
            }
            case Kind::Alt: {
                Alternatives output;
                for (const auto child : node.children) {
                    auto rows = self(self, child, false, depth + 1);
                    if (rows.size() > max_productions - output.size()) throw Limit{};
                    output.insert(output.end(), rows.begin(), rows.end());
                }
                return output;
            }
            case Kind::Repeat: {
                if (!node.upper || *node.upper > 32) throw Limit{};
                const auto body = self(self, node.children.front(), false, depth + 1);
                Alternatives power{{}}, output;
                for (std::size_t count = 0; count <= *node.upper; ++count) {
                    if (count >= node.lower) {
                        if (power.size() > max_productions - output.size()) throw Limit{};
                        output.insert(output.end(), power.begin(), power.end());
                    }
                    if (count != *node.upper) power = product(power, body);
                }
                return output;
            }
            case Kind::Reference: return {};
            }
            return {};
        };
        for (const auto id : reachable) if (designated[id] != std::numeric_limits<Symbol>::max()) {
            Alternatives rows;
            const auto& node = ast.nodes[id];
            if (node.kind == Kind::Repeat && !node.upper) {
                if (node.lower > 32) throw Limit{};
                const auto body = expand(expand, node.children.front(), false, 0);
                rows = {{}};
                for (std::size_t i = 0; i < node.lower; ++i) rows = product(rows, body);
                auto next = product(Alternatives{{designated[id]}}, body);
                if (next.size() > max_productions - rows.size()) throw Limit{};
                rows.insert(rows.end(), next.begin(), next.end());
            } else rows = expand(expand, id, true, 0);
            std::sort(rows.begin(), rows.end()); rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
            if (rows.size() > max_productions - productions.size()) throw Limit{};
            for (auto& row : rows) productions.push_back({designated[id], std::move(row)});
        }
        statistics_.productions = productions.size();
        std::vector<std::vector<std::size_t>> by_lhs(nonterminals_);
        for (std::size_t p = 0; p < productions.size(); ++p) by_lhs[productions[p].lhs - nt_base].push_back(p);
        std::vector<unsigned char> nullable(nonterminals_);
        std::vector<std::bitset<257>> first(nonterminals_), follow(nonterminals_);
        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto& production : productions) {
                charge(); auto& target = first[production.lhs - nt_base]; const auto old = target;
                bool empty = true;
                for (const auto s : production.rhs) {
                    charge();
                    if (s < nt_base) { target.set(s); empty = false; break; }
                    target |= first[s - nt_base];
                    if (!nullable[s - nt_base]) { empty = false; break; }
                }
                if (empty && !nullable[production.lhs - nt_base]) { nullable[production.lhs - nt_base] = 1; changed = true; }
                changed |= target != old;
            }
        }
        // Reject any cycle whose traversal can consume zero bytes. This keeps
        // the runtime reduction sequence progressing independently of caps.
        std::vector<std::vector<std::size_t>> zero(nonterminals_);
        for (const auto& p : productions) for (std::size_t i = 0; i < p.rhs.size(); ++i) {
            if (p.rhs[i] < nt_base) continue;
            bool empty = true;
            for (std::size_t j = 0; j < p.rhs.size(); ++j) if (i != j) {
                charge(); if (p.rhs[j] < nt_base || !nullable[p.rhs[j] - nt_base]) { empty = false; break; }
            }
            if (empty) zero[p.lhs - nt_base].push_back(p.rhs[i] - nt_base);
        }
        std::vector<unsigned char> color(nonterminals_);
        const auto acyclic = [&](auto&& self, std::size_t n) -> bool {
            charge(); if (color[n] == 1) return false; if (color[n] == 2) return true;
            color[n] = 1;
            for (const auto child : zero[n]) if (!self(self, child)) return false;
            color[n] = 2; return true;
        };
        for (std::size_t n = 0; n < nonterminals_; ++n) if (!acyclic(acyclic, n)) return;
        follow[augmented - nt_base].set(terminals_);
        changed = true;
        while (changed) {
            changed = false;
            for (const auto& p : productions) {
                auto suffix = follow[p.lhs - nt_base];
                for (auto i = p.rhs.size(); i-- > 0;) {
                    charge(); const auto s = p.rhs[i];
                    if (s < nt_base) { suffix.reset(); suffix.set(s); }
                    else {
                        auto& target = follow[s - nt_base]; const auto old = target;
                        target |= suffix; changed |= old != target;
                        suffix = nullable[s - nt_base] ? suffix | first[s - nt_base] : first[s - nt_base];
                    }
                }
            }
        }
        const auto closure = [&](std::vector<Item> items) {
            std::set<Item> unique(items.begin(), items.end());
            for (std::size_t at = 0; at < items.size(); ++at) {
                charge(); const auto item = items[at]; const auto& rhs = productions[item.production].rhs;
                if (item.dot == rhs.size() || rhs[item.dot] < nt_base) continue;
                for (const auto p : by_lhs[rhs[item.dot] - nt_base]) {
                    charge(); Item next{static_cast<std::uint16_t>(p), 0};
                    if (unique.insert(next).second) items.push_back(next);
                    if (items.size() > max_items) throw Limit{};
                }
            }
            std::sort(items.begin(), items.end()); return items;
        };
        std::vector<std::vector<Item>> states{closure({Item{0, 0}})};
        std::map<std::vector<Item>, std::size_t> ids{{states.front(), 0}};
        std::size_t total_items = states.front().size();
        for (std::size_t state = 0; state < states.size(); ++state) {
            actions_.resize((state + 1) * (terminals_ + 1));
            gotos_.resize((state + 1) * nonterminals_, std::numeric_limits<std::uint16_t>::max());
            const auto action = [&](std::size_t token, std::int32_t value) {
                charge(); auto& slot = actions_[state * (terminals_ + 1) + token];
                if (slot && slot != value) {
                    ++statistics_.conflicts;
                    statistics_.conflict_state = state; statistics_.conflict_token = token;
                    statistics_.conflict_existing_action = slot; statistics_.conflict_new_action = value;
                    return false;
                }
                slot = value; return true;
            };
            std::map<Symbol, std::vector<Item>> shifts;
            for (const auto item : states[state]) {
                charge(); const auto& p = productions[item.production];
                if (item.dot < p.rhs.size()) shifts[p.rhs[item.dot]].push_back({item.production, static_cast<std::uint16_t>(item.dot + 1)});
                else if (!item.production) { if (!action(terminals_, accept_action)) return; }
                else for (std::size_t token = 0; token <= terminals_; ++token)
                    if (follow[p.lhs - nt_base][token] && !action(token, -static_cast<std::int32_t>(item.production))) return;
            }
            for (auto& [symbol, kernel] : shifts) {
                auto target = closure(std::move(kernel));
                auto found = ids.find(target);
                std::size_t next;
                if (found == ids.end()) {
                    if (states.size() == max_states || target.size() > max_items - total_items) throw Limit{};
                    total_items += target.size(); next = states.size(); ids.emplace(target, next); states.push_back(std::move(target));
                } else next = found->second;
                if (symbol < nt_base) { if (!action(symbol, static_cast<std::int32_t>(next + 1))) return; }
                else gotos_[state * nonterminals_ + symbol - nt_base] = static_cast<std::uint16_t>(next);
            }
        }
        statistics_.states = states.size(); statistics_.items = total_items;
        for (const auto& p : productions) reductions_.push_back({static_cast<Symbol>(p.lhs - nt_base), static_cast<Symbol>(p.rhs.size())});
        statistics_.certified = true;
        statistics_.retained_bytes = sizeof(*this) + actions_.capacity() * sizeof(std::int32_t) +
            gotos_.capacity() * sizeof(std::uint16_t) + reductions_.capacity() * sizeof(Reduction);
    }
public:
    template<class Analysis>
    explicit DeterministicProgram(const Ast& ast, const Analysis& analysis,
            bool normalize_pure_aliases = true) {
        statistics_.attempted = true;
        try {
            build(ast, analysis);
            if (normalize_pure_aliases && !statistics_.certified && statistics_.conflicts) {
                statistics_.alias_normalization_attempted = true;
                statistics_.original_conflicts = statistics_.conflicts;
                if (auto normalized = pure_alias_source_view(ast, [this](std::size_t count = 1) { charge(count); }, statistics_.normalized_alias_calls)) {
                    actions_.clear(); gotos_.clear(); reductions_.clear();
                    terminals_ = nonterminals_ = 0;
                    statistics_.productions = statistics_.states = statistics_.items = 0;
                    statistics_.terminal_classes = statistics_.conflicts = 0;
                    // Each node's language and nullability are preserved, so
                    // the original immutable source facts remain valid.
                    build(*normalized, analysis);
                }
            }
        }
        catch (const Limit&) { statistics_.limited = true; }
        catch (const std::bad_alloc&) { statistics_.limited = true; }
        if (!statistics_.certified) {
            decltype(actions_){}.swap(actions_); decltype(gotos_){}.swap(gotos_); decltype(reductions_){}.swap(reductions_);
            statistics_.retained_bytes = sizeof(*this);
        }
    }
    [[nodiscard]] bool certified() const { return statistics_.certified; }
    [[nodiscard]] const ExactProgramPreparationStatistics& statistics() const { return statistics_; }
    struct Runtime { std::size_t actions = 0, shifts = 0, reductions = 0, peak_stack = 0; };
    template<class Tick, class Allocate>
    bool execute(std::string_view subject, std::size_t begin, std::size_t end,
                 Tick&& tick, Allocate&& allocate, Runtime& runtime) const {
        if (!certified() || begin > end || end > subject.size())
            throw std::invalid_argument("invalid exact-program demand");
        runtime = {};
        std::vector<std::uint16_t> stack;
        const auto push = [&](std::uint16_t state) {
            allocate(); stack.push_back(state); runtime.peak_stack = std::max(runtime.peak_stack, stack.size());
        };
        push(0); std::size_t position = begin;
        for (;;) {
            tick(); ++runtime.actions;
            const auto token = position == end ? terminals_ : byte_class_[static_cast<unsigned char>(subject[position])];
            const auto action = actions_[stack.back() * (terminals_ + 1) + token];
            if (!action) return false;
            if (action == accept_action) return position == end;
            if (action > 0) { push(static_cast<std::uint16_t>(action - 1)); ++position; ++runtime.shifts; }
            else {
                const auto& reduction = reductions_[-action];
                if (reduction.length >= stack.size()) throw std::logic_error("invalid exact-program reduction");
                stack.resize(stack.size() - reduction.length);
                const auto next = gotos_[stack.back() * nonterminals_ + reduction.lhs];
                if (next == std::numeric_limits<std::uint16_t>::max()) throw std::logic_error("invalid exact-program goto");
                push(next); ++runtime.reductions;
            }
        }
    }
};

class DeterministicProgramCache {
    const Ast& ast_;
    const OccurrenceAnalysis& analysis_;
    mutable std::once_flag once_;
    mutable std::atomic_bool ready_{false};
    mutable std::unique_ptr<DeterministicProgram> program_;
public:
    DeterministicProgramCache(const Ast& ast, const OccurrenceAnalysis& analysis) : ast_(ast), analysis_(analysis) {}
    const DeterministicProgram* certificate() const {
        if (!ready_.load(std::memory_order_acquire)) {
            std::call_once(once_, [&] {
                try { program_ = std::make_unique<DeterministicProgram>(ast_, analysis_); }
                catch (const std::bad_alloc&) {}
                ready_.store(true, std::memory_order_release);
            });
        }
        return program_ && program_->certified() ? program_.get() : nullptr;
    }
    ExactProgramPreparationStatistics statistics() const {
        if (!ready_.load(std::memory_order_acquire)) return {};
        if (program_) return program_->statistics();
        ExactProgramPreparationStatistics result; result.attempted = result.limited = true; return result;
    }
};
} // namespace crank::detail
