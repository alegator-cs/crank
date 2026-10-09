#include "ast_machine.hpp"
#include "occurrence_analysis.hpp"
#include "occurrence_matcher.hpp"

#include <algorithm>
#include <bitset>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
std::size_t checks = 0, analyses = 0, summaries = 0, derived_words = 0;
std::size_t membership_cases = 0, endpoint_cases = 0, witness_cases = 0;
void require(bool value, const std::string& label) {
    ++checks;
    if (!value) throw std::runtime_error(label);
}

// The public fingerprint contract. The string oracle never uses fingerprints
// to generate words, so a missing bit exposes an unsound summary transfer.
[[maybe_unused]] std::uint64_t pair_bit(unsigned first, unsigned last) {
    return std::uint64_t{1} << ((17U * first + last) & 63U);
}
[[maybe_unused]] std::uint64_t pair_mask(std::initializer_list<std::string_view> pairs) {
    std::uint64_t result = 0;
    for (const auto text : pairs) {
        if (text.size() != 2) throw std::runtime_error("pair fixture is not two bytes");
        result |= pair_bit(static_cast<unsigned char>(text[0]), static_cast<unsigned char>(text[1]));
    }
    return result;
}

using Language = std::set<std::string>;
Language product(const Language& a, const Language& b, std::size_t maximum) {
    Language result;
    for (const auto& first : a)
        for (const auto& second : b)
            if (first.size() <= maximum && second.size() <= maximum - first.size())
                result.insert(first + second);
    return result;
}

std::vector<std::string> words(std::string_view alphabet, unsigned maximum) {
    std::vector<std::string> result{std::string{}};
    std::size_t begin = 0, end = 1;
    for (unsigned length = 0; length < maximum; ++length) {
        for (auto i = begin; i < end; ++i)
            for (const char byte : alphabet) result.push_back(result[i] + byte);
        begin = end;
        end = result.size();
    }
    return result;
}

std::string hex(std::string_view text) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (const unsigned char byte : text) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 15]);
    }
    return result;
}

struct Symbol { bool terminal; std::size_t id; std::string text; };
Symbol n(std::size_t id) { return {false, id, {}}; }
Symbol t(std::string text) { return {true, 0, std::move(text)}; }
struct Production { std::size_t lhs; std::vector<Symbol> rhs; };

// An explicit grammar-language fixed point independent of AST requests,
// arithmetic facts, FIRST/LAST transfer functions and execution order.
std::vector<Language> grammar_languages(std::size_t size,
    const std::vector<Production>& productions, std::size_t maximum) {
    std::vector<Language> result(size);
    bool changed;
    do {
        changed = false;
        for (const auto& production : productions) {
            Language partial{std::string{}};
            for (const auto& symbol : production.rhs) {
                const Language terminal{symbol.text};
                partial = product(partial, symbol.terminal ? terminal : result.at(symbol.id), maximum);
            }
            auto& destination = result.at(production.lhs);
            const auto before = destination.size();
            destination.insert(partial.begin(), partial.end());
            changed |= before != destination.size();
        }
    } while (changed);
    return result;
}

struct Fixture {
    std::string name, pattern;
    std::size_t nonterminals;
    std::vector<Production> productions;
    std::string alphabet = "ab";
};

