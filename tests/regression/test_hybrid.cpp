#include "occurrence_matcher.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
std::size_t checks = 0, grammar_cases = 0, generated_grammars = 0;
std::size_t subject_cases = 0, endpoint_cases = 0, witness_cases = 0;

void require(bool condition, const std::string& context) {
    ++checks;
    if (!condition) throw std::runtime_error(context);
}

template<class Exception, class Function>
void expect_exception(Function&& function, const std::string& context) {
    bool caught = false;
    try { function(); } catch (const Exception&) { caught = true; }
    require(caught, context);
}

// All strategy combinations run through the same public API. Keeping this
// helper central also makes the ablation choices obvious in failure messages.
crank::OccurrenceMatchOptions strategy(unsigned mask, bool prune = true) {
    crank::OccurrenceMatchOptions options;
    options.compress_returns = (mask & 1U) != 0;
    options.collapse_control = (mask & 2U) != 0;
    options.prune = prune;
    return options;
}

std::string variant(unsigned mask, bool prune) {
    return " [compression=" + std::to_string(mask & 1U) +
        ", fusion=" + std::to_string((mask >> 1U) & 1U) +
        ", arithmetic=" + std::to_string(prune) + "]";
}

std::vector<std::string> words(std::string_view alphabet, unsigned maximum) {
    std::vector<std::string> result{std::string{}};
    std::size_t begin = 0, end = 1;
    for (unsigned length = 0; length < maximum; ++length) {
        for (auto index = begin; index < end; ++index)
            for (char byte : alphabet) result.push_back(result[index] + byte);
        begin = end;
        end = result.size();
    }
    return result;
}

struct Symbol {
    bool terminal;
    std::size_t nonterminal = 0;
    std::string text;
};
Symbol n(std::size_t id) { return {false, id, {}}; }
Symbol t(std::string text) { return {true, 0, std::move(text)}; }
struct Production { std::size_t lhs; std::vector<Symbol> rhs; };
using Language = std::set<std::string>;

// Denotational oracle adapted from engine/test_native.cpp. Each nonterminal
// starts at the empty language; concatenation and union monotonically add
// words until the least fixed point. Length truncation is sound because these
// grammars concatenate terminals and never delete bytes. This algorithm has
// no AST, input positions, continuations, compressed paths, or chart states.
std::vector<Language> bounded_languages(std::size_t nonterminals,
                                       const std::vector<Production>& productions,
                                       std::size_t maximum) {
    std::vector<Language> languages(nonterminals);
    bool changed;
    do {
        changed = false;
        for (const auto& production : productions) {
            Language partial{std::string{}};
            for (const auto& symbol : production.rhs) {
                const Language terminal{symbol.text};
                const auto& suffixes = symbol.terminal ? terminal : languages.at(symbol.nonterminal);
                Language next;
                for (const auto& prefix : partial)
                    for (const auto& suffix : suffixes)
                        if (prefix.size() + suffix.size() <= maximum)
                            next.insert(prefix + suffix);
                partial = std::move(next);
            }
            auto& destination = languages.at(production.lhs);
            const auto before = destination.size();
            destination.insert(partial.begin(), partial.end());
            changed |= destination.size() != before;
        }
    } while (changed);
    return languages;
}

struct Fixture {
    std::string name, pattern, alphabet;
    unsigned maximum;
    std::size_t nonterminals;
    std::vector<Production> productions;
};

std::vector<std::size_t> oracle_endpoints(const Language& language,
                                        std::string_view subject, std::size_t start) {
    std::vector<std::size_t> expected;
    for (auto end = start; end <= subject.size(); ++end)
        if (language.contains(std::string(subject.substr(start, end - start))))
            expected.push_back(end);
    return expected;
}

