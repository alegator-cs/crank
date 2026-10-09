#pragma once
#include "regex_ast.hpp"
#include <optional>
#include <vector>

namespace crank::detail {
// Existing pure alias law, independent of the representation consuming it.
// Captured aliases ending in one call denote that call target's least language.
// Redirect only call edges; preserve each node's language and nullable fact.
// Functional alias cycles stay unchanged, never guessed empty or epsilon.
// The caller must validate purity and source/edge bounds before this helper,
// and supplies a shared bounded preparation meter (including the source copy).
template<class Charge>
std::optional<Ast> pure_alias_source_view(const Ast& ast, Charge&& charge, std::size_t& rewritten_calls) {
        std::vector<NodeId> alias(ast.nodes.size(), no_id);
        for (NodeId id = 0; id < ast.nodes.size(); ++id) {
            charge();
            if (ast.nodes[id].kind != Kind::Capture) continue;
            NodeId next = ast.nodes[id].children.front();
            std::size_t depth = 0;
            while (depth++ < ast.nodes.size()) {
                charge();
                const auto& node = ast.nodes[next];
                if (node.kind == Kind::Call) { alias[id] = node.call_target; break; }
                if ((node.kind == Kind::Capture || node.kind == Kind::Concat || node.kind == Kind::Alt) &&
                    node.children.size() == 1) next = node.children.front();
                else break;
            }
        }
        // The source-size/edge caps were checked before the first conflict.
        // Resolve only pure alias edges, with the same overall analysis budget.
        std::vector<NodeId> targets(ast.nodes.size(), no_id);
        std::size_t changed = 0;
        for (NodeId id = 0; id < ast.nodes.size(); ++id) {
            charge();
            if (ast.nodes[id].kind != Kind::Call) continue;
            NodeId target = ast.nodes[id].call_target;
            std::size_t depth = 0;
            while (alias[target] != no_id && depth < ast.nodes.size()) {
                charge(); ++depth; target = alias[target];
            }
            // Reaching the node bound proves a cycle on this functional graph.
            if (depth == ast.nodes.size()) target = ast.nodes[id].call_target;
            targets[id] = target;
            changed += target != ast.nodes[id].call_target;
        }
        if (!changed) return std::nullopt;
        // Charge the bounded local copy before allocating its node/edge arrays.
        charge(ast.nodes.size());
        for (const auto& node : ast.nodes) charge(node.children.size());
        Ast result = ast;
        for (NodeId id = 0; id < ast.nodes.size(); ++id)
            if (targets[id] != no_id) result.nodes[id].call_target = targets[id];
        rewritten_calls = changed;
        return result;
    }
} // namespace crank::detail