std::vector<Fixture> fixtures() {
    std::vector<Fixture> result{
        {"balanced nullable", "(?<S>a(?&S)b|)", 1,
         {{0, {t("a"), n(0), t("b")}}, {0, {}}}},
        {"balanced positive", "(?<S>a(?&S)b|ab)", 1,
         {{0, {t("a"), n(0), t("b")}}, {0, {t("ab")}}}},
        {"left recursion", "(?<S>(?&S)a|b)", 1,
         {{0, {n(0), t("a")}}, {0, {t("b")}}}},
        {"right recursion", "(?<S>a(?&S)|b)", 1,
         {{0, {t("a"), n(0)}}, {0, {t("b")}}}},
        {"nullable unit cycle", "(?<S>(?&S)|a|)", 1,
         {{0, {n(0)}}, {0, {t("a")}}, {0, {}}}},
        {"ambiguous nullable recursion", "(?<S>(?&S)(?&S)|a|b|)", 1,
         {{0, {n(0), n(0)}}, {0, {t("a")}}, {0, {t("b")}}, {0, {}}}},
        {"dead recursion", "(?<S>(?&S))", 1, {{0, {n(0)}}}},
        {"dead branch", "(?(DEFINE)(?<D>(?&D)))(?:(?&D)a|b)", 2,
         {{0, {n(1), t("a")}}, {0, {t("b")}}, {1, {n(1)}}}},
        {"mutual nullable", "(?(DEFINE)(?<A>a(?&B)|)(?<B>(?&A)b))(?&A)", 2,
         {{0, {t("a"), n(1)}}, {0, {}}, {1, {n(0), t("b")}}}},
        {"nullable prefix and suffix", "a?ba?", 2,
         {{0, {n(1), t("b"), n(1)}}, {1, {}}, {1, {t("a")}}}},
        {"shared calls different enclosing ends",
         "(?(DEFINE)(?<A>a|ab))(?:(?&A)b|(?&A)bb)", 2,
         {{0, {n(1), t("b")}}, {0, {n(1), t("bb")}},
          {1, {t("a")}}, {1, {t("ab")}}}},
        {"reversed shared branches",
         "(?(DEFINE)(?<A>a|ab))(?:(?&A)bb|(?&A)b)", 2,
         {{0, {n(1), t("b")}}, {0, {n(1), t("bb")}},
          {1, {t("a")}}, {1, {t("ab")}}}},
        {"nested scalar effects", "(?<outer>(?<a>a?)b(?<b>a?))", 2,
         {{0, {n(1), t("b"), n(1)}}, {1, {}}, {1, {t("a")}}}},
        {"nullable bounded repetition", "(?:a|){2,3}b", 2,
         {{0, {n(1), n(1), t("b")}}, {0, {n(1), n(1), n(1), t("b")}},
          {1, {}}, {1, {t("a")}}}},
        {"all byte signs", "(?<S>\\x00(?&S)\\xff|\\x80|)", 1,
         {{0, {t(std::string(1, '\0')), n(0), t(std::string(1, char(255)))}},
          {0, {t(std::string(1, char(128)))}}, {0, {}}},
         std::string{char(0), char(128), char(255)}}
    };
    result.push_back({"correlated two-byte choices", "(?:ab|ba)", 1,
                      {{0, {t("ab")}}, {0, {t("ba")}}}});
    result.push_back({"nullable pair choices", "(?:ab|ba|)", 1,
                      {{0, {t("ab")}}, {0, {t("ba")}}, {0, {}}}});
    result.push_back({"one byte or pair", "(?:a|ba)", 1,
                      {{0, {t("a")}}, {0, {t("ba")}}}});
    result.push_back({"nullable repeated pair", "(?:ab|){0,3}", 1,
                      {{0, {}}, {0, {t("ab")}}, {0, {t("abab")}}, {0, {t("ababab")}}}});
    result.push_back({"common prefix recursive pairs", "(?<S>ab(?&S)ba|ab)", 1,
                      {{0, {t("ab"), n(0), t("ba")}}, {0, {t("ab")}}}});
    result.push_back({"correlated high bytes", "(?:\\x00\\xff|\\x80\\x00)", 1,
                      {{0, {t(std::string{char(0), char(255)})}},
                       {0, {t(std::string{char(128), char(0)})}}},
                      std::string{char(0), char(128), char(255)}});
    result.push_back({"fingerprint collision", "(?:aa|!!)", 1,
                      {{0, {t("aa")}}, {0, {t("!!")}}}, "a!"});
    // Generate grammar text and a separate production list from the same
    // symbolic rules; neither is derived from compiled summary fields.
    for (unsigned i = 0; i < 18; ++i) {
        const std::string left = i % 3 == 0 ? "" : (i % 3 == 1 ? "a" : "b");
        const std::string right = i % 2 == 0 ? "b" : "";
        const std::string base = i % 4 == 0 ? "" : (i % 4 == 1 ? "a" : "b");
        const std::string other = i % 5 == 0 ? "" : "a";
        const bool double_call = i % 3 == 0;
        const auto b_rule = double_call ? "(?&A)(?&A)" : "(?&A)";
        const std::string pattern = "(?(DEFINE)(?<A>" + left + "(?&B)" + right +
            "|" + base + ")(?<B>" + b_rule + "|" + other + "))(?&A)";
        std::vector<Production> productions{
            {0, {t(left), n(1), t(right)}}, {0, {t(base)}}, {1, {t(other)}}};
        productions.push_back({1, double_call ? std::vector<Symbol>{n(0), n(0)}
                                               : std::vector<Symbol>{n(0)}});
        result.push_back({"generated mutual " + std::to_string(i), pattern, 2, std::move(productions)});
    }
    return result;
}

#ifndef CRANK_REJECTION_BASELINE
// This oracle computes actual strings, not boundary sets. Truncation cannot
// hide a shorter derivation because concatenation only increases byte length.
// Runtime references are deliberately sampled by epsilon and every one-byte
// string; tests below separately require their abstract boundary to be top.
std::vector<Language> ast_languages(const crank::Ast& ast, std::size_t maximum) {
    std::vector<Language> result(ast.nodes.size());
    Language unknown{std::string{}};
    if (maximum)
        for (unsigned byte = 0; byte < 256; ++byte) unknown.insert(std::string(1, char(byte)));
    bool changed;
    do {
        changed = false;
        for (std::size_t id = 0; id < ast.nodes.size(); ++id) {
            const auto& node = ast.nodes[id];
            Language next;
            switch (node.kind) {
            case crank::Kind::Empty: case crank::Kind::Define: next.insert(""); break;
            case crank::Kind::CharSet:
                if (maximum)
                    for (unsigned byte = 0; byte < 256; ++byte)
                        if (node.characters.test(byte)) next.insert(std::string(1, char(byte)));
                break;
            case crank::Kind::Reference: next = unknown; break;
            case crank::Kind::Call:
                next = node.call_target < result.size() ? result[node.call_target] : unknown;
                break;
            case crank::Kind::Capture:
                next = node.children.empty() ? Language{std::string{}} : result.at(node.children.front());
                break;
            case crank::Kind::Concat:
                next.insert("");
                for (const auto child : node.children) next = product(next, result.at(child), maximum);
                break;
            case crank::Kind::Alt:
                for (const auto child : node.children)
                    next.insert(result.at(child).begin(), result.at(child).end());
                break;
            case crank::Kind::Repeat: {
                if (node.children.empty()) { next = unknown; break; }
                const bool dynamic = node.use_count != crank::no_id || node.count_path != crank::no_id;
                const auto lower = dynamic ? 0 : node.lower;
                const auto upper = dynamic ? std::optional<std::size_t>{} : node.upper;
                if (upper && *upper < lower) break;
                if (lower > 8 || (upper && *upper > 8))
                    throw std::runtime_error("bounded string oracle received huge repetition");
                Language exact{std::string{}};
                for (std::size_t count = 0; ; ++count) {
                    if (count >= lower) next.insert(exact.begin(), exact.end());
                    if (upper && count >= *upper) break;
                    auto following = product(exact, result.at(node.children.front()), maximum);
                    if (following.empty()) break;
                    if (!upper && count >= lower && following == exact) break;
                    exact = std::move(following);
                }
                break;
            }
            }
            const auto before = result[id].size();
            result[id].insert(next.begin(), next.end());
            changed |= result[id].size() != before;
        }
    } while (changed);
    return result;
}