void compare_fixture(const Fixture& fixture, bool all_offsets = true) {
    const auto ast = crank::parse(fixture.pattern);
    const crank::OccurrenceProgram program(ast);
    require(program.supports_shared_calls(), fixture.name + ": CFG eligibility");
    const auto languages = bounded_languages(fixture.nonterminals, fixture.productions,
                                            fixture.maximum);
    ++grammar_cases;
    for (const auto& subject : words(fixture.alphabet, fixture.maximum)) {
        const auto context = fixture.name + ": '" + subject + "'";
        const bool expected = languages.at(0).contains(subject);
        ++subject_cases;
        require(program.full_match(subject) == expected, context + ": default independent membership");
        for (unsigned mask = 0; mask < 4; ++mask) {
            for (const bool prune : {false, true}) {
                const auto options = strategy(mask, prune);
                crank::OccurrenceStatistics statistics;
                require(program.full_match(subject, options, &statistics) == expected,
                        context + variant(mask, prune) + ": independent membership");
                require(!statistics.limit_reached && statistics.proof_nodes == 0,
                        context + variant(mask, prune) + ": complete recognition without proofs");
            }
        }

        const auto last_start = all_offsets || subject.size() <= 2 ? subject.size() : 0;
        for (std::size_t start = 0; start <= last_start; ++start) {
            const auto expected_ends = oracle_endpoints(languages[0], subject, start);
            ++endpoint_cases;
            for (unsigned mask = 0; mask < 4; ++mask)
                for (const bool prune : {false, true}) {
                    crank::OccurrenceStatistics statistics;
                    require(program.endpoints(subject, start, strategy(mask, prune), &statistics) == expected_ends,
                            context + variant(mask, prune) + ": independent endpoints at " + std::to_string(start));
                    require(!statistics.limit_reached && statistics.proof_nodes == 0,
                            context + variant(mask, prune) + ": endpoint query does not construct proofs");
                }
        }

        // Both optimizations are recognition-only. A scalar witness retains
        // the original finite proof and deterministic completion ordering.
        if (subject.size() <= 2) {
            const auto original = program.first_match(subject, strategy(0));
            require(original.has_value() == expected, context + ": witness independent membership");
            if (original) {
                require(original->end == subject.size() &&
                        original->captures.at(0) == crank::Span{0, subject.size()},
                        context + ": full witness span");
                for (const auto& span : original->captures)
                    require(!span || (span->begin <= span->end && span->end <= subject.size()),
                            context + ": valid scalar capture boundaries");
            }
            for (unsigned mask = 1; mask < 4; ++mask)
                require(program.first_match(subject, strategy(mask)) == original,
                        context + variant(mask, true) + ": witness preserves fallback ordering and effects");
            ++witness_cases;
        }
    }
    for (unsigned mask = 0; mask < 4; ++mask)
        expect_exception<std::out_of_range>([&] { (void)program.endpoints("ab", 3, strategy(mask)); },
                                            fixture.name + ": invalid endpoint start is rejected");
}

