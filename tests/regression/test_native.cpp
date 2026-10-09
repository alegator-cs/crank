#include "ast_machine.hpp"
#include "crank.hpp"
#include "occurrence_matcher.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
std::size_t checks = 0, oracle_cases = 0, endpoint_cases = 0, generated_grammars = 0;

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

std::vector<std::string> words(std::string_view alphabet, unsigned maximum) {
    std::vector<std::string> result{std::string{}};
    std::size_t begin = 0, end = 1;
    for (unsigned length = 0; length < maximum; ++length) {
        for (auto i = begin; i < end; ++i)
            for (char byte : alphabet) result.push_back(result[i] + byte);
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

// Denotational oracle over finite sets of words. This has no input positions,
// AST nodes, calls, return links, continuations, or Earley chart operations.
// All rules start at the empty language and grow to the least fixed point.
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
    // When true, capture i+1 corresponds to oracle nonterminal i.
    bool named_rule_captures = true;
};

void compare_fixture(const Fixture& fixture, bool exhaustive_endpoints) {
    const auto ast = crank::parse(fixture.pattern);
    require(crank::detail::AstMachineProgram::eligible(ast), fixture.name + ": native eligibility");
    const crank::OccurrenceProgram native(ast);
    const auto languages = bounded_languages(fixture.nonterminals, fixture.productions,
                                            fixture.maximum);
    for (const auto& subject : words(fixture.alphabet, fixture.maximum)) {
        const bool expected = languages.at(0).contains(subject);
        const auto context = fixture.name + ": subject '" + subject + "'";
        for (const bool prune : {false, true}) {
            crank::OccurrenceMatchOptions options;
            options.prune = prune;
            crank::OccurrenceStatistics stats;
            require(native.full_match(subject, options, &stats) == expected,
                    context + ": independent membership");
            require(!stats.limit_reached, context + ": completed membership is not incomplete");
            require(stats.proof_nodes == 0, context + ": recognition does not retain proof nodes");
        }
        ++oracle_cases;
        if (exhaustive_endpoints || subject.size() <= 2) {
            for (std::size_t start = 0; start <= subject.size(); ++start) {
                std::vector<std::size_t> expected_ends;
                for (auto end = start; end <= subject.size(); ++end)
                    if (languages.at(0).contains(subject.substr(start, end - start)))
                        expected_ends.push_back(end);
                for (const bool prune : {false, true}) {
                    crank::OccurrenceMatchOptions options;
                    options.prune = prune;
                    require(native.endpoints(subject, start, options) == expected_ends,
                            context + ": exact endpoints at " + std::to_string(start));
                }
                ++endpoint_cases;
            }
        }
        if (subject.size() <= 3) {
            crank::OccurrenceStatistics stats;
            const auto witness = native.first_match(subject, {}, &stats);
            require(witness.has_value() == expected, context + ": witness membership");
            if (witness) {
                require(witness->end == subject.size() &&
                        witness->captures.at(0) == crank::Span{0, subject.size()},
                        context + ": full witness span");
                require(witness->counts.empty(), context + ": no fabricated count bindings");
                require(witness->captures.size() == ast.capture_names.size(),
                        context + ": capture slot count");
                for (std::size_t id = 1; id < witness->captures.size(); ++id) {
                    const auto& span = witness->captures[id];
                    require(!span || (span->begin <= span->end && span->end <= subject.size()),
                            context + ": capture boundaries");
                    if (span && fixture.named_rule_captures && id <= languages.size())
                        require(languages[id - 1].contains(subject.substr(span->begin, span->end - span->begin)),
                                context + ": captured invocation belongs to its independent rule language");
                }
            }
        }
    }
    expect_exception<std::out_of_range>([&] { (void)native.endpoints("ab", 3); },
                                        fixture.name + ": invalid start");
}

void representative_grammars() {
    const std::vector<Fixture> fixtures{
        {"right recursion", "(?<S>a(?&S)|)", "ab", 5, 1,
            {{0, {t("a"), n(0)}}, {0, {}}}},
        {"left recursion", "(?<S>(?&S)a|)", "ab", 5, 1,
            {{0, {n(0), t("a")}}, {0, {}}}},
        {"mutual nullable cycle", "(?(DEFINE)(?<A>(?&B)|a)(?<B>(?&A)|))(?&A)", "ab", 5, 2,
            {{0, {n(1)}}, {0, {t("a")}}, {1, {n(0)}}, {1, {}}}},
        {"nonnullable mutual cycle", "(?(DEFINE)(?<A>(?&B))(?<B>(?&A)|b))(?&A)", "ab", 4, 2,
            {{0, {n(1)}}, {1, {n(0)}}, {1, {t("b")}}}},
        {"empty self cycle", "(?<S>(?&S))", "ab", 4, 1,
            {{0, {n(0)}}}},
        {"empty mutual cycle", "(?(DEFINE)(?<A>(?&B))(?<B>(?&A)))(?&A)", "ab", 4, 2,
            {{0, {n(1)}}, {1, {n(0)}}}},
        {"nullable self cycle", "(?<S>(?&S)|)", "ab", 4, 1,
            {{0, {n(0)}}, {0, {}}}},
        {"ambiguous nullable tree", "(?<S>(?&S)(?&S)|a|)", "ab", 5, 1,
            {{0, {n(0), n(0)}}, {0, {t("a")}}, {0, {}}}},
        {"balanced nested and following", "(?<S>a(?&S)b(?&S)|)", "ab", 6, 1,
            {{0, {t("a"), n(0), t("b"), n(0)}}, {0, {}}}},
        {"palindromes", "(?<S>a(?&S)a|b(?&S)b|a|b|)", "ab", 5, 1,
            {{0, {t("a"), n(0), t("a")}}, {0, {t("b"), n(0), t("b")}},
             {0, {t("a")}}, {0, {t("b")}}, {0, {}}}},
        {"bounded recursive repetition", "(?<S>a(?&S){0,2}b|)", "ab", 6, 1,
            {{0, {t("a"), t("b")}}, {0, {t("a"), n(0), t("b")}},
             {0, {t("a"), n(0), n(0), t("b")}}, {0, {}}}},
        {"whole pattern call", "a(?R)b|", "ab", 5, 1,
            {{0, {t("a"), n(0), t("b")}}, {0, {}}}, false},
        {"numbered call", "(?<S>a(?1)b|)", "ab", 5, 1,
            {{0, {t("a"), n(0), t("b")}}, {0, {}}}},
        {"unused unproductive definition", "(?(DEFINE)(?<unused>(?&unused)))(?:ab|ba)", "ab", 4, 1,
            {{0, {t("ab")}}, {0, {t("ba")}}}, false},
        {"left recursive expression",
            "(?(DEFINE)(?<E>(?&E)\\+(?&T)|(?&T))(?<T>\\((?&E)\\)|0))(?&E)", "0+()", 4, 2,
            {{0, {n(0), t("+"), n(1)}}, {0, {n(1)}},
             {1, {t("("), n(0), t(")")}}, {1, {t("0")}}}}
    };
    for (const auto& fixture : fixtures) compare_fixture(fixture, true);
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
        pattern += ')';
    }
    return pattern + ")(?&R0)";
}