void sound_pairs(const crank::Ast& ast, std::size_t maximum, const std::string& label) {
    const crank::OccurrenceAnalysis analysis(ast);
    const auto language = ast_languages(ast, maximum);
    ++analyses;
    for (std::size_t id = 0; id < ast.nodes.size(); ++id) {
        const auto& boundary = analysis.boundary(id);
        const auto& pairs = analysis.pairs(id);
        ++summaries;
        require(boundary.nullable == language[id].contains(""), label + ": exact nullable node " + std::to_string(id));
        for (const auto& text : language[id]) {
            ++derived_words;
            if (text.size() >= 2) {
                const auto first = pair_bit(static_cast<unsigned char>(text[0]),
                                            static_cast<unsigned char>(text[1]));
                const auto last = pair_bit(static_cast<unsigned char>(text[text.size() - 2]),
                                           static_cast<unsigned char>(text.back()));
                require((pairs.first & first) != 0, label + ": first pair removed derived word " + hex(text));
                require((pairs.last & last) != 0, label + ": last pair removed derived word " + hex(text));
            }
            if (text.empty()) continue;
            require(boundary.first.test(static_cast<unsigned char>(text.front())),
                    label + ": FIRST removed derived word " + hex(text));
            require(boundary.last.test(static_cast<unsigned char>(text.back())),
                    label + ": LAST removed derived word " + hex(text));
        }
    }
}

crank::NodeId add(crank::Ast& ast, crank::Kind kind, std::vector<crank::NodeId> children = {}) {
    crank::Node node;
    node.kind = kind;
    node.children = std::move(children);
    ast.nodes.push_back(std::move(node));
    return ast.nodes.size() - 1;
}
crank::NodeId byte_node(crank::Ast& ast, unsigned byte) {
    const auto id = add(ast, crank::Kind::CharSet);
    ast.nodes[id].characters.set(byte);
    return id;
}
crank::NodeId repeat(crank::Ast& ast, crank::NodeId child, std::size_t lower,
                     std::optional<std::size_t> upper) {
    const auto id = add(ast, crank::Kind::Repeat, {child});
    ast.nodes[id].lower = lower;
    ast.nodes[id].upper = upper;
    return id;
}

