#include "ast_control.hpp"
#include "ast_equivalence.hpp"

#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

using namespace crank;
using namespace crank::detail;

namespace {
std::size_t checks = 0;
void require(bool condition, const char* message) {
    ++checks;
    if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}
NodeId add(Ast& ast, Kind kind, std::vector<NodeId> children = {}) {
    Node node; node.kind = kind; node.children = std::move(children);
    ast.nodes.push_back(std::move(node));
    return ast.nodes.size() - 1;
}
NodeId byte(Ast& ast, unsigned char value) {
    const auto id = add(ast, Kind::CharSet);
    ast.nodes[id].characters.set(value);
    return id;
}
NodeId repeat(Ast& ast, NodeId child, std::size_t lower,
              std::optional<std::size_t> upper = {}) {
    const auto id = add(ast, Kind::Repeat, {child});
    ast.nodes[id].lower = lower; ast.nodes[id].upper = upper;
    return id;
}
NodeId self(Ast& ast, unsigned char open, unsigned char close, bool nullable) {
    const auto a = byte(ast, open), b = byte(ast, close);
    const auto call = add(ast, Kind::Call);
    const auto sequence = add(ast, Kind::Concat, {a, call, b});
    const auto base = nullable ? add(ast, Kind::Empty) : byte(ast, 'x');
    const auto alt = add(ast, Kind::Alt, {sequence, base});
    const auto capture = add(ast, Kind::Capture, {alt});
    ast.nodes[call].call_target = capture;
    return capture;
}
NodeId mutual(Ast& ast, unsigned char close) {
    const auto a = byte(ast, 'a'), b = byte(ast, close);
    const auto call_r = add(ast, Kind::Call), call_s = add(ast, Kind::Call);
    const auto s = add(ast, Kind::Capture, {add(ast, Kind::Concat, {b, call_r})});
    const auto r = add(ast, Kind::Capture, {add(ast, Kind::Alt,
        {add(ast, Kind::Concat, {a, call_s}), add(ast, Kind::Empty)})});
    ast.nodes[call_r].call_target = r; ast.nodes[call_s].call_target = s;
    return r;
}

// Independent finite-relation least fixed point, without native requests,
// scheduling, control collapse, length summaries, or quotient implementation.
using Relation = std::vector<std::uint64_t>;
Relation identity(std::size_t width) {
    Relation result(width);
    for (std::size_t i = 0; i < width; ++i) result[i] = std::uint64_t{1} << i;
    return result;
}
Relation compose(const Relation& left, const Relation& right) {
    Relation result(left.size());
    for (std::size_t begin = 0; begin < left.size(); ++begin)
        for (std::size_t middle = 0; middle < left.size(); ++middle)
            if (left[begin] & (std::uint64_t{1} << middle)) result[begin] |= right[middle];
    return result;
}
void unite(Relation& into, const Relation& from) {
    for (std::size_t i = 0; i < into.size(); ++i) into[i] |= from[i];
}
std::vector<Relation> language(const Ast& ast, const std::string& input) {
    const auto width = input.size() + 1;
    std::vector<Relation> result(ast.nodes.size(), Relation(width));
    bool changed;
    do {
        changed = false;
        for (NodeId id = 0; id < ast.nodes.size(); ++id) {
            const auto& node = ast.nodes[id];
            Relation value(width);
            switch (node.kind) {
            case Kind::Empty: case Kind::Define: value = identity(width); break;
            case Kind::CharSet:
                for (std::size_t begin = 0; begin < input.size(); ++begin)
                    if (node.characters[static_cast<unsigned char>(input[begin])])
                        value[begin] |= std::uint64_t{1} << (begin + 1);
                break;
            case Kind::Capture: value = result[node.children.front()]; break;
            case Kind::Call: value = result[node.call_target]; break;
            case Kind::Alt:
                for (const auto child : node.children) unite(value, result[child]);
                break;
            case Kind::Concat:
                value = identity(width);
                for (const auto child : node.children) value = compose(value, result[child]);
                break;
            case Kind::Repeat: {
                auto power = identity(width);
                // Finite cases in this oracle have bounds <= 4. In an
                // unbounded repeat, lower + input length iterations suffice:
                // consuming factors cost bytes and epsilon can pad lower.
                const auto upper = node.upper.value_or(node.lower + input.size());
                for (std::size_t count = 0; count <= upper; ++count) {
                    if (count >= node.lower) unite(value, power);
                    power = compose(power, result[node.children.front()]);
                }
                break;
            }
            default: std::abort();
            }
            auto next = result[id]; unite(next, value);
            if (next != result[id]) { result[id] = std::move(next); changed = true; }
        }
    } while (changed);
    return result;
}
void exhaust(const Ast& ast, const std::string& alphabet, std::size_t maximum) {
    const auto control = pure_control_targets(ast);
    const auto quotient = pure_ast_equivalent_targets(ast, control);
    for (NodeId id = 0; id < quotient.size(); ++id)
        require(quotient[quotient[id]] == quotient[id], "quotient must be idempotent");
    std::vector<std::string> level{std::string{}};
    for (std::size_t length = 0; length <= maximum; ++length) {
        std::vector<std::string> next;
        for (const auto& input : level) {
            const auto relations = language(ast, input);
            for (NodeId id = 0; id < ast.nodes.size(); ++id)
                require(relations[id] == relations[quotient[id]], "quotient changed an all-span language");
            for (const auto ch : alphabet) next.push_back(input + ch);
        }
        level = std::move(next);
    }
}
} // namespace

