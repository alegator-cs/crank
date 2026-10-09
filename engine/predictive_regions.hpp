#pragma once
#include "occurrence_analysis.hpp"
#include <algorithm>
#include <array>

namespace crank::detail {

// Internal precondition: AstMachineProgram already validated every actual AST
// edge, node kind and effect field; summary flags are not trusted.
// A certificate for one closed Boolean demand, not an endpoint enumerator.
// FOLLOW is computed from the original root with EOF as its only outer token.
// Native endpoint and witness queries do not consume this certificate.
struct PredictiveRegion {
    struct Choice {
        std::array<NodeId, 256> byte;
        NodeId empty = no_id;
        Choice() { byte.fill(no_id); }
    };
    struct Scope {
        NodeId entry;
        std::bitset<256> follow;
        bool eof = false;
    };
    NodeId entry = no_id, decline_node = no_id;
    const char* decline_reason = "builder declined; detailed witness unavailable";
    bool certified = false, limit_reached = false;
    std::size_t analysis_steps = 0;
    std::size_t root_child_count = no_id;
    std::vector<NodeId> targets;
    std::vector<Choice> choices;
    std::vector<std::size_t> choice_index;

    template<class Analysis>
    PredictiveRegion(const Ast& ast, const Analysis& analysis,
                     std::size_t root_children = no_id) : entry(ast.root), root_child_count(root_children) {
        try { build(ast, analysis, {}); }
        catch (const std::bad_alloc&) {
            targets.clear(); choices.clear(); choice_index.clear(); limit_reached = true;
        }
    }
    // A regional certificate is seeded by the admitted consumer token set.
    // EOF is distinct from every byte; it participates in the executor's exit
    // predicate but cannot conflict with a nonempty FIRST set. All reachable
    // source calls are closed here, so no unfinished native relation is read.
    template<class Analysis>
    PredictiveRegion(const Ast& ast, const Analysis& analysis, Scope scope)
        : entry(scope.entry) {
        try { build(ast, analysis, scope.follow); }
        catch (const std::bad_alloc&) {
            targets.clear(); choices.clear(); choice_index.clear(); limit_reached = true;
        }
    }
private:
    template<class Analysis>
    void build(const Ast& ast, const Analysis& analysis,
               const std::bitset<256>& outer_follow) {
        constexpr std::size_t max_nodes = 4096, max_edges = 16384;
        constexpr std::size_t max_work = 1'048'576, max_choices = 256;
        if (ast.nodes.size() > max_nodes) { limit_reached = true; return; }
        if (entry >= ast.nodes.size()) return;
        // An acyclic expression already has the regular compiler. Inspect real
        // nodes instead of trusting Ast::has_calls, which is public mutable data.
        if (std::none_of(ast.nodes.begin(), ast.nodes.end(), [](const Node& node) {
                return node.kind == Kind::Call;
            })) return;
        const auto child_count = [&](NodeId id) {
            return id == ast.root && root_child_count != no_id ? root_child_count : ast.nodes[id].children.size();
        };
        if (root_child_count != no_id && (ast.nodes[ast.root].kind != Kind::Concat ||
            root_child_count > ast.nodes[ast.root].children.size())) return;
        const auto edges = [&](NodeId id, bool leading) {
            std::vector<NodeId> result;
            const auto& node = ast.nodes[id];
            if (node.kind == Kind::Call) result.push_back(node.call_target);
            else if (node.kind == Kind::Capture || node.kind == Kind::Alt)
                result = node.children;
            else if (node.kind == Kind::Concat) {
                for (std::size_t i = 0; i < child_count(id); ++i) {
                    const auto child = node.children[i];
                    result.push_back(child);
                    if (leading && !analysis.boundary(child).nullable) break;
                }
            } else if (node.kind == Kind::Repeat && (!node.upper || *node.upper))
                result = node.children;
            return result;
        };
        std::vector<unsigned char> seen(ast.nodes.size());
        std::vector<NodeId> reachable, pending{entry};
        std::vector<std::vector<NodeId>> graph(ast.nodes.size()), zero(ast.nodes.size());
        std::size_t edge_count = 0;
        while (!pending.empty()) {
            const auto id = pending.back(); pending.pop_back();
            if (seen[id]) continue;
            seen[id] = 1; reachable.push_back(id);
            graph[id] = edges(id, false); zero[id] = edges(id, true);
            edge_count += graph[id].size();
            if (edge_count > max_edges) { limit_reached = true; return; }
            for (const auto child : graph[id]) pending.push_back(child);
        }
        const auto cyclic = [&](const auto& links) {
            struct Frame { NodeId id; std::size_t next = 0; };
            std::vector<unsigned char> color(ast.nodes.size());
            std::vector<Frame> stack;
            for (const auto seed : reachable) {
                if (color[seed]) continue;
                color[seed] = 1; stack.push_back({seed});
                while (!stack.empty()) {
                    auto& top = stack.back();
                    if (top.next == links[top.id].size()) {
                        color[top.id] = 2; stack.pop_back(); continue;
                    }
                    const auto child = links[top.id][top.next++];
                    if (color[child] == 1) return true;
                    if (!color[child]) { color[child] = 1; stack.push_back({child}); }
                }
            }
            return false;
        };
        // Regular execution has stronger existing representations. Any cycle
        // in the zero-consumption graph can diverge without reading a byte.
        if (!cyclic(graph) || cyclic(zero)) return;
        std::vector<std::bitset<256>> follow(ast.nodes.size());
        follow[entry] = outer_follow;
        bool changed = true;
        while (changed) {
            changed = false;
            const auto add = [&](NodeId id, const std::bitset<256>& bytes) {
                if (++analysis_steps > max_work) return false;
                const auto combined = follow[id] | bytes;
                if (combined != follow[id]) { follow[id] = combined; changed = true; }
                return true;
            };
            for (const auto id : reachable) {
                const auto& node = ast.nodes[id];
                if (node.kind == Kind::Concat) {
                    auto suffix = follow[id];
                    for (auto i = child_count(id); i-- > 0;) {
                        const auto child = node.children[i];
                        if (!add(child, suffix)) { limit_reached = true; return; }
                        const auto& b = analysis.boundary(child);
                        suffix = b.nullable ? suffix | b.first : b.first;
                    }
                } else if (node.kind == Kind::Repeat && (!node.upper || *node.upper)) {
                    const auto child = node.children.front();
                    auto suffix = follow[id];
                    if (!node.upper || *node.upper > 1) suffix |= analysis.boundary(child).first;
                    if (!add(child, suffix)) { limit_reached = true; return; }
                } else {
                    for (const auto child : graph[id])
                        if (!add(child, follow[id])) { limit_reached = true; return; }
                }
            }
        }
        std::vector<Choice> prepared;
        std::vector<std::size_t> indices(ast.nodes.size(), no_id);
        for (const auto id : reachable) {
            const auto& node = ast.nodes[id];
            if (node.kind == Kind::Alt) {
                if (prepared.size() == max_choices) { limit_reached = true; return; }
                Choice choice;
                std::bitset<256> first;
                for (const auto child : node.children) {
                    const auto& b = analysis.boundary(child);
                    if ((first & b.first).any()) { decline_node = id; decline_reason = "overlapping alternative FIRST bytes"; return; }
                    first |= b.first;
                    if (b.nullable) {
                        if (choice.empty != no_id) { decline_node = id; decline_reason = "multiple nullable alternatives"; return; }
                        choice.empty = child;
                    }
                    for (std::size_t byte = 0; byte < 256; ++byte)
                        if (b.first[byte]) choice.byte[byte] = child;
                }
                if (choice.empty != no_id && (first & follow[id]).any()) { decline_node = id; decline_reason = "nullable alternative FIRST/FOLLOW conflict"; return; }
                indices[id] = prepared.size(); prepared.push_back(std::move(choice));
            } else if (node.kind == Kind::Repeat && (!node.upper || *node.upper)) {
                const auto& body = analysis.boundary(node.children.front());
                if (body.nullable) { decline_node = id; decline_reason = "nullable repeated body"; return; }
                if ((!node.upper || *node.upper != node.lower) &&
                    (body.first & follow[id]).any()) { decline_node = id; decline_reason = "repeat FIRST/FOLLOW conflict"; return; }
            }
        }
        choices = std::move(prepared); choice_index = std::move(indices);
        targets.resize(ast.nodes.size());
        for (const auto id : reachable) {
            auto current = id;
            for (std::size_t count = 0; count < ast.nodes.size(); ++count) {
                if (++analysis_steps > max_work) { limit_reached = true; return; }
                const auto& node = ast.nodes[current];
                if (node.kind == Kind::Call) current = node.call_target;
                else if (node.kind == Kind::Capture ||
                         ((node.kind == Kind::Concat || node.kind == Kind::Alt) && child_count(current) == 1))
                    current = node.children.front();
                else break;
            }
            targets[id] = current;
        }
        certified = true;
    }
};

// Full-root exact membership can validate a fixed trailing word first, making
// the residual end an actual EOF demand for FIRST/FOLLOW. The root is never a
// source-rule target (or a structural child), so trimming its outgoing concat
// edges cannot change the meaning of any recursive invocation. Node identities
// and all child summaries remain those of the original validated AST.
struct PredictiveRimRegion {
    bool certified = false, limit_reached = false;
    std::size_t root_child_count = no_id;
    std::vector<NodeId> suffix;
    std::optional<PredictiveRegion> certificate;
    PredictiveRimRegion(const Ast& ast, const OccurrenceAnalysis& analysis,
                        bool ordinary_certified = false) {
        if (ordinary_certified || ast.nodes.size() > 4096 || ast.nodes[ast.root].kind != Kind::Concat) return;
        try { build(ast, analysis); }
        catch (const std::bad_alloc&) { suffix.clear(); certificate.reset(); limit_reached = true; }
    }
private:
    void build(const Ast& ast, const OccurrenceAnalysis& analysis) {
        for (const auto& node : ast.nodes) {
            if (node.kind == Kind::Call && node.call_target == ast.root) return;
            if (std::find(node.children.begin(), node.children.end(), ast.root) != node.children.end()) return;
        }
        std::size_t work = 0;
        const auto word = [&](auto&& self, NodeId id, std::vector<NodeId>& output, std::size_t depth) -> bool {
            if (++work > 65536 || depth > 64) { limit_reached = true; return false; }
            const auto& node = ast.nodes[id];
            switch (node.kind) {
            case Kind::Empty: case Kind::Define: return true;
            case Kind::CharSet:
                if (output.size() == 1024) { limit_reached = true; return false; }
                output.push_back(id); return true;
            case Kind::Capture: return self(self, node.children.front(), output, depth + 1);
            case Kind::Concat:
                for (const auto child : node.children) if (!self(self, child, output, depth + 1)) return false;
                return true;
            case Kind::Repeat:
                if (!node.upper || *node.upper != node.lower || node.lower > 64) return false;
                for (std::size_t n = 0; n < node.lower; ++n)
                    if (!self(self, node.children.front(), output, depth + 1)) return false;
                return true;
            default: return false;
            }
        };
        const auto& children = ast.nodes[ast.root].children;
        root_child_count = children.size();
        while (root_child_count > 1) {
            std::vector<NodeId> part;
            if (!word(word, children[root_child_count - 1], part, 0)) break;
            if (part.size() > 1024 - suffix.size()) { limit_reached = true; return; }
            suffix.insert(suffix.begin(), part.begin(), part.end());
            --root_child_count;
        }
        if (limit_reached || suffix.empty()) return;
        certificate.emplace(ast, analysis, root_child_count);
        certified = certificate->certified;
        limit_reached = certificate->limit_reached;
    }
};
} // namespace crank::detail