void exact_pairs() {
    struct PairExpected {
        std::string pattern;
        std::uint64_t first, last;
    };
    for (const auto& fixture : std::vector<PairExpected>{
            {"", 0, 0}, {"a", 0, 0}, {"ab", pair_mask({"ab"}), pair_mask({"ab"})},
            {"(?:ab|ba)", pair_mask({"ab", "ba"}), pair_mask({"ab", "ba"})},
            {"a?b?", pair_mask({"ab"}), pair_mask({"ab"})},
            {"a{0}", 0, 0}, {"a{1}", 0, 0},
            {"a{2}", pair_mask({"aa"}), pair_mask({"aa"})},
            {"a{0,1}", 0, 0}, {"a{0,2}", pair_mask({"aa"}), pair_mask({"aa"})},
            {"(?:a|){2}", pair_mask({"aa"}), pair_mask({"aa"})},
            {"(?<S>(?&S))", 0, 0},
            {"(?(DEFINE)(?<D>(?&D)))(?:(?&D)aa|ab)", pair_mask({"ab"}), pair_mask({"ab"})},
            {"(?(DEFINE)(?<D>(?&D)))(?:aa(?&D)|ab)", pair_mask({"ab"}), pair_mask({"ab"})}}) {
        const auto ast = crank::parse(fixture.pattern);
        const crank::OccurrenceAnalysis analysis(ast);
        const auto& pairs = analysis.pairs(ast.root);
        require(pairs.first == fixture.first && pairs.last == fixture.last,
                "exact pair fingerprint for " + fixture.pattern);
        sound_pairs(ast, 4, fixture.pattern);
    }
    struct Expected { std::string pattern, first, last; bool nullable; };
    for (const auto& fixture : std::vector<Expected>{
            {"", "", "", true}, {"a", "a", "a", false},
            {"a?b?", "ab", "ab", true}, {"a?ba?", "ab", "ab", false},
            {"a{0}", "", "", true}, {"(?:a|){2,3}", "a", "a", true},
            {"(?<S>a(?&S)b|)", "a", "b", true},
            {"(?<S>(?&S)a|b)", "b", "ab", false},
            {"(?<S>a(?&S)|b)", "ab", "b", false},
            {"(?<S>(?&S))", "", "", false},
            {"(?(DEFINE)(?<S>(?&S)))a", "a", "a", false},
            {"(?(DEFINE)(?<D>(?&D)))(?:(?&D)a|b)", "b", "b", false},
            {"(?(DEFINE)(?<D>(?&D)))(?:a(?&D)|b)", "b", "b", false}}) {
        const auto ast = crank::parse(fixture.pattern);
        const crank::OccurrenceAnalysis analysis(ast);
        const auto& boundary = analysis.boundary(ast.root);
        std::bitset<256> first, last;
        for (const unsigned char byte : fixture.first) first.set(byte);
        for (const unsigned char byte : fixture.last) last.set(byte);
        require(boundary.first == first && boundary.last == last && boundary.nullable == fixture.nullable,
                "exact root boundary for " + fixture.pattern);
        sound_pairs(ast, 5, fixture.pattern);
    }
    {
        crank::Ast ast;
        const auto empty = add(ast, crank::Kind::Empty);
        const auto dead = add(ast, crank::Kind::Alt);
        const auto empty_set = add(ast, crank::Kind::CharSet);
        const auto a = byte_node(ast, 'a');
        const auto zero = repeat(ast, dead, 0, 0);
        const auto zero_or_dead = repeat(ast, empty_set, 0, {});
        const auto positive_dead = repeat(ast, dead, 1, {});
        const auto nullable = add(ast, crank::Kind::Alt, {empty, a});
        const auto huge = repeat(ast, nullable, std::numeric_limits<std::size_t>::max(),
                                 std::numeric_limits<std::size_t>::max());
        const auto huge_empty = repeat(ast, empty, std::numeric_limits<std::size_t>::max(), {});
        const auto invalid = repeat(ast, nullable, 2, 1);
        const auto dynamic = repeat(ast, a, 2, 2);
        ast.nodes[dynamic].use_count = 0;
        const auto dynamic_path = repeat(ast, a, 2, 2);
        ast.nodes[dynamic_path].count_path = 0;
        const auto binding = repeat(ast, a, 2, 3);
        ast.nodes[binding].bind_count = 0;
        const auto reference = add(ast, crank::Kind::Reference);
        const auto forward = add(ast, crank::Kind::Reference);
        ast.nodes[forward].forward = true;
        const auto missing_call = add(ast, crank::Kind::Call);
        const auto missing_repeat = add(ast, crank::Kind::Repeat);
        const auto empty_capture = add(ast, crank::Kind::Capture);
        const auto define = add(ast, crank::Kind::Define, {reference});
        ast.root = add(ast, crank::Kind::Concat, {zero, a});
        const crank::OccurrenceAnalysis analysis(ast);
        for (const auto id : {zero, zero_or_dead, huge_empty, empty_capture, define}) {
            const auto& boundary = analysis.boundary(id);
            require(boundary.nullable && boundary.first.none() && boundary.last.none(),
                    "epsilon does not acquire a byte from a suppressed child");
        }
        for (const auto id : {dead, empty_set, positive_dead, invalid}) {
            const auto& boundary = analysis.boundary(id);
            require(!boundary.nullable && boundary.first.none() && boundary.last.none(),
                    "empty language is distinct from epsilon");
        }
        for (const auto id : {huge, dynamic, dynamic_path}) {
            const auto& boundary = analysis.boundary(id);
            require(boundary.nullable && boundary.first.count() == 1 && boundary.first.test('a') &&
                    boundary.last == boundary.first, "nullable or unresolved repeats retain body boundaries");
        }
        require(!analysis.boundary(binding).nullable && analysis.boundary(binding).first.test('a'),
                "binding retains positive declared lower bound");
        for (const auto id : {zero, zero_or_dead, huge_empty, empty_capture, define,
                              dead, empty_set, positive_dead, invalid})
            require(analysis.pairs(id).first == 0 && analysis.pairs(id).last == 0,
                    "non-two-byte languages do not acquire a pair from suppressed children");
        for (const auto id : {huge, dynamic, dynamic_path, binding})
            require((analysis.pairs(id).first & pair_mask({"aa"})) != 0 &&
                    (analysis.pairs(id).last & pair_mask({"aa"})) != 0,
                    "large and unresolved repeats keep possible cross-iteration pair");
        for (const auto id : {reference, forward, missing_call, missing_repeat}) {
            require(analysis.pairs(id).first == ~std::uint64_t{0} &&
                    analysis.pairs(id).last == ~std::uint64_t{0},
                    "unresolved references and malformed analysis-only links widen pair sets to top");
            const auto& boundary = analysis.boundary(id);
            require(boundary.nullable && boundary.first.all() && boundary.last.all(),
                    "unresolved reference or malformed analysis-only link widens to top");
        }
        for (const auto id : {ast.nodes.size(), crank::no_id}) {
            bool caught = false;
            try { (void)analysis.boundary(id); } catch (const std::out_of_range&) { caught = true; }
            require(caught, "boundary accessor checks invalid node ID");
            caught = false;
            try { (void)analysis.pairs(id); } catch (const std::out_of_range&) { caught = true; }
            require(caught, "pair accessor checks invalid node ID");
        }
        auto copied = analysis;
        auto moved = std::move(copied);
        for (std::size_t id = 0; id < ast.nodes.size(); ++id)
            require(moved.boundary(id).nullable == analysis.boundary(id).nullable &&
                    moved.boundary(id).first == analysis.boundary(id).first &&
                    moved.boundary(id).last == analysis.boundary(id).last,
                    "copied and moved analysis retains owned boundary facts");
        for (std::size_t id = 0; id < ast.nodes.size(); ++id)
            require(moved.pairs(id).first == analysis.pairs(id).first &&
                    moved.pairs(id).last == analysis.pairs(id).last,
                    "copied and moved analysis retains owned pair facts");
        const auto saved_root = ast.root;
        const auto* stable = &moved.pairs(saved_root);
        const auto expected_first = stable->first, expected_last = stable->last;
        ast.nodes.clear();
        require(&moved.pairs(saved_root) == stable,
                "analysis accessor remains a stable reference");
        require(stable->first == expected_first && stable->last == expected_last,
                "pair summaries own their storage independently of source AST");
    }
    {
        // Reverse dependencies exceed a single machine-word-sized pass count.
        crank::Ast ast;
        for (unsigned i = 0; i < 130; ++i) {
            const auto id = add(ast, crank::Kind::Call);
            ast.nodes[id].call_target = id + 1;
        }
        byte_node(ast, 255);
        ast.root = 0;
        sound_pairs(ast, 1, "long reverse dependency chain");
        const crank::OccurrenceAnalysis analysis(ast);
        require(!analysis.boundary(0).nullable && analysis.boundary(0).first.test(255) &&
                analysis.boundary(0).last.test(255), "boundary fixed point reaches first caller");
    }
    {
        crank::Ast ast;
        for (unsigned i = 0; i < 130; ++i) {
            const auto id = add(ast, crank::Kind::Call);
            ast.nodes[id].call_target = id + 1;
        }
        const auto concat = add(ast, crank::Kind::Concat);
        const auto zero = byte_node(ast, 0), high = byte_node(ast, 255);
        ast.nodes[concat].children = {zero, high};
        ast.root = 0;
        sound_pairs(ast, 2, "long pair reverse dependency chain");
        const crank::OccurrenceAnalysis analysis(ast);
        require(analysis.pairs(0).first == pair_bit(0, 255) &&
                analysis.pairs(0).last == pair_bit(0, 255),
                "pair fixed point reaches the first caller");
    }
    {
        // A saturated mask alone cannot catch a wrong hash or wrong byte
        // compression. Check each actual byte in each argument separately,
        // then check sparse Cartesian products against direct enumeration.
        crank::Ast ast;
        ast.root = add(ast, crank::Kind::Empty);
        std::vector<crank::NodeId> bytes;
        for (unsigned byte = 0; byte < 256; ++byte) bytes.push_back(byte_node(ast, byte));
        std::vector<std::pair<crank::NodeId, std::uint64_t>> expected;
        for (unsigned byte = 0; byte < 256; ++byte) {
            const auto first = add(ast, crank::Kind::Concat, {bytes[byte], bytes[255]});
            const auto last = add(ast, crank::Kind::Concat, {bytes[128], bytes[byte]});
            expected.push_back({first, pair_bit(byte, 255)});
            expected.push_back({last, pair_bit(128, byte)});
        }
        const std::vector<std::vector<unsigned>> classes{
            {0, 1, 63, 64, 127, 128, 255}, {2, 31, 32, 129, 254},
            {0, 64, 128, 192}, {17, 34, 51}, {63, 127, 191, 255}};
        for (std::size_t i = 0; i < classes.size(); ++i) {
            const auto& left = classes[i];
            const auto& right = classes[(i + 1) % classes.size()];
            const auto a = add(ast, crank::Kind::CharSet), b = add(ast, crank::Kind::CharSet);
            for (const auto byte : left) ast.nodes[a].characters.set(byte);
            for (const auto byte : right) ast.nodes[b].characters.set(byte);
            std::uint64_t mask = 0;
            for (const auto first : left)
                for (const auto last : right) mask |= pair_bit(first, last);
            expected.push_back({add(ast, crank::Kind::Concat, {a, b}), mask});
        }
        const crank::OccurrenceAnalysis analysis(ast);
        for (const auto& [id, mask] : expected)
            require(analysis.pairs(id).first == mask && analysis.pairs(id).last == mask,
                    "exact byte and sparse Cartesian pair projection at node " + std::to_string(id));
        sound_pairs(ast, 2, "all byte projections and sparse Cartesian classes");
    }
    {
        crank::Ast ast;
        const auto any = add(ast, crank::Kind::CharSet);
        ast.nodes[any].characters.set();
        ast.root = add(ast, crank::Kind::Concat, {any, any});
        sound_pairs(ast, 2, "all 65536 two-byte words");
        const crank::OccurrenceAnalysis analysis(ast);
        require(analysis.pairs(ast.root).first == ~std::uint64_t{0} &&
                analysis.pairs(ast.root).last == ~std::uint64_t{0},
                "full byte alphabet saturates fingerprints");
        require(analysis.pairs(any).first == 0 && analysis.pairs(any).last == 0,
                "a one-byte character set cannot yield a two-byte word");
    }
    for (const auto& fixture : fixtures()) sound_pairs(crank::parse(fixture.pattern), 4, fixture.name);
    for (const std::string pattern : {"(?:ab|c)?(?:de|f)?", "(?:a|ba){2}",
                                      "(?:ab|){0,1}", "(?:ab|){2}", "(?:ab)*",
                                      "(?:a?b?){2,3}", "a?(?:b|c?)d?"})
        sound_pairs(crank::parse(pattern), 5, "nullable and composed pair transfer " + pattern);
    for (const std::string pattern : {"(?<x>a|b)\\k<x>", "a{#n:1,3}b{=n}", "a{#n:1,3}b{=#0.@count}"})
        sound_pairs(crank::parse(pattern), 3, "stateful conservative summary " + pattern);
}