int main() {
    {
        Ast ast;
        const auto r0 = self(ast, 'a', 'b', true);
        const auto r1 = self(ast, 'a', 'b', true);
        const auto r2 = self(ast, 'a', 'c', true);
        const auto r3 = self(ast, 'a', 'b', false);
        ast.root = add(ast, Kind::Alt, {r0, r1, r2, r3});
        EquivalenceStatistics statistics;
        const auto map = pure_ast_equivalent_targets(ast, pure_control_targets(ast), &statistics);
        require(statistics.eligible && !statistics.limit_reached &&
                statistics.work > 0 && statistics.merged_nodes > 0, "successful quotient diagnostics");
        require(map[r0] == map[r1], "renamed self-recursive copies must merge");
        require(map[r0] != map[r2], "different closing terminals must remain distinct");
        require(map[r0] != map[r3], "different recursive bases must remain distinct");
        exhaust(ast, "abcx", 4);
    }
    {
        Ast ast;
        const auto r0 = mutual(ast, 'b'), r1 = mutual(ast, 'b'), r2 = mutual(ast, 'c');
        ast.root = add(ast, Kind::Alt, {r0, r1, r2});
        const auto map = pure_ast_equivalent_targets(ast, pure_control_targets(ast));
        require(map[r0] == map[r1], "renamed mutually recursive copies must merge");
        require(map[r0] != map[r2], "different mutual equation must not merge");
        exhaust(ast, "abc", 5);
    }
    {
        Ast ast;
        const auto a = byte(ast, 'a'), a2 = byte(ast, 'a');
        const auto nullable = add(ast, Kind::Alt, {a, add(ast, Kind::Empty)});
        const auto nullable2 = add(ast, Kind::Alt, {a2, add(ast, Kind::Empty)});
        const auto r0 = repeat(ast, nullable, 1, 3), r1 = repeat(ast, nullable2, 1, 3);
        const auto r2 = repeat(ast, nullable2, 1, 4), r3 = repeat(ast, nullable2, 0, 3);
        const auto r4 = repeat(ast, nullable2, 1), r5 = repeat(ast, nullable, 1);
        ast.root = add(ast, Kind::Alt, {r0, r1, r2, r3, r4, r5});
        const auto map = pure_ast_equivalent_targets(ast, pure_control_targets(ast));
        require(map[r0] == map[r1] && map[r4] == map[r5], "equal bounds must merge");
        require(map[r0] != map[r2] && map[r0] != map[r3] && map[r0] != map[r4],
                "different bound syntax must stay distinct");
        exhaust(ast, "ab", 5);
    }
    {
        Ast ast;
        const auto c0 = add(ast, Kind::Call), c1 = add(ast, Kind::Call);
        ast.nodes[c0].call_target = c0; ast.nodes[c1].call_target = c1;
        const auto nullable_cycle = add(ast, Kind::Alt, {add(ast, Kind::Empty)});
        ast.nodes[nullable_cycle].children.push_back(nullable_cycle);
        ast.root = add(ast, Kind::Alt, {c0, c1, nullable_cycle});
        const auto map = pure_ast_equivalent_targets(ast, pure_control_targets(ast));
        require(map[c0] != map[c1], "pure alias cycles are deliberately not merged");
        require(map[c0] != map[nullable_cycle], "unproductive and nullable cycle differ");
        exhaust(ast, "ab", 4);
    }
    {
        Ast ast;
        const auto a = byte(ast, 'a'), b = byte(ast, 'b');
        const auto ab = add(ast, Kind::Concat, {a, b}), ba = add(ast, Kind::Concat, {b, a});
        const auto lo = byte(ast, 0), hi = byte(ast, 255), hi2 = byte(ast, 255);
        ast.root = add(ast, Kind::Alt, {ab, ba, lo, hi, hi2});
        const auto map = pure_ast_equivalent_targets(ast, pure_control_targets(ast));
        require(map[ab] != map[ba], "ordered children must remain ordered");
        require(map[lo] != map[hi] && map[hi] == map[hi2], "full 256-bit terminals must be compared");
        exhaust(ast, std::string{"ab\0\xff", 4}, 4);
    }
    {
        Ast ast;
        const auto a = byte(ast, 'a'), duplicate = byte(ast, 'a');
        auto last = add(ast, Kind::Empty);
        for (std::size_t i = 0; i < 1'600; ++i) last = add(ast, Kind::Concat, {a, last});
        ast.root = add(ast, Kind::Alt, {last, duplicate});
        const auto control = pure_control_targets(ast);
        EquivalenceStatistics statistics;
        require(pure_ast_equivalent_targets(ast, control, &statistics) == control,
                "compile cap must discard even obvious early aliases");
        require(statistics.eligible && statistics.limit_reached && statistics.merged_nodes == 0 &&
                statistics.work > 0 && statistics.work <= 1'000'000, "work cap diagnostics");
    }
    {
        Ast ast; const auto a = byte(ast, 'a'); byte(ast, 'a');
        ast.nodes[a].bind_count = 0;
        const auto control = pure_control_targets(ast);
        EquivalenceStatistics statistics;
        require(pure_ast_equivalent_targets(ast, control, &statistics) == control, "effects must disable quotient");
        require(!statistics.eligible && !statistics.limit_reached && statistics.merged_nodes == 0,
                "effectful decline diagnostics");
        ast.nodes[a].bind_count = no_id;
        auto invalid = control; invalid[0] = ast.nodes.size();
        require(pure_ast_equivalent_targets(ast, invalid) == invalid, "invalid map must safely decline");
    }
    {
        Ast ast; const auto a = byte(ast, 'a');
        const auto largest = std::numeric_limits<std::size_t>::max();
        const auto finite = repeat(ast, a, largest, largest);
        const auto unbounded = repeat(ast, a, largest);
        const auto finite_copy = repeat(ast, a, largest, largest);
        const auto map = pure_ast_equivalent_targets(ast, pure_control_targets(ast));
        require(map[finite] == map[finite_copy], "largest finite bounds must compare exactly");
        require(map[finite] != map[unbounded], "largest finite bound is not unbounded");
    }
    {
        Ast ast; ast.nodes.resize(16'385);
        const auto control = pure_control_targets(ast);
        EquivalenceStatistics statistics;
        require(pure_ast_equivalent_targets(ast, control, &statistics) == control, "node cap must decline safely");
        require(!statistics.eligible && statistics.limit_reached && statistics.work == 0 &&
                statistics.merged_nodes == 0, "node cap diagnostics");
    }
    std::cout << checks << " structural equivalence checks passed\n";
}