void generated_fixed_point_oracle() {
    const std::vector<std::vector<Symbol>> alternatives{
        {}, {t("a")}, {t("b")}, {n(0)}, {n(1)},
        {n(0), t("a")}, {t("a"), n(0)}, {n(1), t("b")}, {t("b"), n(1)},
        {n(0), n(1)}, {n(1), n(0)}, {n(0), n(0)}, {n(1), n(1)},
        {t("a"), n(0), t("b")}, {t("b"), n(1), t("a")}
    };
    // Fixed integer PRNG: reproducible grammar generation independent of
    // compiler/library random-distribution implementations.
    std::uint32_t seed = 0x31415926U;
    const auto next = [&]() { seed = seed * 1664525U + 1013904223U; return seed; };
    for (unsigned example = 0; example < 80; ++example) {
        std::vector<Production> productions;
        for (std::size_t rule = 0; rule < 2; ++rule) {
            const auto count = 1U + next() % 3U;
            for (unsigned i = 0; i < count; ++i)
                productions.push_back({rule, alternatives[next() % alternatives.size()]});
        }
        const Fixture fixture{"generated grammar " + std::to_string(example),
                              render_grammar(2, productions), "ab", 4, 2, productions};
        compare_fixture(fixture, false);
        ++generated_grammars;
    }
}