std::uint32_t random_state = 0x7b9c35a1U;
std::uint32_t random_value() {
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}
void generated_summaries() {
    for (unsigned serial = 0; serial < 32; ++serial) {
        crank::Ast ast;
        add(ast, crank::Kind::Empty);
        byte_node(ast, 'a');
        byte_node(ast, 'b');
        add(ast, crank::Kind::Alt);
        const auto forward = add(ast, crank::Kind::Call);
        for (unsigned step = 0; step < 28; ++step) {
            const auto size = ast.nodes.size();
            const auto a = random_value() % size, b = random_value() % size;
            switch (random_value() % 8) {
            case 0: add(ast, crank::Kind::Concat, {a, b}); break;
            case 1: add(ast, crank::Kind::Alt, {a, b}); break;
            case 2: repeat(ast, a, random_value() % 3, {}); break;
            case 3: repeat(ast, a, 0, random_value() % 4); break;
            case 4: add(ast, crank::Kind::Capture, {a}); break;
            case 5: {
                const auto call = add(ast, crank::Kind::Call);
                ast.nodes[call].call_target = step % 3 == 0 ? call : a;
                break;
            }
            case 6: add(ast, crank::Kind::Define, {a, b}); break;
            case 7: add(ast, crank::Kind::Alt, {forward, a, b}); break;
            }
        }
        ast.nodes[forward].call_target = serial % 4 == 0 ? forward : ast.nodes.size() - 1;
        ast.root = ast.nodes.size() - 1;
        sound_pairs(ast, 4, "generated AST graph " + std::to_string(serial));
    }
}

