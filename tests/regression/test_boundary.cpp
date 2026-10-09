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

#ifndef CRANK_BOUNDARY_BASELINE
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

void sound_boundaries(const crank::Ast& ast, std::size_t maximum, const std::string& label) {
    const crank::OccurrenceAnalysis analysis(ast);
    const auto language = ast_languages(ast, maximum);
    ++analyses;
    for (std::size_t id = 0; id < ast.nodes.size(); ++id) {
        const auto& boundary = analysis.boundary(id);
        ++summaries;
        require(boundary.nullable == language[id].contains(""), label + ": exact nullable node " + std::to_string(id));
        for (const auto& text : language[id]) {
            ++derived_words;
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

void exact_boundaries() {
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
        sound_boundaries(ast, 5, fixture.pattern);
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
        for (const auto id : {reference, forward, missing_call, missing_repeat}) {
            const auto& boundary = analysis.boundary(id);
            require(boundary.nullable && boundary.first.all() && boundary.last.all(),
                    "unresolved reference or malformed analysis-only link widens to top");
        }
        for (const auto id : {ast.nodes.size(), crank::no_id}) {
            bool caught = false;
            try { (void)analysis.boundary(id); } catch (const std::out_of_range&) { caught = true; }
            require(caught, "boundary accessor checks invalid node ID");
        }
        auto copied = analysis;
        auto moved = std::move(copied);
        for (std::size_t id = 0; id < ast.nodes.size(); ++id)
            require(moved.boundary(id).nullable == analysis.boundary(id).nullable &&
                    moved.boundary(id).first == analysis.boundary(id).first &&
                    moved.boundary(id).last == analysis.boundary(id).last,
                    "copied and moved analysis retains owned boundary facts");
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
        sound_boundaries(ast, 1, "long reverse dependency chain");
        const crank::OccurrenceAnalysis analysis(ast);
        require(!analysis.boundary(0).nullable && analysis.boundary(0).first.test(255) &&
                analysis.boundary(0).last.test(255), "boundary fixed point reaches first caller");
    }
    for (const auto& fixture : fixtures()) sound_boundaries(crank::parse(fixture.pattern), 4, fixture.name);
    for (const std::string pattern : {"(?<x>a|b)\\k<x>", "a{#n:1,3}b{=n}", "a{#n:1,3}b{=#0.@count}"})
        sound_boundaries(crank::parse(pattern), 3, "stateful conservative summary " + pattern);
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
        sound_boundaries(ast, 4, "generated AST graph " + std::to_string(serial));
    }
}

void rejection_work() {
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
        if (argc != 1) throw std::runtime_error("usage: test_boundary [--witness-record|--no-prune-record]");
#ifndef CRANK_BOUNDARY_BASELINE
        exact_boundaries();
        generated_summaries();
        rejection_work();
#endif
        runtime_equivalence();
        std::cout << "boundary checks=" << checks << " analyses=" << analyses << " summaries=" << summaries
                  << " derived_words=" << derived_words << " membership=" << membership_cases
                  << " endpoints=" << endpoint_cases << " witnesses=" << witness_cases << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "boundary test failure: " << error.what() << '\n';
        return 1;
    }
}