void endpoint_demand_isolation() {
    // Same callee/start under unequal required endpoints. Each fixture has a
    // separate finite-language specification, independent of matcher tables.
    const std::vector<Fixture> fixtures{
        {"right recursion, longer suffix first",
         "(?(DEFINE)(?<S>a(?&S)|))(?:(?&S)aa|(?&S)a)", "ab", 6, 2,
         {{0,{n(1),t("aa")}},{0,{n(1),t("a")}},{1,{t("a"),n(1)}},{1,{}}}, false},
        {"right recursion, shorter suffix first",
         "(?(DEFINE)(?<S>a(?&S)|))(?:(?&S)a|(?&S)aa)", "ab", 6, 2,
         {{0,{n(1),t("a")}},{0,{n(1),t("aa")}},{1,{t("a"),n(1)}},{1,{}}}, false},
        {"two unrestricted right recursive calls",
         "(?(DEFINE)(?<S>a(?&S)|))(?&S)(?&S)", "ab", 6, 2,
         {{0,{n(1),n(1)}},{1,{t("a"),n(1)}},{1,{}}}, false},
        {"left recursive call with distinct suffixes",
         "(?(DEFINE)(?<S>(?&S)a|))(?:(?&S)a|(?&S)aa)", "ab", 6, 2,
         {{0,{n(1),t("a")}},{0,{n(1),t("aa")}},{1,{n(1),t("a")}},{1,{}}}, false},
        {"balanced call with or without suffix",
         "(?(DEFINE)(?<S>a(?&S)b|))(?:(?&S)ab|(?&S))", "ab", 6, 2,
         {{0,{n(1),t("ab")}},{0,{n(1)}},{1,{t("a"),n(1),t("b")}},{1,{}}}, false},
        {"two balanced calls",
         "(?(DEFINE)(?<S>a(?&S)b|))(?&S)(?&S)", "ab", 6, 2,
         {{0,{n(1),n(1)}},{1,{t("a"),n(1),t("b")}},{1,{}}}, false}
    };
    for (const auto& fixture : fixtures) compare_fixture(fixture, true);
}

void capture_proofs_and_generic_boundary() {
    const std::vector<std::pair<std::string, std::string>> fixtures{
        {"(?(DEFINE)(?<S>a(?&S)b|)(?<unused>x))(?&S)", "aaabbb"},
        {"(?(DEFINE)(?<item>a))(?&item)(?&item)", "aa"},
        {"(?(DEFINE)(?<item>ab))(?&item){2,3}", "ababab"},
        {"(?<S>(?&S)a|)", "aaa"},
        {"a(?R)b|", "aabb"}
    };
    for (const auto& [pattern, subject] : fixtures) {
        const auto ast = crank::parse(pattern);
        const crank::OccurrenceProgram native(ast);
            crank::OccurrenceStatistics stats;
        const auto witness = native.first_match(subject, {}, &stats);
        require(witness.has_value(), "unambiguous capture fixture has a finite witness");
        require(witness == native.first_match(subject), "first proof discovery is deterministic");
        require(ast.capture_names.size() > 1 ? stats.proof_nodes > 0 : stats.proof_nodes == 0,
                "capture proof storage follows observable non-root capture effects: " + pattern);
        crank::OccurrenceMatchOptions generic;
        generic.share_calls = false;
        require(native.full_match(subject, generic), "legacy continuation execution remains available");
        require(native.first_match(subject, generic) == witness,
                "unambiguous native and generic witnesses agree");
    }

    const crank::OccurrenceProgram siblings(crank::parse(
        "(?(DEFINE)(?<item>a))(?&item)(?&item)"));
    require(siblings.first_match("aa")->captures.at(1) == crank::Span{1, 2},
            "later sibling completion overwrites earlier capture");
    const crank::OccurrenceProgram nested(crank::parse("(?<S>a(?&S)b|)"));
    require(nested.first_match("aaabbb")->captures.at(1) == crank::Span{0, 6},
            "outer recursive completion overwrites inner capture");
    const crank::OccurrenceProgram repeated_nullable(crank::parse("(?<S>(?&S)|){0,5}"));
    require(repeated_nullable.first_match("").has_value(), "nullable repetitions retain a finite proof");

    // The shared recognizer is not used to merge environments whose scalar
    // outcomes or complete occurrence histories are observable.
    const crank::OccurrenceProgram ambiguous(crank::parse("(a*)(a*)"));
    const auto outcomes = ambiguous.match_outcomes("aa");
    require(outcomes.size() == 3, "all-outcome query preserves equal-end unequal-capture histories");
    crank::OccurrenceMatchOptions generic;
    generic.share_calls = false;
    require(outcomes == ambiguous.match_outcomes("aa", 0, true, generic),
            "shared-call option does not project all-outcome environments");
    const auto trace = nested.first_traced_match("aabb");
    require(trace && trace->occurrences.size() >= 4,
            "trace query retains separate recursive invocation occurrences");
    require(trace->outcome.captures.at(1) == crank::Span{0, 4},
            "trace remains consistent with capture closure");
}