void rejection_work() {
    for (const auto& [pattern, subject] : std::vector<std::pair<std::string, std::string>>{
            {"(?:abz|cdz)", "adz"}, {"(?:abx|acy)", "aby"}}) {
        const auto ast = crank::parse(pattern);
        const crank::OccurrenceAnalysis analysis(ast);
        require(analysis.length(ast.root).accepts(subject.size()), "pair fixture passes old length filter");
        require(analysis.boundary(ast.root).first.test(static_cast<unsigned char>(subject.front())) &&
                analysis.boundary(ast.root).last.test(static_cast<unsigned char>(subject.back())),
                "pair fixture passes old individual boundary filter");
        const crank::OccurrenceProgram program(ast);
        crank::OccurrenceMatchOptions off;
        off.prune = false;
        crank::OccurrenceStatistics before, after;
        require(!program.full_match(subject, off, &before), "pair fixture rejects without optional pruning");
        require(!program.full_match(subject, {}, &after), "pair impossibility rejects same input");
        require(after.states == 0 && before.states > 0 && after.pruned > 0,
                "pair proof rejects before allocating a native request");
    }
    {
        const auto ast = crank::parse("(?:aa|!!)");
        const crank::OccurrenceAnalysis analysis(ast);
        require(pair_bit('a', 'a') == pair_bit('a', '!') && pair_bit('a', '!') == pair_bit('!', '!'),
                "collision fixture deliberately maps unequal pairs to one fingerprint");
        require((analysis.pairs(ast.root).first & pair_bit('a', '!')) != 0 &&
                (analysis.pairs(ast.root).last & pair_bit('a', '!')) != 0,
                "collision remains admitted by the necessary-condition filter");
        const crank::OccurrenceProgram program(ast);
        crank::OccurrenceStatistics statistics;
        require(!program.full_match("a!", {}, &statistics) && statistics.states > 0,
                "fingerprint collision never certifies a match");
        require(program.full_match("aa") && program.full_match("!!"), "collision fixture still accepts its language");
    }
    {
        const crank::OccurrenceProgram one_or_long(crank::parse("(?:a|ba)"));
        require(one_or_long.endpoints("aa") == std::vector<std::size_t>{1},
                "unrestricted pair filter preserves a one-byte result on a longer remaining subject");
        require(one_or_long.endpoints("baa") == std::vector<std::size_t>{2},
                "unrestricted pair filter does not read LAST from the subject tail");
        require(one_or_long.endpoints("zaa", 1) == std::vector<std::size_t>{2},
                "nonzero endpoint start retains a one-byte match");
        require(one_or_long.full_match("a"), "one-byte exact span never reads a second byte");
        const crank::OccurrenceProgram optional_pair(crank::parse("(?:ab|)"));
        require(optional_pair.endpoints("aa") == std::vector<std::size_t>{0},
                "unrestricted pair failure preserves an epsilon result");
        require(optional_pair.endpoints("x", 1) == std::vector<std::size_t>{1},
                "end-of-subject nullable invocation has no pair to inspect");
    }
    const crank::OccurrenceProgram program(crank::parse("(?<S>a(?&S)b|ab)"));
    for (const std::string& subject : {"x" + std::string(31, 'a') + std::string(32, 'b'),
                                      std::string(32, 'a') + std::string(31, 'b') + "x"}) {
        crank::OccurrenceStatistics baseline, filtered;
        crank::OccurrenceMatchOptions off;
        off.prune = false;
        require(!program.full_match(subject, off, &baseline), "rejection fixture rejects without optional pruning");
        require(!program.full_match(subject, {}, &filtered), "boundary proof rejects same input");
        require(filtered.states < baseline.states && filtered.work_steps < baseline.work_steps && filtered.pruned > 0,
                "FIRST or LAST proof avoids native requests on arithmetic-admissible bad input");
    }
    const crank::OccurrenceProgram nullable(crank::parse("(?<S>a(?&S)b|)"));
    require(nullable.endpoints("aabbx") == std::vector<std::size_t>({0, 4}),
            "unrestricted endpoint call must not inspect the enclosing tail as LAST");
    require(nullable.endpoints("x") == std::vector<std::size_t>({0}),
            "FIRST mismatch must preserve nullable endpoint");
    require(nullable.full_match(""), "zero-byte exact demand never dereferences subject boundaries");
}
#endif

