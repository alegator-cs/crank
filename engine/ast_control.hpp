#pragma once

#include "regex_ast.hpp"
#include <vector>

namespace crank::detail {

// Requires a valid, pure AST and a query that does not observe captures.
// These single-successor nodes preserve their child's recognition language.
// Effectful nodes must never be passed through this control-only shortcut.
[[nodiscard]] inline std::vector<NodeId> pure_control_targets(const Ast& ast) {
    std::vector<NodeId> targets(ast.nodes.size());
    for (NodeId id = 0; id < ast.nodes.size(); ++id) {
        const auto& node = ast.nodes[id];
        targets[id] = id;
        switch (node.kind) {
        case Kind::Capture:
            targets[id] = node.children.front();
            break;
        case Kind::Call:
            targets[id] = node.call_target;
            break;
        case Kind::Concat:
        case Kind::Alt:
            if (node.children.size() == 1) targets[id] = node.children.front();
            break;
        default:
            break;
        }
    }

    // Each node enters one path and is finalized once: O(AST size), without
    // using the C++ call stack. A pure alias cycle has no productive exit, so
    // any cycle member represents its (empty) language for the usual analysis.
    std::vector<unsigned char> state(ast.nodes.size(), 0);
    std::vector<NodeId> path;
    for (NodeId start = 0; start < ast.nodes.size(); ++start) {
        if (state[start] != 0) continue;
        path.clear();
        NodeId current = start;
        while (state[current] == 0) {
            state[current] = 1;
            path.push_back(current);
            current = targets[current];
        }
        const NodeId target = state[current] == 2 ? targets[current] : current;
        for (const NodeId id : path) {
            targets[id] = target;
            state[id] = 2;
        }
    }
    return targets;
}

} // namespace crank::detail