void bytes_and_deep_calls() {
    const crank::OccurrenceProgram bytes(crank::parse("(?<S>[\\x00-\\xff](?&S)|)"));
    std::string all_bytes;
    for (unsigned byte = 0; byte < 256; ++byte) {
        const std::string one(1, static_cast<char>(byte));
        require(bytes.full_match(one), "recursive terminal accepts every unsigned byte");
        require(bytes.endpoints(one) == std::vector<std::size_t>{0, 1},
                "nullable byte recursion preserves empty and consuming endpoints");
        all_bytes += one;
    }
    require(bytes.full_match(all_bytes), "embedded NUL and high bytes preserve input length");

    constexpr std::size_t depth = 2048;
    std::string subject(depth, 'a');
    subject.append(depth, 'b');
    const crank::OccurrenceProgram nested(crank::parse("(?<S>a(?&S)b|)"));
    crank::OccurrenceMatchOptions options;
    options.max_states = 3000000;
    options.max_steps = 12000000;
    options.max_call_depth = 1;
    options.max_repeat = 0;
    require(nested.full_match(subject, options),
            "shared calls use heap return links and are not capped by generic history depth");
    const auto witness = nested.first_match(subject, options);
    require(witness && witness->captures.at(1) == crank::Span{0, subject.size()},
            "deep witness reconstruction is iterative and closes outer capture last");
    subject.back() = 'a';
    require(!nested.full_match(subject, options), "deep late mismatch is a complete rejection");
}