crank::OccurrenceMatchOptions options(unsigned mask, bool prune) {
    crank::OccurrenceMatchOptions result;
    result.compress_returns = mask & 1U;
    result.collapse_control = mask & 2U;
    result.prune = prune;
    return result;
}

void runtime_equivalence() {
    for (const auto& fixture : fixtures()) {
        const auto ast = crank::parse(fixture.pattern);
        const crank::OccurrenceProgram program(ast);
        require(program.supports_shared_calls(), fixture.name + ": native sharing certificate");
        const auto language = grammar_languages(fixture.nonterminals, fixture.productions, 4).at(0);
        for (const auto& subject : words(fixture.alphabet, 4)) {
            const auto label = fixture.name + " subject=" + hex(subject);
            const bool expected = language.contains(subject);
            ++membership_cases;
            for (unsigned mask = 0; mask < 4; ++mask)
                for (const bool prune : {false, true}) {
                    crank::OccurrenceStatistics statistics;
                    require(program.full_match(subject, options(mask, prune), &statistics) == expected,
                            label + ": independent membership");
                    require(!statistics.limit_reached && statistics.proof_nodes == 0,
                            label + ": recognition is complete and does not construct scalar proofs");
                }
            for (std::size_t start = 0; start <= subject.size(); ++start) {
                std::vector<std::size_t> expected_ends;
                for (auto end = start; end <= subject.size(); ++end)
                    if (language.contains(subject.substr(start, end - start))) expected_ends.push_back(end);
                ++endpoint_cases;
                for (const unsigned mask : {0U, 3U})
                    for (const bool prune : {false, true})
                        require(program.endpoints(subject, start, options(mask, prune)) == expected_ends,
                                label + ": independent endpoints start=" + std::to_string(start));
            }
            if (subject.size() <= 2) {
                const auto witness = program.first_match(subject, options(0, true));
                require(witness.has_value() == expected, label + ": witness existence");
                for (unsigned mask = 1; mask < 4; ++mask)
                    require(program.first_match(subject, options(mask, true)) == witness,
                            label + ": Boolean-only boundary filters preserve scalar effects");
                ++witness_cases;
            }
        }
    }
    for (const auto& pattern : {"(?<S>a(?&S)b|)\\1", "a{#n:1,3}b{=n}",
                                "a{#n:1,3}b{=#0.@count}"}) {
        const crank::OccurrenceProgram program(crank::parse(pattern));
        require(!program.supports_shared_calls(), "runtime-dependent fields remain on history execution");
        for (const auto& subject : words("ab", 4)) {
            require(program.full_match(subject, options(3, true)) == program.full_match(subject, options(3, false)),
                    "history membership remains unchanged by optional boundary analysis");
            ++membership_cases;
        }
    }
    auto malformed = crank::parse("(?<S>a(?&S)b|)");
    for (auto& node : malformed.nodes)
        if (node.kind == crank::Kind::Call) node.call_target = crank::no_id;
    require(!crank::detail::AstMachineProgram::eligible(malformed), "malformed call still fails native eligibility");
    bool caught = false;
    try { crank::detail::AstMachineProgram rejected(malformed); }
    catch (const std::invalid_argument&) { caught = true; }
    require(caught, "invalid call is rejected before any new summary dereference");
}

void old_summary_records() {
    for (const auto& fixture : fixtures()) {
        const auto ast = crank::parse(fixture.pattern);
        const crank::OccurrenceAnalysis analysis(ast);
        for (std::size_t id = 0; id < ast.nodes.size(); ++id) {
            const auto& length = analysis.length(id);
            const auto& boundary = analysis.boundary(id);
            std::cout << fixture.name << " node=" << id << " length="
                      << length.productive << ',' << length.minimum << ',' << length.offset << ','
                      << length.modulus << ',' << length.residues << " nullable=" << boundary.nullable
                      << " first=" << boundary.first << " last=" << boundary.last << " suffix=";
            const auto& minima = analysis.suffix_minimum(id);
            for (std::size_t i = 0; i < minima.size(); ++i) {
                const auto& suffix = analysis.suffix_length(id, i);
                std::cout << minima[i] << ':' << suffix.productive << ',' << suffix.minimum << ','
                          << suffix.offset << ',' << suffix.modulus << ',' << suffix.residues << ';';
            }
            std::cout << '\n';
        }
        const auto& height = analysis.height_equation();
        std::cout << fixture.name << " height=" << height.has_value();
        if (height) {
            std::cout << ':' << height->capture_id << ',' << height->offset << ',' << height->coefficient << ':';
            for (const auto path : height->count_paths) std::cout << path << ',';
        }
        std::cout << '\n';
    }
}