void representative_grammars() {
    const std::vector<Fixture> fixtures{
        {"right recursion", "(?<S>a(?&S)|)", "ab", 5, 1,
            {{0, {t("a"), n(0)}}, {0, {}}}},
        {"left recursion", "(?<S>(?&S)a|)", "ab", 5, 1,
            {{0, {n(0), t("a")}}, {0, {}}}},
        {"nested equal halves", "(?<S>a(?&S)b|)", "ab", 6, 1,
            {{0, {t("a"), n(0), t("b")}}, {0, {}}}},
        {"ambiguous nonregular nesting", "(?<S>a(?&S)b|aa(?&S)bb|)", "ab", 6, 1,
            {{0, {t("a"), n(0), t("b")}}, {0, {t("aa"), n(0), t("bb")}}, {0, {}}}},
        {"balanced concatenation", "(?<S>a(?&S)b(?&S)|)", "ab", 6, 1,
            {{0, {t("a"), n(0), t("b"), n(0)}}, {0, {}}}},
        {"nullable mutual cycle", "(?(DEFINE)(?<A>(?&B)|a)(?<B>(?&A)|))(?&A)", "ab", 4, 2,
            {{0, {n(1)}}, {0, {t("a")}}, {1, {n(0)}}, {1, {}}}},
        {"mutual consuming right recursion", "(?(DEFINE)(?<A>a(?&B)|)(?<B>b(?&A)|))(?&A)", "ab", 5, 2,
            {{0, {t("a"), n(1)}}, {0, {}}, {1, {t("b"), n(0)}}, {1, {}}}},
        {"nonnullable mutual unit cycle", "(?(DEFINE)(?<A>(?&B))(?<B>(?&A)|b))(?&A)", "ab", 4, 2,
            {{0, {n(1)}}, {1, {n(0)}}, {1, {t("b")}}}},
        {"empty self cycle", "(?<S>(?&S))", "ab", 4, 1,
            {{0, {n(0)}}}},
        {"empty mutual cycle", "(?(DEFINE)(?<A>(?&B))(?<B>(?&A)))(?&A)", "ab", 4, 2,
            {{0, {n(1)}}, {1, {n(0)}}}},
        {"nullable self cycle", "(?<S>(?&S)|)", "ab", 4, 1,
            {{0, {n(0)}}, {0, {}}}},
        {"nullable Catalan", "(?<S>(?&S)(?&S)|a|)", "ab", 5, 1,
            {{0, {n(0), n(0)}}, {0, {t("a")}}, {0, {}}}},
        {"nonnullable Catalan", "(?<S>(?&S)(?&S)|a)", "ab", 5, 1,
            {{0, {n(0), n(0)}}, {0, {t("a")}}}},
        {"nullable terminal suffix", "(?<S>a(?&S)b?|)", "ab", 5, 1,
            {{0, {t("a"), n(0)}}, {0, {t("a"), n(0), t("b")}}, {0, {}}}},
        {"nullable recursive suffix", "(?(DEFINE)(?<S>a(?&S)(?&N)|)(?<N>(?&N)|))(?&S)", "ab", 5, 2,
            {{0, {t("a"), n(0), n(1)}}, {0, {}}, {1, {n(1)}}, {1, {}}}},
        {"empty suffix with nested captures", "(?<S>a(?&S)(?<N>(?:))|)", "ab", 5, 1,
            {{0, {t("a"), n(0)}}, {0, {}}}},
        {"recursive bounded repeat", "(?<S>a(?&S){0,2}b|)", "ab", 5, 1,
            {{0, {t("ab")}}, {0, {t("a"), n(0), t("b")}},
             {0, {t("a"), n(0), n(0), t("b")}}, {0, {}}}},
        {"recursive unbounded repeat", "(?<S>(?:a(?&S)b)*)", "ab", 5, 1,
            {{0, {t("a"), n(0), t("b"), n(0)}}, {0, {}}}},
        {"whole pattern recursion", "a(?R)b|", "ab", 5, 1,
            {{0, {t("a"), n(0), t("b")}}, {0, {}}}},
        {"numbered recursion", "(?<S>a(?1)b|)", "ab", 5, 1,
            {{0, {t("a"), n(0), t("b")}}, {0, {}}}},
        {"unused dead definition and zero repeat", "(?(DEFINE)(?<dead>(?&dead)))(?&dead){0}(?:ab|ba)", "ab", 4, 1,
            {{0, {t("ab")}}, {0, {t("ba")}}}},
        {"zero length only repeated rule", "(?(DEFINE)(?<E>(?&E)|))(?&E){2,4}", "ab", 4, 1,
            {{0, {}}}}
    };
    for (const auto& fixture : fixtures) compare_fixture(fixture);
}

std::string render_grammar(std::size_t nonterminals, const std::vector<Production>& productions) {
    std::string pattern = "(?(DEFINE)";
    for (std::size_t id = 0; id < nonterminals; ++id) {
        pattern += "(?<R" + std::to_string(id) + ">";
        bool first = true;
        for (const auto& production : productions) {
            if (production.lhs != id) continue;
            if (!first) pattern += '|';
            first = false;
            for (const auto& symbol : production.rhs)
                pattern += symbol.terminal ? symbol.text : "(?&R" + std::to_string(symbol.nonterminal) + ")";
        }
        require(!first, "generated grammar defines every nonterminal");
        pattern += ')';
    }
    return pattern + ")(?&R0)";
}

void generated_fixed_point_oracle() {
    const std::vector<std::vector<Symbol>> alternatives{
        {}, {t("a")}, {t("b")}, {n(0)}, {n(1)},
        {n(0), t("a")}, {t("a"), n(0)}, {n(1), t("b")}, {t("b"), n(1)},
        {n(0), n(1)}, {n(1), n(0)}, {n(0), n(0)}, {n(1), n(1)},
        {t("a"), n(0), t("b")}, {t("aa"), n(0), t("bb")},
        {t("a"), n(0), n(1)}, {n(0), n(1), t("b")}
    };
    std::uint32_t seed = 0x7e01cafeU;
    const auto next = [&]() { seed = seed * 1664525U + 1013904223U; return seed; };
    for (unsigned example = 0; example < 64; ++example) {
        std::vector<Production> productions;
        for (std::size_t rule = 0; rule < 2; ++rule) {
            const auto count = 1U + next() % 3U;
            for (unsigned alternative = 0; alternative < count; ++alternative)
                productions.push_back({rule, alternatives[next() % alternatives.size()]});
        }
        compare_fixture({"generated grammar " + std::to_string(example),
                         render_grammar(2, productions), "ab", 4, 2, productions}, false);
        ++generated_grammars;
    }
}