void numeric_repeat_control() {
    const crank::OccurrenceProgram zero(crank::parse(
        "(?(DEFINE)(?<unit>))(?&unit){0}"));
    const auto zero_witness = zero.first_match("");
    require(zero_witness && !zero_witness->captures.at(1),
            "zero repetition performs no capture completion");
    const crank::OccurrenceProgram unused_cycle(crank::parse("(?<S>(?&S)){0}"));
    require(unused_cycle.full_match("") && !unused_cycle.first_match("")->captures.at(1),
            "zero repetition skips an unproductive recursive body");
    const crank::OccurrenceProgram mandatory_cycle(crank::parse("(?<S>(?&S)){1}"));
    require(!mandatory_cycle.full_match(""), "mandatory unproductive recursion cannot become epsilon");
    for (const auto& [lower, upper] : std::vector<std::pair<std::size_t, std::size_t>>{
            {0, 0}, {0, 5}, {2, 7}, {8, 8}, {17, 19}, {64, 65}}) {
        const crank::OccurrenceProgram native(crank::parse(
            "(?(DEFINE)(?<unit>ab))(?&unit){" + std::to_string(lower) + "," +
            std::to_string(upper) + "}"));
        std::string subject;
        for (std::size_t count = 0; count <= upper + 1; ++count) {
            require(native.full_match(subject) == (lower <= count && count <= upper),
                    "native numeric repeat control obeys both bounds");
            require(!native.full_match(subject + 'b'), "numeric repeat retains terminal order");
            subject += "ab";
        }
    }
    const crank::OccurrenceProgram mandatory(crank::parse(
        "(?(DEFINE)(?<unit>a))(?&unit){1000000000}"));
    require(!mandatory.full_match("a"), "huge mandatory consuming count rejects by minimum length");
    const crank::OccurrenceProgram optional(crank::parse(
        "(?(DEFINE)(?<unit>a))(?&unit){0,1000000000}"));
    require(optional.full_match("aaa"), "huge optional count is bounded by actual input consumption");
    for (const auto bound : {std::size_t{1000000000}, std::numeric_limits<std::size_t>::max()}) {
        const crank::OccurrenceProgram nullable(crank::parse(
            "(?(DEFINE)(?<unit>))(?&unit){" + std::to_string(bound) + "}"));
        require(nullable.full_match(""), "huge fixed zero-width repetition recognizes without unrolling");
        const auto witness = nullable.first_match("");
        require(witness && witness->captures.at(1) == crank::Span{0, 0},
                "zero-width repetition preserves its last capture without materializing occurrence history");
        require(!nullable.full_match("a"), "zero-width repeat cannot invent input consumption");
    }
    for (const auto& suffix : {"*", "+", "{3,}", "{2,7}"}) {
        const crank::OccurrenceProgram nullable(crank::parse(
            "(?(DEFINE)(?<unit>(?&unit)|a|))(?&unit)" + std::string(suffix)));
        for (const auto& subject : words("ab", 4))
            require(nullable.full_match(subject) == (subject.find('b') == std::string::npos),
                    "nullable recursive repeat takes the finite language fixed point");
    }
}

void eligibility_and_budgets() {
    for (const auto& pattern : {"(?<S>a(?&S)b|)\\1", "(?<S>a(?&S)b|)c{#n}d{=n}",
                                "(?<S>a(?&S)b|)c{=S[0].@height}",
                                "(?(DEFINE)(?<unused>a{#n}))a"}) {
        auto ast = crank::parse(pattern);
        ast.has_references = ast.has_counts = ast.has_calls = false;
        ast.has_extended = ast.has_occurrence_paths = false;
        for (auto& node : ast.nodes) node.stateful = false;
        require(!crank::detail::AstMachineProgram::eligible(ast),
                "eligibility inspects syntax and rejects forged stateful summary flags");
        expect_exception<std::invalid_argument>([&] { crank::detail::AstMachineProgram rejected(ast); },
                                                "direct native constructor rejects stateful executable syntax");
    }
    auto corrupt = crank::parse("(?<S>a(?&S)b|)");
    for (auto& node : corrupt.nodes)
        if (node.kind == crank::Kind::Call) node.call_target = crank::no_id;
    require(!crank::detail::AstMachineProgram::eligible(corrupt), "invalid call target is ineligible");
    expect_exception<std::invalid_argument>([&] { crank::detail::AstMachineProgram rejected(corrupt); },
                                            "invalid call target rejects before execution");
    corrupt = crank::parse("a");
    corrupt.nodes.at(corrupt.root).bind_count = 0;
    require(!crank::detail::AstMachineProgram::eligible(corrupt),
            "unexpected count fields cannot bypass eligibility on a terminal node");

    const crank::OccurrenceProgram program(crank::parse("(?<S>a(?&S)b|)"));
    crank::OccurrenceStatistics reference, stats;
    require(program.full_match("aaabbb", {}, &reference), "budget baseline matches");
    require(reference.states > 0 && reference.work_steps > 0, "native allocation and work are counted");
    for (const bool states : {false, true}) {
        crank::OccurrenceMatchOptions options;
        if (states) options.max_states = 0;
        else options.max_steps = 0;
        stats = {};
        expect_exception<crank::ResourceLimit>([&] { (void)program.full_match("aaabbb", options, &stats); },
                                              "exhausted recognition budget is not a false negative");
        require(stats.limit_reached, "recognition budget exhaustion retains diagnostics");
        stats = {};
        expect_exception<crank::ResourceLimit>([&] { (void)program.endpoints("xaaabbbx", 1, options, &stats); },
                                              "exhausted endpoint budget is not partial enumeration");
        require(stats.limit_reached, "endpoint budget exhaustion retains diagnostics");
        stats = {};
        expect_exception<crank::ResourceLimit>([&] { (void)program.first_match("aaabbb", options, &stats); },
                                              "exhausted witness budget is not an absent witness");
        require(stats.limit_reached, "witness budget exhaustion retains diagnostics");
    }
}

