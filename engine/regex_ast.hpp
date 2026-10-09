#pragma once
#include "occurrence_path.hpp"
#include <bitset>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace crank {
using NodeId = std::size_t;
inline constexpr std::size_t no_id = std::numeric_limits<std::size_t>::max();
enum class Kind { Empty, CharSet, Concat, Alt, Repeat, Capture, Reference, Call, Define };
struct Node {
    Kind kind = Kind::Empty;
    std::bitset<256> characters;
    std::vector<NodeId> children;
    std::size_t lower = 0;
    std::optional<std::size_t> upper;
    std::size_t capture_id = no_id;
    NodeId call_target = no_id; // Non-structural link; calls may form cycles.
    std::size_t bind_count = no_id;
    std::size_t use_count = no_id;
    std::size_t count_path = no_id; // Index in Ast::count_paths; explicit occurrence query.
    bool forward = false;
    bool nullable = true; // Conservative when runtime references are present.
    std::size_t minimum_length = 0; // Sound lower bound, saturated.
    bool stateful = false;
};
struct Ast {
    std::vector<Node> nodes;
    NodeId root = 0;
    std::vector<std::string> capture_names{std::string{}}; // Slot 0 is the full match.
    std::vector<NodeId> capture_nodes{no_id}; // Slot 0 is root after parsing.
    std::vector<std::string> count_names;
    std::vector<NodeId> count_nodes;
    std::vector<CountPath> count_paths;
    bool has_references = false;
    bool has_counts = false;
    bool has_calls = false; // Includes non-consuming DEFINE blocks.
    bool has_occurrence_paths = false;
    bool has_extended = false; // Paths or bounded binders require the occurrence engine.
    [[nodiscard]] bool regular() const {
        return !has_references && !has_counts && !has_calls && !has_extended &&
               !has_occurrence_paths;
    }
};
[[nodiscard]] Ast parse(std::string_view pattern);
}
