#pragma once
#include "regular_matcher.hpp"
#include <algorithm>
#include <optional>

namespace crank::detail {

struct RegularRegionStatistics {
    std::size_t candidates = 0, programs = 0;
    std::size_t nfa_states = 0, dfa_states = 0;
    bool limit_reached = false;
};

// An exact summary owns only its compiled transition table. No input spans,
// endpoint matrix, expanded AST, or query budget is consumed by compilation.
struct RegularRegions {
    struct Continuation {
        RegularProgram program;
        // no_id forwards an alternation result; otherwise resume concat here.
        std::size_t boundary;
    };
    std::vector<std::optional<RegularProgram>> programs;
    std::vector<Continuation> continuations;
    std::vector<std::vector<std::size_t>> concatenations;
    std::vector<std::size_t> alternations;
    std::vector<std::vector<bool>> alternate_members;
    RegularRegionStatistics statistics;

    RegularRegions() = default;
    RegularRegions(const Ast& ast, const std::vector<NodeId>& control,
                   const std::vector<NodeId>& equivalent) {
        try { build(ast, control, equivalent); }
        catch (const std::bad_alloc&) {
            // Optional compilation never prevents preparation of an otherwise
            // usable native program. Release every partially retained table.
            *this = RegularRegions{};
            statistics.limit_reached = true;
        }
    }

private:
    void build(const Ast& ast, const std::vector<NodeId>& control,
               const std::vector<NodeId>& equivalent) {
        constexpr std::size_t max_nodes = 16'384, max_edges = 65'536;
        constexpr std::size_t max_programs = 64, max_nfa = 8192, max_dfa = 1024;
        if (ast.nodes.size() > max_nodes) { statistics.limit_reached = true; return; }
        struct Frame { NodeId node; std::size_t next = 0; bool regular = true; };
        std::vector<unsigned char> color(ast.nodes.size()), regular(ast.nodes.size());
        std::vector<Frame> stack{{ast.root}};
        std::size_t edges = 0;
        const auto edge_count = [&](NodeId id) {
            const auto& node = ast.nodes[id];
            return node.kind == Kind::Define ? std::size_t{0} :
                node.kind == Kind::Call ? std::size_t{1} : node.children.size();
        };
        const auto child_at = [&](NodeId id, std::size_t index) {
            const auto& node = ast.nodes[id];
            return node.kind == Kind::Call ? node.call_target : node.children[index];
        };
        // Iterative postorder marks exactly the nodes whose reachable executed
        // graph is acyclic. A backedge taints its ancestors, including the whole
        // recursive component, but does not taint independent regular children.
        while (!stack.empty()) {
            auto& frame = stack.back();
            color[frame.node] = 1;
            if (frame.next == edge_count(frame.node)) {
                regular[frame.node] = frame.regular;
                color[frame.node] = 2;
                const bool valid = frame.regular;
                stack.pop_back();
                if (!stack.empty()) stack.back().regular &= valid;
                continue;
            }
            if (++edges > max_edges) { statistics.limit_reached = true; return; }
            const auto child = child_at(frame.node, frame.next++);
            if (color[child] == 0) stack.push_back({child});
            else frame.regular &= color[child] == 2 && regular[child];
        }
        // Whole regular programs already have the top-level DFA route. This
        // pass pays only for the seam between recursive and acyclic execution.
        if (regular[ast.root]) return;
        programs.resize(ast.nodes.size());
        concatenations.resize(ast.nodes.size());
        alternations.resize(ast.nodes.size(), no_id);
        alternate_members.resize(ast.nodes.size());
        std::vector<NodeId> pending{ast.root};
        std::vector<bool> seen(ast.nodes.size());
        while (!pending.empty()) {
            const auto id = pending.back(); pending.pop_back();
            if (seen[id]) continue;
            seen[id] = true;
            if (regular[id]) {
                const auto target = control[id];
                const auto& node = ast.nodes[target];
                if (node.kind == Kind::CharSet || node.kind == Kind::Empty || node.kind == Kind::Define)
                    continue;
                ++statistics.candidates;
                if (statistics.candidates > max_programs || statistics.nfa_states == max_nfa ||
                    statistics.dfa_states == max_dfa) { statistics.limit_reached = true; continue; }
                if (programs[target]) continue;
                try {
                    RegularOptions options;
                    options.paired_transitions = false; // Region frontiers use single-step transitions.
                    options.max_nfa_states = std::min<std::size_t>(512, max_nfa - statistics.nfa_states);
                    options.max_dfa_states = std::min<std::size_t>(128, max_dfa - statistics.dfa_states);
                    options.max_expansion_depth = 256;
                    RegularProgram program(ast, target, options);
                    const auto counts = program.construction_statistics();
                    statistics.nfa_states += counts.nfa_states;
                    statistics.dfa_states += counts.dfa_states;
                    ++statistics.programs;
                    programs[target] = program;
                    // Both execution control maps must find the same language.
                    programs[equivalent[target]] = program;
                    programs[id] = std::move(program);
                    continue;
                } catch (const std::invalid_argument&) {
                    // Public native Ast values may contain ignored leaf
                    // children that the stricter DFA compiler declines.
                } catch (const std::length_error&) {
                    statistics.limit_reached = true;
                } catch (const std::bad_alloc&) {
                    statistics.limit_reached = true;
                    return;
                }
                // A bounded construction failure may leave smaller islands.
            }
            for (std::size_t edge = 0; edge < edge_count(id); ++edge)
                pending.push_back(child_at(id, edge));
        }
        // Syntax flattening must not hide a regular region: several adjacent
        // children of a recursive concatenation are still one exact language.
        // Likewise, a mixed alternation observes only the union of its regular
        // alternatives. One temporary AST adds composition nodes; source nodes
        // and resolved call targets are neither expanded nor changed.
        Ast extended = ast;
        for (NodeId id = 0; id < ast.nodes.size(); ++id) {
            if (!color[id] || regular[id]) continue;
            const auto& node = ast.nodes[id];
            if (node.kind != Kind::Concat && node.kind != Kind::Alt) continue;
            const auto compile = [&](std::vector<NodeId> children, std::size_t boundary) {
                if (++statistics.candidates > max_programs || statistics.nfa_states == max_nfa ||
                    statistics.dfa_states == max_dfa) { statistics.limit_reached = true; return no_id; }
                Node joined;
                joined.kind = node.kind;
                joined.children = std::move(children);
                extended.nodes.push_back(std::move(joined));
                try {
                    RegularOptions options;
                    options.paired_transitions = false; // Region frontiers use single-step transitions.
                    options.max_nfa_states = std::min<std::size_t>(512, max_nfa - statistics.nfa_states);
                    options.max_dfa_states = std::min<std::size_t>(128, max_dfa - statistics.dfa_states);
                    options.max_expansion_depth = 256;
                    RegularProgram program(extended, extended.nodes.size() - 1, options);
                    const auto counts = program.construction_statistics();
                    statistics.nfa_states += counts.nfa_states;
                    statistics.dfa_states += counts.dfa_states;
                    ++statistics.programs;
                    const auto result = continuations.size();
                    continuations.push_back({std::move(program), boundary});
                    return result;
                } catch (const std::invalid_argument&) { /* Native-only AST shape. */ }
                catch (const std::length_error&) { statistics.limit_reached = true; }
                catch (const std::bad_alloc&) { statistics.limit_reached = true; }
                return no_id;
            };
            if (node.kind == Kind::Concat) {
                for (std::size_t begin = 0; begin < node.children.size();) {
                    if (!regular[node.children[begin]]) { ++begin; continue; }
                    auto end = begin + 1;
                    while (end < node.children.size() && regular[node.children[end]]) ++end;
                    if (end - begin >= 2) {
                        const auto region = compile({node.children.begin() + begin, node.children.begin() + end}, end);
                        if (region != no_id) {
                            if (concatenations[id].empty()) concatenations[id].resize(node.children.size(), no_id);
                            concatenations[id][begin] = region;
                        }
                    }
                    begin = end;
                }
            } else {
                std::vector<NodeId> members;
                for (const auto child : node.children) if (regular[child]) members.push_back(child);
                if (members.size() < 2) continue;
                const auto region = compile(std::move(members), no_id);
                if (region != no_id) {
                    alternations[id] = region;
                    for (const auto child : node.children) alternate_members[id].push_back(regular[child]);
                }
            }
        }
    }
};
} // namespace crank::detail