void late_waiters_and_endpoint_demands() {
    // Both branches eventually request A at the same position. A chain of
    // unit rules delays the second subscription until earlier results exist.
    // Reversing alternatives exercises both old-result replay and new-result
    // delivery. Unequal suffixes also separate exact full-match demands.
    const std::vector<Production> rules{
        {0, {n(1), t("b")}}, {0, {n(2), t("c")}},
        {1, {t("a"), n(1)}}, {1, {}},
        {2, {n(3)}}, {3, {n(4)}}, {4, {n(1)}}
    };
    for (const bool reverse : {false, true}) {
        auto ordered = rules;
        if (reverse) std::reverse(ordered.begin(), ordered.end());
        compare_fixture({reverse ? "late waiter reversed" : "late waiter forward",
                         render_grammar(5, ordered), "abc", 4, 5, ordered});
    }

    const std::vector<Fixture> demands{
        {"late competing waiter through empty captures",
            "(?(DEFINE)(?<A>a))(?:(?&A)x|(?<pad>(?<pad2>))(?&A)y)", "axy", 3, 2,
            {{0, {n(1), t("x")}}, {0, {n(1), t("y")}}, {1, {t("a")}}}},
        {"late competing waiter through empty captures reversed",
            "(?(DEFINE)(?<A>a))(?:(?<pad>(?<pad2>))(?&A)y|(?&A)x)", "axy", 3, 2,
            {{0, {n(1), t("y")}}, {0, {n(1), t("x")}}, {1, {t("a")}}}},
        {"different exact demands long first", "(?(DEFINE)(?<A>a(?&A)|))(?:(?&A)aa|(?&A)a)", "ab", 5, 2,
            {{0, {n(1), t("aa")}}, {0, {n(1), t("a")}}, {1, {t("a"), n(1)}}, {1, {}}}},
        {"different exact demands short first", "(?(DEFINE)(?<A>a(?&A)|))(?:(?&A)a|(?&A)aa)", "ab", 5, 2,
            {{0, {n(1), t("a")}}, {0, {n(1), t("aa")}}, {1, {t("a"), n(1)}}, {1, {}}}},
        {"same callee in two concatenation positions", "(?(DEFINE)(?<A>a(?&A)|))(?&A)(?&A)", "ab", 5, 2,
            {{0, {n(1), n(1)}}, {1, {t("a"), n(1)}}, {1, {}}}},
        {"nested rule with unrestricted follower", "(?(DEFINE)(?<A>a(?&A)b|))(?&A)(?&A)", "ab", 6, 2,
            {{0, {n(1), n(1)}}, {1, {t("a"), n(1), t("b")}}, {1, {}}}},
        {"nullable late caller", "(?(DEFINE)(?<A>(?&A)|)(?<B>(?&C))(?<C>(?&A)))(?:(?&A)a|(?&B)b)", "ab", 4, 4,
            {{0, {n(1), t("a")}}, {0, {n(2), t("b")}}, {1, {n(1)}}, {1, {}},
             {2, {n(3)}}, {3, {n(1)}}}}
    };
    for (const auto& fixture : demands) compare_fixture(fixture);
}

std::size_t capture_id(const crank::Ast& ast, std::string_view name) {
    const auto found = std::find(ast.capture_names.begin(), ast.capture_names.end(), name);
    require(found != ast.capture_names.end(), "fixture capture exists");
    return static_cast<std::size_t>(found - ast.capture_names.begin());
}

void capture_fallback_boundaries() {
    struct Case {
        std::string pattern, subject, name;
        crank::Span expected;
    };
    const std::vector<Case> fixtures{
        {"(?<S>a(?&S)b|)", "aaabbb", "S", {0, 6}},
        {"(?<S>a(?&S)|)", "aaa", "S", {0, 3}},
        {"(?(DEFINE)(?<item>a))(?&item)(?&item)", "aa", "item", {1, 2}},
        {"(?(DEFINE)(?<item>ab))(?&item){2,3}", "ababab", "item", {4, 6}},
        {"(?<S>a(?&S)(?<E>)|)", "aaa", "E", {3, 3}}
    };
    for (const auto& fixture : fixtures) {
        const auto ast = crank::parse(fixture.pattern);
        const crank::OccurrenceProgram program(ast);
        const auto original = program.first_match(fixture.subject, strategy(0));
        require(original && original->captures.at(capture_id(ast, fixture.name)) == fixture.expected,
                "independent capture completion effect: " + fixture.pattern);
        for (unsigned mask = 0; mask < 4; ++mask) {
            crank::OccurrenceStatistics statistics;
            require(program.first_match(fixture.subject, strategy(mask), &statistics) == original,
                    "capture first-witness fallback is identical: " + fixture.pattern + variant(mask, true));
            require(statistics.proof_nodes > 0, "witness still records capture effects");
            require(statistics.compressed_completions == 0 && statistics.control_redirects == 0,
                    "scalar witness retains uncompressed control and return provenance");
        }
        ++witness_cases;
    }
}