void public_scalar_witness_contracts() {
    for (const auto& pattern : {"(a*)(a*)", "(a|b)*", "([ab]*)\\1", "\\1([ab]*)",
                                "a{#n}b{=n}", "(a|b){#n}c{=n}"}) {
        const crank::Program program(pattern);
        for (const auto& subject : words("abc", 4)) {
            const auto outcomes = program.outcomes(subject);
            for (const bool optimized : {false, true}) {
                crank::RunOptions options;
                options.optimize = optimized;
                const auto witness = program.first_match(subject, options);
                require(witness.has_value() == !outcomes.empty(),
                        "nonrecursive first witness agrees with complete outcome existence");
                if (witness)
                    require(std::find(outcomes.begin(), outcomes.end(), *witness) != outcomes.end(),
                            "ordinary/backreference/count first witness is an actual complete outcome");
            }
        }
    }

}

void public_dispatch() {
    const crank::Program program("(?<S>a(?&S)b|)");
    crank::RunStatistics stats;
    require(program.full_match("aabb", {}, &stats) && stats.backend == "native-ast",
            "Auto recursive recognition uses the AST-native core");
    require(stats.occurrence.memo_entries > 0,
            "Auto recursive recognition retains shared native calls");
    require(program.endpoints("xaabby", 1, {}, &stats) == std::vector<std::size_t>{1, 5} &&
            stats.backend == "native-ast", "Auto endpoints use the same native core");
    require(program.first_match("aabb", {}, &stats).has_value() &&
            stats.backend == "native-ast-witness", "Auto witness uses native capture proof");
    for (const bool shared : {false, true}) {
        crank::RunOptions options;
        options.backend = crank::Backend::Occurrences;
        options.occurrence.share_calls = shared;
        require(program.full_match("aabb", options, &stats) &&
                stats.backend == (shared ? "native-ast" : "occurrences"),
                "explicit Occurrences exposes shared and generic continuation execution");
    }
    for (const auto& [pattern, subject] : std::vector<std::pair<std::string, std::string>>{
            {"(?<S>a(?&S)b|)\\1", "aabbaabb"},
            {"(?<S>a(?&S)b|)c{#n}d{=n}", "aabbccdd"},
            {"(?<S>a(?&S)b|)c{=S[0].@height}", "aabbcc"}}) {
        const crank::Program stateful(pattern);
        require(stateful.full_match(subject, {}, &stats) && stats.backend == "occurrences",
                "observable recursive constraints use the unified VM");
        require(stateful.ast().has_occurrence_paths
                    ? (stats.occurrence.occurrences > 0 && stats.occurrence.memo_entries == 0)
                    : (stats.occurrence.occurrences == 0 && stats.occurrence.memo_entries > 0),
                "explicit paths retain histories; latest bindings use environment-aware scalar memo");
        const auto invalid = subject == "aabbaabb" ? "aabbaaba" :
                             subject == "aabbccdd" ? "aabbccd" : "aabbc";
        require(!stateful.full_match(invalid), "recursive observations still constrain recognition");
    }
}
} // namespace

int main() {
    try {
        representative_grammars();
        generated_fixed_point_oracle();
        endpoint_demand_isolation();
        capture_proofs_and_generic_boundary();
        bytes_and_deep_calls();
        numeric_repeat_control();
        eligibility_and_budgets();
        public_dispatch();
        public_scalar_witness_contracts();
        std::cout << "PASS: " << checks << " native core checks, " << oracle_cases
                  << " independent fixed-point cases, " << endpoint_cases
                  << " exact endpoint cases, " << generated_grammars
                  << " generated grammars\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