void statistics_record(const crank::OccurrenceStatistics& s) {
    std::cout << " stats=" << s.states << ',' << s.work_steps << ',' << s.down_steps << ',' << s.up_steps
        << ',' << s.pruned << ',' << s.arithmetic_pruned << ',' << s.path_checks << ',' << s.forward_obligations
        << ',' << s.max_call_depth << ',' << s.occurrences << ',' << s.memo_entries << ',' << s.memo_hits
        << ',' << s.return_links << ',' << s.completions << ',' << s.replay_steps << ',' << s.proof_nodes
        << ',' << s.max_pending << ',' << s.compression_lookups << ',' << s.compression_hops
        << ',' << s.compressed_completions << ',' << s.compression_cycles << ',' << s.control_redirects
        << ',' << s.ordered_positions << ',' << s.duplicate_completions << ',' << s.limit_reached;
}

// Compare this transcript byte-for-byte against the frozen core. In witness
// mode the optimization is disabled, so capture/proof scheduling and counters
// must remain identical as well as accepted language.
void witness_records() {
    for (const auto& fixture : fixtures()) {
        const crank::OccurrenceProgram program(crank::parse(fixture.pattern));
        for (const auto& subject : words(fixture.alphabet, 4)) {
            crank::OccurrenceStatistics statistics;
            const auto witness = program.first_match(subject, {}, &statistics);
            std::cout << fixture.name << " subject=" << hex(subject) << " match=" << witness.has_value();
            if (witness) {
                std::cout << " end=" << witness->end << " captures=";
                for (const auto& span : witness->captures) {
                    if (span) std::cout << span->begin << ':' << span->end;
                    else std::cout << '-';
                    std::cout << ',';
                }
                std::cout << " counts=";
                for (const auto count : witness->counts) {
                    if (count) std::cout << *count;
                    else std::cout << '-';
                    std::cout << ',';
                }
            }
            statistics_record(statistics);
            std::cout << '\n';
        }
    }
    for (const std::string pattern : {"(?<S>a(?&S)b|)\\1", "a{#n:1,3}b{=n}",
                                      "a{#n:1,3}b{=#0.@count}"}) {
        const crank::OccurrenceProgram program(crank::parse(pattern));
        for (const auto& subject : words("ab", 4)) {
            crank::OccurrenceStatistics statistics;
            const auto witness = program.first_match(subject, {}, &statistics);
            std::cout << "history pattern=" << pattern << " subject=" << hex(subject)
                      << " match=" << witness.has_value();
            if (witness) {
                std::cout << " end=" << witness->end << " captures=";
                for (const auto& span : witness->captures) {
                    if (span) std::cout << span->begin << ':' << span->end;
                    else std::cout << '-';
                    std::cout << ',';
                }
                std::cout << " counts=";
                for (const auto count : witness->counts) {
                    if (count) std::cout << *count;
                    else std::cout << '-';
                    std::cout << ',';
                }
            }
            statistics_record(statistics);
            std::cout << '\n';
        }
    }
}

// With optional pruning disabled, both recognition paths must retain their
// original answers and work counts. This isolates accidental integration
// changes from the intentional decrease in work with the filter enabled.
void no_prune_records() {
    for (const auto& fixture : fixtures()) {
        const crank::OccurrenceProgram program(crank::parse(fixture.pattern));
        for (const auto& subject : words(fixture.alphabet, 3)) {
            for (const unsigned mask : {0U, 3U}) {
                const auto strategy = options(mask, false);
                crank::OccurrenceStatistics statistics;
                const auto found = program.full_match(subject, strategy, &statistics);
                std::cout << fixture.name << " subject=" << hex(subject) << " mask=" << mask
                          << " full=" << found;
                statistics_record(statistics);
                std::cout << '\n';
                const auto ends = program.endpoints(subject, 0, strategy, &statistics);
                std::cout << fixture.name << " subject=" << hex(subject) << " mask=" << mask << " endpoints=";
                for (const auto end : ends) std::cout << end << ',';
                statistics_record(statistics);
                std::cout << '\n';
            }
        }
    }
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--witness-record") {
            witness_records();
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--no-prune-record") {
            no_prune_records();
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--summary-record") {
            old_summary_records();
            return 0;
        }
        if (argc != 1) throw std::runtime_error("usage: test_rejection [--witness-record|--no-prune-record|--summary-record]");
#ifndef CRANK_REJECTION_BASELINE
        exact_pairs();
        generated_summaries();
        rejection_work();
#endif
        runtime_equivalence();
        std::cout << "rejection checks=" << checks << " analyses=" << analyses << " summaries=" << summaries
                  << " derived_words=" << derived_words << " membership=" << membership_cases
                  << " endpoints=" << endpoint_cases << " witnesses=" << witness_cases << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "rejection test failure: " << error.what() << '\n';
        return 1;
    }
}