void byte_offsets_arithmetic_and_limits() {
    const crank::OccurrenceProgram program(crank::parse("(?<S>[\\x00-\\xff](?&S)|)"));
    std::string all_bytes;
    for (unsigned byte = 0; byte < 256; ++byte) all_bytes += static_cast<char>(byte);
    for (unsigned mask = 0; mask < 4; ++mask) {
        require(program.full_match(all_bytes, strategy(mask)), "recursive recognition accepts all 256 bytes");
        const std::string subject = std::string("!\0", 2) + std::string("\xff\x80", 2);
        require(program.endpoints(subject, 1, strategy(mask)) == std::vector<std::size_t>{1, 2, 3, 4},
                "nonzero offsets retain NUL and high byte endpoints");
        crank::OccurrenceStatistics statistics;
        auto options = strategy(mask);
        options.max_states = 0;
        expect_exception<crank::ResourceLimit>([&] { (void)program.endpoints("aa", 0, options, &statistics); },
                                              "state exhaustion remains explicit");
        require(statistics.limit_reached, "state limit is reported through statistics");
        options = strategy(mask);
        options.max_steps = 0;
        expect_exception<crank::ResourceLimit>([&] { (void)program.full_match("aa", options, &statistics); },
                                              "work exhaustion remains explicit");
        require(statistics.limit_reached, "work limit is reported through statistics");
    }

    const crank::OccurrenceProgram nested(crank::parse("(?<S>a(?&S)b|)"));
    for (unsigned mask = 0; mask < 4; ++mask) {
        crank::OccurrenceStatistics statistics;
        require(!nested.full_match("aaabb", strategy(mask), &statistics),
                "odd length nested near miss rejects");
        require(statistics.arithmetic_pruned > 0 && statistics.memo_entries == 0,
                "global arithmetic certificate still rejects before recursive work");
        require(!nested.full_match("aaabb", strategy(mask, false)),
                "disabling arithmetic does not change rejection");
    }
}

void strategy_activation_diagnostics() {
    constexpr std::size_t length = 80;
    const crank::OccurrenceProgram program(crank::parse("(?<S>a(?&S)|)"));
    const std::string subject(length, 'a');
    std::vector<std::size_t> expected;
    for (std::size_t end = 0; end <= length; ++end) expected.push_back(end);
    for (unsigned mask = 0; mask < 4; ++mask) {
        crank::OccurrenceStatistics statistics;
        require(program.endpoints(subject, 0, strategy(mask), &statistics) == expected,
                "right-recursive size probe returns every prefix");
        require((statistics.compressed_completions != 0) == ((mask & 1U) != 0),
                "compression switch activates the intended return-chain optimization");
        require((statistics.control_redirects != 0) == ((mask & 2U) != 0),
                "control switch activates the intended structural optimization");
        std::cout << "right recursion, 80 bytes" << variant(mask, true)
                  << ": " << statistics.states << " state units, " << statistics.work_steps
                  << " work steps, " << statistics.completions << " retained completions, "
                  << statistics.compressed_completions << " compressed completions, "
                  << statistics.control_redirects << " control redirects\n";
    }
    // Activation and exact answers are correctness requirements. No timing,
    // asymptotic theorem, or speed-ratio assertion belongs in this test gate.
}
} // namespace

int main() {
    try {
        representative_grammars();
        generated_fixed_point_oracle();
        late_waiters_and_endpoint_demands();
        capture_fallback_boundaries();
        byte_offsets_arithmetic_and_limits();
        strategy_activation_diagnostics();
        std::cout << "Hybrid core: " << checks << " checks passed; " << grammar_cases
                  << " grammar fixtures, including " << generated_grammars << " generated grammars; "
                  << subject_cases << " independent membership subjects; " << endpoint_cases
                  << " independent endpoint queries; " << witness_cases << " scalar witness cases.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Hybrid core failure after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
