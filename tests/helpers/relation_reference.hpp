#pragma once
#include "ast_machine.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

// Deliberately independent denotational reference evaluator. It knows only the
// public AST syntax and a finite Boolean relation on input positions. It uses
// no Crank analysis, request keys, scheduler, continuations, proofs, summaries,
// or length/nullability metadata. The dense relation is appropriate here only
// because oracle inputs are short; this is not an execution backend.
namespace certified_reference {
using namespace crank;

struct Matrix {
    std::size_t width;
    std::vector<unsigned char> bits;
    explicit Matrix(std::size_t n) : width(n), bits(n * n) {}
    unsigned char& at(std::size_t i, std::size_t j) { return bits[i * width + j]; }
    bool at(std::size_t i, std::size_t j) const { return bits[i * width + j] != 0; }
    bool operator==(const Matrix&) const = default;
};

Matrix identity(std::size_t n) {
    Matrix result(n);
    for (std::size_t i = 0; i < n; ++i) result.at(i, i) = 1;
    return result;
}
void unite(Matrix& into, const Matrix& from) {
    for (std::size_t i = 0; i < into.bits.size(); ++i) into.bits[i] |= from.bits[i];
}
Matrix compose(const Matrix& left, const Matrix& right) {
    Matrix result(left.width);
    for (std::size_t i = 0; i < left.width; ++i)
        for (std::size_t k = 0; k < left.width; ++k)
            if (left.at(i, k))
                for (std::size_t j = 0; j < left.width; ++j)
                    result.at(i, j) |= right.at(k, j);
    return result;
}
Matrix closure(Matrix result) {
    for (std::size_t i = 0; i < result.width; ++i) result.at(i, i) = 1;
    for (std::size_t k = 0; k < result.width; ++k)
        for (std::size_t i = 0; i < result.width; ++i)
            if (result.at(i, k))
                for (std::size_t j = 0; j < result.width; ++j)
                    result.at(i, j) |= result.at(k, j);
    return result;
}

// Binary exponentiation plus a geometric relation sum. Returns (R^n,
// I | R | ... | R^(n-1)); finite repeats never need a count-sized loop.
std::pair<Matrix, Matrix> powers(const Matrix& relation, std::size_t n) {
    if (!n) return {identity(relation.width), Matrix(relation.width)};
    auto [half, sum] = powers(relation, n / 2);
    auto square = compose(half, half);
    auto tail = compose(half, sum);
    unite(sum, tail);
    if (n % 2) {
        unite(sum, square);
        square = compose(square, relation);
    }
    return {std::move(square), std::move(sum)};
}

struct Language {
    std::vector<Matrix> nodes;
    std::size_t rounds = 0;
};
Language language(const Ast& ast, const std::string& input) {
    const auto width = input.size() + 1;
    Language result{std::vector<Matrix>(ast.nodes.size(), Matrix(width))};
    for (;;) {
        auto next = result.nodes;
        for (NodeId id = 0; id < ast.nodes.size(); ++id) {
            const auto& node = ast.nodes[id];
            Matrix value(width);
            switch (node.kind) {
            case Kind::Empty: case Kind::Define: value = identity(width); break;
            case Kind::CharSet:
                for (std::size_t i = 0; i < input.size(); ++i)
                    if (node.characters[static_cast<unsigned char>(input[i])])
                        value.at(i, i + 1) = 1;
                break;
            case Kind::Capture: value = result.nodes.at(node.children.at(0)); break;
            case Kind::Call: value = result.nodes.at(node.call_target); break;
            case Kind::Alt:
                for (auto child : node.children) unite(value, result.nodes.at(child));
                break;
            case Kind::Concat:
                value = identity(width);
                for (auto child : node.children) value = compose(value, result.nodes.at(child));
                break;
            case Kind::Repeat: {
                const auto& child = result.nodes.at(node.children.at(0));
                auto lower = powers(child, node.lower).first;
                if (node.upper) {
                    const auto difference = *node.upper - node.lower;
                    auto [last, sum] = powers(child, difference);
                    unite(sum, last); // Inclusive upper, without overflowing upper+1.
                    value = compose(lower, sum);
                } else value = compose(lower, closure(child));
                break;
            }
            default: throw std::logic_error("oracle received an effectful node");
            }
            unite(next[id], value);
        }
        ++result.rounds;
        if (next == result.nodes) return result;
        result.nodes = std::move(next);
        // Every strict iteration adds at least one of finitely many facts.
        if (result.rounds > ast.nodes.size() * width * width + 1)
            throw std::logic_error("least-fixed-point convergence invariant");
    }
}

std::string escaped(std::string_view value) {
    std::ostringstream out;
    for (unsigned char c : value) {
        if (c >= 32 && c < 127) out << c;
        else out << "\\x" << std::hex << std::setw(2) << std::setfill('0') << unsigned(c);
    }
    return out.str();
}

struct Random {
    std::uint64_t state;
    std::uint64_t next() {
        state ^= state >> 12; state ^= state << 25; state ^= state >> 27;
        return state * UINT64_C(2685821657736338717);
    }
    std::size_t choose(std::size_t bound) { return next() % bound; }
};

NodeId add(Ast& ast, Kind kind, std::vector<NodeId> children = {}) {
    Node n; n.kind = kind; n.children = std::move(children);
    ast.nodes.push_back(std::move(n)); return ast.nodes.size() - 1;
}
NodeId byte(Ast& ast, unsigned char value) {
    const auto id = add(ast, Kind::CharSet); ast.nodes[id].characters.set(value); return id;
}
NodeId repeat(Ast& ast, NodeId child, std::size_t lower, std::optional<std::size_t> upper) {
    const auto id = add(ast, Kind::Repeat, {child});
    ast.nodes[id].lower = lower; ast.nodes[id].upper = upper; return id;
}
NodeId call(Ast& ast, NodeId target) {
    const auto id = add(ast, Kind::Call); ast.nodes[id].call_target = target;
    ast.has_calls = true; return id;
}

struct Fixture {
    std::string name;
    Ast ast;
    std::vector<std::string> inputs;
};
std::vector<std::string> words(std::string alphabet, unsigned maximum) {
    std::vector<std::string> all{std::string{}}, layer = all;
    for (unsigned length = 0; length < maximum; ++length) {
        std::vector<std::string> next;
        for (const auto& prefix : layer) for (const auto ch : alphabet) next.push_back(prefix + ch);
        all.insert(all.end(), next.begin(), next.end()); layer = std::move(next);
    }
    return all;
}

NodeId expression(Ast& ast, const std::vector<NodeId>& rules, Random& random, unsigned depth) {
    const auto kind = random.choose(depth ? 9 : 4);
    if (kind == 0) return add(ast, Kind::Empty);
    if (kind == 1) return byte(ast, 'a');
    if (kind == 2) return byte(ast, 'b');
    if (kind == 3) return call(ast, rules[random.choose(rules.size())]);
    if (kind == 4 || kind == 5) {
        std::vector<NodeId> children;
        const auto count = 2 + random.choose(2);
        for (std::size_t i = 0; i < count; ++i) children.push_back(expression(ast, rules, random, depth - 1));
        return add(ast, kind == 4 ? Kind::Concat : Kind::Alt, std::move(children));
    }
    if (kind == 6) {
        const auto id = add(ast, Kind::CharSet); ast.nodes[id].characters.set('a');
        ast.nodes[id].characters.set('b'); return id;
    }
    const auto child = expression(ast, rules, random, depth - 1);
    const auto lower = random.choose(3);
    return repeat(ast, child, lower, kind == 7 ? std::optional<std::size_t>{lower + random.choose(3)} : std::nullopt);
}

Fixture random_fixture(Random& random, unsigned ordinal, unsigned maximum) {
    Ast ast; std::vector<NodeId> rules;
    const auto count = 2 + random.choose(3);
    for (std::size_t i = 0; i < count; ++i) {
        const auto id = add(ast, Kind::Capture);
        ast.nodes[id].capture_id = ast.capture_names.size();
        ast.capture_names.push_back("R" + std::to_string(i));
        ast.capture_nodes.push_back(id); rules.push_back(id);
    }
    for (const auto rule : rules) {
        const auto child = expression(ast, rules, random, 2 + random.choose(2));
        ast.nodes[rule].children = {child};
    }
    // Several callers of the same rules, with different suffixes and origins.
    const auto first = call(ast, rules[random.choose(count)]);
    const auto second = call(ast, rules[random.choose(count)]);
    const auto third = call(ast, rules[random.choose(count)]);
    const auto left = add(ast, Kind::Concat, {byte(ast, 'a'), first, byte(ast, 'a')});
    const auto right = add(ast, Kind::Concat, {second, byte(ast, 'b')});
    ast.root = add(ast, Kind::Alt, {left, right, third}); ast.capture_nodes[0] = ast.root;
    return {"random_" + std::to_string(ordinal), std::move(ast), words("ab", maximum)};
}

} // namespace certified_reference
