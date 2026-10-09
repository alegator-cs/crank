#pragma once
#include "occurrence_analysis.hpp"
#include <algorithm>
#include <array>
#include <functional>

namespace crank::detail {

// A bounded certificate for pure Boolean membership of a fixed span. Every
// clause has the language prefix . child? . suffix, where the two rims are
// fixed words of character sets. Competing clauses have disjoint necessary
// boundary/length conditions. A selected child therefore has one exact span:
// no pending return is needed. This is independent of FIRST/FOLLOW prediction.
// Internal precondition: the owner validated the actual AST and effect fields.
struct ExactEndSpines {
    struct Clause {
        std::vector<NodeId> prefix, suffix;
        NodeId child = no_id; // AST target while building, state index afterwards.
        std::size_t minimum = 0;
        [[nodiscard]] std::size_t width() const { return prefix.size() + suffix.size(); }
    };
    struct State {
        std::vector<Clause> clauses;
        // Optional one-byte dispatch. Zero-width bases are selected only for
        // the empty span; every nonempty candidate has an explicit chosen rim.
        enum class Axis { None, First, Last } axis = Axis::None;
        std::array<std::size_t, 256> choice;
        std::size_t empty = no_id;
        State() { choice.fill(no_id); }
    };
    bool certified = false, limit_reached = false;
    std::size_t analysis_steps = 0;
    std::vector<State> states;

    template<class Analysis>
    ExactEndSpines(const Ast& ast, const Analysis& analysis, NodeId entry = no_id) {
        try { build(ast, analysis, entry == no_id ? ast.root : entry); }
        catch (const Declined&) { states.clear(); }
        catch (const Limited&) { states.clear(); limit_reached = true; }
        catch (const std::bad_alloc&) { states.clear(); limit_reached = true; }
    }
private:
    struct Declined {};
    struct Limited {};
    static constexpr std::size_t max_nodes = 4096, max_states = 128;
    static constexpr std::size_t max_clauses = 512, max_atoms = 16384;
    static constexpr std::size_t max_work = 262144, max_depth = 256;
    void charge(std::size_t work = 1) {
        if (work > max_work - analysis_steps) throw Limited{};
        analysis_steps += work;
    }
    template<class Analysis>
    void build(const Ast& ast, const Analysis& analysis, NodeId entry) {
        if (ast.nodes.size() > max_nodes) throw Limited{};
        if (entry >= ast.nodes.size()) throw Declined{};
        if (std::none_of(ast.nodes.begin(), ast.nodes.end(), [](const Node& node) {
                return node.kind == Kind::Call;
            })) return;
        std::vector<std::optional<std::vector<Clause>>> memo(ast.nodes.size());
        std::vector<unsigned char> active(ast.nodes.size());
        std::size_t saved_atoms = 0, saved_clauses = 0;
        const auto multiply = [&](const std::vector<Clause>& left, const std::vector<Clause>& right) {
            std::vector<Clause> result;
            if (left.size() && right.size() > max_clauses / left.size()) throw Limited{};
            for (const auto& a : left) for (const auto& b : right) {
                charge();
                if (a.child != no_id && b.child != no_id) throw Declined{};
                if (a.width() > max_atoms - b.width()) throw Limited{};
                Clause combined = a;
                if (a.child == no_id) {
                    combined.prefix.insert(combined.prefix.end(), b.prefix.begin(), b.prefix.end());
                    combined.child = b.child;
                    combined.suffix = b.suffix;
                } else {
                    combined.suffix.insert(combined.suffix.end(), b.prefix.begin(), b.prefix.end());
                }
                charge(combined.width());
                result.push_back(std::move(combined));
            }
            return result;
        };
        std::function<const std::vector<Clause>&(NodeId, std::size_t)> expand;
        expand = [&](NodeId id, std::size_t depth) -> const std::vector<Clause>& {
            charge();
            if (memo[id]) return *memo[id];
            if (active[id]) throw Declined{}; // Malformed structural cycles are not call spines.
            if (depth >= max_depth) throw Limited{};
            active[id] = 1;
            const auto& node = ast.nodes[id];
            std::vector<Clause> result;
            switch (node.kind) {
            case Kind::Empty: case Kind::Define: result.emplace_back(); break;
            case Kind::CharSet:
                if (node.characters.any()) { result.emplace_back(); result.back().prefix.push_back(id); }
                break;
            case Kind::Call:
                if (analysis.length(node.call_target).productive) {
                    result.emplace_back(); result.back().child = node.call_target;
                }
                break;
            case Kind::Capture: result = expand(node.children.front(), depth + 1); break;
            case Kind::Alt:
                for (const auto child : node.children) {
                    const auto& terms = expand(child, depth + 1);
                    if (terms.size() > max_clauses - result.size()) throw Limited{};
                    result.insert(result.end(), terms.begin(), terms.end());
                }
                break;
            case Kind::Concat:
                result.emplace_back();
                for (const auto child : node.children) result = multiply(result, expand(child, depth + 1));
                break;
            case Kind::Repeat: {
                if (node.upper && *node.upper == 0) { result.emplace_back(); break; }
                if (!node.upper || *node.upper > 64) throw Declined{};
                const auto& body = expand(node.children.front(), depth + 1);
                std::vector<Clause> power(1);
                for (std::size_t count = 0; count <= *node.upper; ++count) {
                    if (count >= node.lower) {
                        if (power.size() > max_clauses - result.size()) throw Limited{};
                        result.insert(result.end(), power.begin(), power.end());
                    }
                    if (count < *node.upper) power = multiply(power, body);
                }
                break;
            }
            case Kind::Reference: throw Declined{};
            }
            if (result.size() > max_clauses - saved_clauses) throw Limited{};
            saved_clauses += result.size();
            for (const auto& clause : result) {
                if (clause.width() > max_atoms - saved_atoms) throw Limited{};
                saved_atoms += clause.width(); charge(clause.width());
            }
            active[id] = 0; memo[id] = std::move(result); return *memo[id];
        };
        std::vector<NodeId> entries{entry};
        std::vector<std::size_t> index(ast.nodes.size(), no_id); index[entry] = 0;
        for (std::size_t i = 0; i < entries.size(); ++i) {
            State state; state.clauses = expand(entries[i], 0);
            for (auto& clause : state.clauses) {
                clause.minimum = clause.width();
                if (clause.child == no_id) continue;
                const auto child = clause.child;
                const auto lower = analysis.length(child).minimum;
                if (lower > no_id - clause.minimum) throw Declined{};
                clause.minimum += lower;
                if (index[child] == no_id) {
                    if (entries.size() == max_states) throw Limited{};
                    index[child] = entries.size(); entries.push_back(child);
                }
                clause.child = index[child];
            }
            states.push_back(std::move(state));
        }
        const auto incompatible = [&](NodeId a, NodeId b) {
            charge(); return (ast.nodes[a].characters & ast.nodes[b].characters).none();
        };
        const auto separated = [&](const Clause& a, const Clause& b) {
            if (a.child == no_id && b.child == no_id && a.width() != b.width()) return true;
            const auto base_against = [&](const Clause& base, const Clause& recursive) {
                if (base.width() < recursive.minimum) return true;
                for (std::size_t j = 0; j < recursive.prefix.size(); ++j)
                    if (incompatible(base.prefix[j], recursive.prefix[j])) return true;
                for (std::size_t j = 0; j < recursive.suffix.size(); ++j)
                    if (incompatible(base.prefix[base.width() - recursive.suffix.size() + j], recursive.suffix[j])) return true;
                return false;
            };
            if (a.child == no_id && b.child != no_id) return base_against(a, b);
            if (b.child == no_id && a.child != no_id) return base_against(b, a);
            for (std::size_t j = 0; j < std::min(a.prefix.size(), b.prefix.size()); ++j)
                if (incompatible(a.prefix[j], b.prefix[j])) return true;
            for (std::size_t j = 0; j < std::min(a.suffix.size(), b.suffix.size()); ++j)
                if (incompatible(a.suffix[a.suffix.size() - 1 - j], b.suffix[b.suffix.size() - 1 - j])) return true;
            return false;
        };
        for (auto& state : states) {
            for (std::size_t i = 0; i < state.clauses.size(); ++i)
                for (std::size_t j = 0; j < i; ++j) {
                    charge();
                    if (!separated(state.clauses[i], state.clauses[j])) throw Declined{};
                }
            const auto dispatch = [&](State::Axis axis) {
                state.choice.fill(no_id); state.empty = no_id;
                for (std::size_t i = 0; i < state.clauses.size(); ++i) {
                    const auto& clause = state.clauses[i];
                    if (clause.child == no_id && clause.width() == 0) { state.empty = i; continue; }
                    NodeId terminal = no_id;
                    if (axis == State::Axis::First && !clause.prefix.empty()) terminal = clause.prefix.front();
                    if (axis == State::Axis::Last) {
                        if (!clause.suffix.empty()) terminal = clause.suffix.back();
                        else if (clause.child == no_id && !clause.prefix.empty()) terminal = clause.prefix.back();
                    }
                    if (terminal == no_id) return false;
                    for (std::size_t byte = 0; byte < 256; ++byte) {
                        charge();
                        if (!ast.nodes[terminal].characters[byte]) continue;
                        if (state.choice[byte] != no_id) return false;
                        state.choice[byte] = i;
                    }
                }
                state.axis = axis; return true;
            };
            if (!dispatch(State::Axis::First)) dispatch(State::Axis::Last);
        }
        // Positive rims shrink the span. Acyclic zero-rim transfers may change
        // rule identity but cannot postpone consumption indefinitely.
        const auto cyclic = [&](bool zero_only) {
            std::vector<unsigned char> color(states.size());
            std::function<bool(std::size_t)> visit = [&](std::size_t i) {
                if (color[i]) return color[i] == 1;
                color[i] = 1;
                for (const auto& clause : states[i].clauses) {
                    charge();
                    if (clause.child != no_id && (!zero_only || clause.width() == 0) && visit(clause.child)) return true;
                }
                color[i] = 2; return false;
            };
            for (std::size_t i = 0; i < states.size(); ++i) if (visit(i)) return true;
            return false;
        };
        if (!cyclic(false) || cyclic(true)) throw Declined{};
        certified = true;
    }
};
} // namespace crank::detail
