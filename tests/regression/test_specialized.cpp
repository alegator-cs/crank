#include "crank.hpp"
#include "count_matcher.hpp"
#include "equality_matcher.hpp"

#include <algorithm>
#include <functional>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
std::size_t checks = 0, differential_cases = 0, construction_cases = 0;

void require(bool condition, const std::string& context) {
    ++checks;
    if (!condition) throw std::runtime_error(context);
}

template<class Exception, class Function>
void expect_exception(Function&& function, const std::string& context) {
    bool caught = false;
    try { function(); }
    catch (const Exception&) { caught = true; }
    require(caught, context);
}

std::vector<std::string> words(const std::string& alphabet, unsigned maximum) {
    std::vector<std::string> result{std::string{}};
    std::size_t begin = 0, end = 1;
    for (unsigned length = 0; length < maximum; ++length) {
        for (std::size_t i = begin; i < end; ++i)
            for (char byte : alphabet) result.push_back(result[i] + byte);
        begin = end;
        end = result.size();
    }
    return result;
}

crank::MatchOptions unpruned() {
    crank::MatchOptions options;
    options.prune = false;
    return options;
}

void differential(const crank::Program& program, const std::string& pattern,
                  const std::string& subject) {
    const auto complete = crank::match_states(program.ast(), subject, 0, true, unpruned());
    const bool expected = !complete.empty();
    const std::string context = "pattern " + pattern + ", subject '" + subject + "'";
    require(program.full_match(subject) == expected, "Auto differs: " + context);
    crank::RunOptions forced;
    forced.backend = crank::Backend::States;
    require(program.full_match(subject, forced) == expected,
            "projected States differs: " + context);
    require(crank::match_exists(program.ast(), subject) == expected,
            "match_exists differs: " + context);
    require(crank::match_exists(program.ast(), subject, 0, true, unpruned()) == expected,
            "unpruned match_exists differs: " + context);
    ++differential_cases;
}

void constructed(const std::string& pattern, const std::string& subject, bool expected) {
    const crank::Program program(pattern);
    require(program.full_match(subject) == expected,
            "independent construction: " + pattern + " on '" + subject + "'");
    require(!crank::match_states(program.ast(), subject, 0, true, unpruned()).empty() == expected,
            "complete outcomes disagree with independent construction: " + pattern);
    ++construction_cases;
}

void plan_eligibility() {
    for (const auto& pattern : std::vector<std::string>{
             "a{#n}b{=n}c{=n}", "(?:ab|ba){#n}(?:ac|ca){=n}",
             "!(ab){#n}\\?(cd){=n}\\.", "(a|b){#unused}",
             "(?:a*){0}a{#n}b{=n}"}) {
        const crank::Program program(pattern);
        require(crank::make_count_plan(program.ast()).has_value(),
                "missing count plan: " + pattern);
        require(program.explain().find("exact shared-count plan:") != std::string::npos,
                "count plan missing from explain: " + pattern);
    }
    for (const auto& pattern : std::vector<std::string>{
             "(?:a{#n}b{=n})*", "a{#n}b{#m}a{=n}b{=m}",
             "(?:a{#n})?b{=n}", "(?:a|ab){#n}b{=n}",
             "a*(a{#n}b{=n})", "(?<x>a{#n})b{=n}\\k<x>"}) {
        const crank::Program program(pattern);
        require(!crank::make_count_plan(program.ast()), "unsound count eligibility: " + pattern);
        require(program.explain().find("exact shared-count plan:") == std::string::npos,
                "unexpected count explain certificate: " + pattern);
    }
    for (const auto& pattern : std::vector<std::string>{
             "(?<x>a*)\\k<x>", "\\k<x>(?<x>a|b)",
             "\\k<x>:(?<x>(?:ab|ba)*):\\k<x>",
             "(?<unused>(?<x>(a|b)*))#\\k<x>",
             "(?:ab|ba)(?<x>a*)\\k<x>(?:ab|ba)"}) {
        const crank::Program program(pattern);
        require(crank::make_equality_plan(program.ast()).has_value(),
                "missing equality plan: " + pattern);
        const auto explanation = program.explain();
        require(explanation.find("Equality plan:") != std::string::npos &&
                    explanation.find("Equality plan: not eligible") == std::string::npos,
                "equality plan missing from explain: " + pattern);
    }
    for (const auto& pattern : std::vector<std::string>{
             "(?<x>a)?\\k<x>", "(?:\\k<x>(?<x>a|b))*",
             "\\k<x>(?:(?<x>a)|b)", "(?<x>a*)a*\\k<x>",
             "(?<x>a)(?<y>b)\\k<x>\\k<y>", "(?<x>a{#n})\\k<x>"}) {
        const crank::Program program(pattern);
        require(!crank::make_equality_plan(program.ast()),
                "unsound equality eligibility: " + pattern);
        require(program.explain().find("Equality plan: not eligible") != std::string::npos,
                "unexpected equality explain certificate: " + pattern);
    }
    crank::RegularOptions tiny;
    tiny.max_nfa_states = 1;
    require(!crank::make_count_plan(crank::parse("(ab){#n}c{=n}"), tiny),
            "count compiler limit must decline its certificate");
    require(!crank::make_equality_plan(crank::parse("(?<x>(ab)*)\\k<x>"), tiny),
            "equality compiler limit must decline its certificate");
}

void short_properties() {
    const std::vector<std::string> binary_patterns{
        "a{#n}b{=n}", "(a|b){#n}a{=n}",
        "(?:ab|ba){#n}(?:ab|ba){=n}", "a{#n}a{=n}",
        "(?:a{#n}b{=n})*", "(?:a{#unused})*", "(?:a{#unused}b)*",
        "a{#n}(?:b{#m}a{=m})*b{=n}",
        "(?:(?<x>a|b)\\k<x>)*", "(?:\\k<x>(?<x>a|b))*",
        "\\k<x>(?:(?<x>a)|b)", "(?<x>a)?\\k<x>",
        "(?<x>a*)\\k<x>", "\\k<x>(?<x>a*)", "(?<x>(a|b)*)\\k<x>",
        "(?<unused>(?<x>a|b)\\k<x>)", "(?<x>a|b)(a?)*\\k<x>",
        "((a?)){0,3}\\2", "((a)?b?){0,3}\\2", "((a?)){2,4}",
        "((a|b)*)*", "(?:(a{#unused})(?<x>b))?\\k<x>",
        "((a){#unused})*", "(?:(a|b){#n})?a{=n}",
        "(?:(\\k<x>)(?<x>(a|b)))*", "((a)?b){#unused}\\2",
        "((?:\\3)(a)(b))*"
    };
    const std::vector<std::string> ternary_patterns{
        "a{#n}b{=n}c{=n}", "(ab){#n}c{=n}", "a{#n}(b|c){=n}",
        "a{#n}b{#m}c{=n}a{=m}", "(?<x>a|b)c\\k<x>",
        "\\k<x>c(?<x>a|b)", "(?<x>a*)b\\k<x>c\\k<x>",
        "(?<x>(a|b)*)c\\k<x>",
        "(?<ignore>a{#dead})(?<x>b{#live})c{=live}\\k<x>",
        "(a{#n}b){#unused}c{=n}"
    };
    for (const auto& [patterns, subjects] : {
             std::pair{binary_patterns, words("ab", 5)},
             std::pair{ternary_patterns, words("abc", 4)}}) {
        for (const auto& pattern : patterns) {
            const crank::Program program(pattern);
            for (const auto& subject : subjects) differential(program, pattern, subject);
        }
    }
    std::mt19937 random(0x4352414eU);
    for (unsigned case_id = 0; case_id < 320; ++case_id) {
        const bool ternary = case_id % 2;
        const auto& patterns = ternary ? ternary_patterns : binary_patterns;
        const auto& pattern = patterns[random() % patterns.size()];
        std::string subject;
        const auto length = random() % 8;
        for (unsigned i = 0; i < length; ++i)
            subject += static_cast<char>('a' + random() % (ternary ? 3 : 2));
        differential(crank::Program(pattern), pattern, subject);
    }
}

void independent_constructions() {
    for (std::size_t n = 0; n <= 9; ++n) {
        const auto a = std::string(n, 'a'), b = std::string(n, 'b'), c = std::string(n, 'c');
        constructed("a{#n}b{=n}c{=n}", a + b + c, true);
        if (n) {
            constructed("a{#n}b{=n}c{=n}", a + c + b, false);
            constructed("a{#n}b{=n}c{=n}", std::string(n + 1, 'a') +
                        std::string(n - 1, 'b') + c, false);
        }
        std::string left, right;
        for (std::size_t i = 0; i < n; ++i) {
            left += i % 2 ? "ab" : "ba";
            right += i % 2 ? "ca" : "ac";
        }
        constructed("(?:ab|ba){#n}(?:ac|ca){=n}", left + right, true);
        if (n) {
            left[0] = left[1]; // Same length and count equation; invalid first body.
            constructed("(?:ab|ba){#n}(?:ac|ca){=n}", left + right, false);
        }
    }
    for (const auto& word : words("ab", 5)) {
        bool body_member = word.size() % 2 == 0;
        for (std::size_t i = 0; i + 1 < word.size(); i += 2)
            body_member = body_member && word[i] != word[i + 1];
        constructed("(?<x>(?:ab|ba)*)#\\k<x>", word + '#' + word, body_member);
        constructed("\\k<x>:(?<x>(?:ab|ba)*):\\k<x>",
                    word + ':' + word + ':' + word, body_member);
        if (!word.empty()) {
            auto changed = word;
            changed.back() = changed.back() == 'a' ? 'b' : 'a';
            constructed("(?<x>(?:a|b)*)#\\k<x>", word + '#' + changed, false);
        }
    }
    for (const auto& subject : words("ab", 5)) {
        bool adjacent_pairs = subject.size() % 2 == 0;
        for (std::size_t i = 0; i + 1 < subject.size(); i += 2)
            adjacent_pairs = adjacent_pairs && subject[i] == subject[i + 1];
        constructed("(?:\\k<x>(?<x>a|b))*", subject, adjacent_pairs);
    }
    constructed("(?<x>a)?\\k<x>", "", false); // Unset differs from captured epsilon.
    constructed("(?<x>a?)\\k<x>", "", true);
    constructed("\\k<x>(?:(?<x>a)|b)", "b", false); // Undischarged obligation.
    constructed("((a)?b){#unused}\\2", "abba", true); // Skipped inner capture retains a.
}

void offsets_and_limits() {
    for (const auto& pattern : std::vector<std::string>{
             "(?<x>a*)\\k<x>", "\\k<x>(?<x>a|b)", "a{#n}b{=n}",
             "(?:a{#unused})*", "((a)?b){#unused}\\2", "((a?)){1,3}"}) {
        const crank::Program program(pattern);
        const auto& ast = program.ast();
        const std::string subject = "abaabb";
        for (std::size_t start = 0; start <= subject.size(); ++start) {
            for (bool full : {false, true}) {
                const bool expected = !crank::match_states(ast, subject, start, full, unpruned()).empty();
                require(crank::match_exists(ast, subject, start, full) == expected,
                        "start/full existence semantics: " + pattern);
                require(crank::match_exists(ast, subject, start, full, unpruned()) == expected,
                        "unpruned start/full existence semantics: " + pattern);
            }
            std::vector<std::size_t> expected_ends;
            for (const auto& outcome : crank::match_states(ast, subject, start, false, unpruned()))
                expected_ends.push_back(outcome.end);
            std::sort(expected_ends.begin(), expected_ends.end());
            expected_ends.erase(std::unique(expected_ends.begin(), expected_ends.end()), expected_ends.end());
            require(crank::match_endpoints(ast, subject, start) == expected_ends,
                    "projected endpoint set differs: " + pattern);
            require(program.endpoints(subject, start) == expected_ends,
                    "Program endpoint dispatch differs: " + pattern);
            crank::RunOptions exact;
            exact.optimize = false;
            require(program.endpoints(subject, start, exact) == expected_ends,
                    "unoptimized Program endpoint set differs: " + pattern);
        }
        expect_exception<std::out_of_range>([&] {
            (void)crank::match_exists(ast, subject, subject.size() + 1, false);
        }, "match_exists must reject an out-of-range start");
        expect_exception<std::out_of_range>([&] {
            (void)crank::match_endpoints(ast, subject, subject.size() + 1);
        }, "match_endpoints must reject an out-of-range start");
    }
    const auto ast = crank::parse("(a){2}");
    auto options = unpruned();
    options.max_states = 0;
    expect_exception<crank::ResourceLimit>([&] {
        (void)crank::match_exists(ast, "aa", 0, true, options);
    }, "zero state budget must throw rather than report no match");
    options = unpruned(); options.max_steps = 0;
    expect_exception<crank::ResourceLimit>([&] {
        (void)crank::match_exists(ast, "aa", 0, true, options);
    }, "zero work budget must throw rather than report no match");
    options = unpruned(); options.max_repeat = 0;
    expect_exception<crank::ResourceLimit>([&] {
        (void)crank::match_exists(ast, "aa", 0, true, options);
    }, "zero repeat budget must throw rather than report no match");

    // Observational projection must save actual retained states, while complete
    // capture reporting must preserve all results or raise a resource exception.
    const auto ambiguous = crank::parse("((a?))*");
    crank::MatchStatistics projected, complete;
    require(crank::match_exists(ambiguous, "aaaaaaaa", 0, true, {}, &projected),
            "projected nullable example must accept");
    require(!crank::match_states(ambiguous, "aaaaaaaa", 0, true, {}, &complete).empty(),
            "complete nullable example must accept");
    require(projected.states < complete.states,
            "discarding unread captures should reduce retained state storage");
    options = {}; options.max_states = projected.states;
    require(crank::match_exists(ambiguous, "aaaaaaaa", 0, true, options),
            "projected query must fit its measured retained-state budget");
    expect_exception<crank::ResourceLimit>([&] {
        (void)crank::match_states(ambiguous, "aaaaaaaa", 0, true, options);
    }, "complete outcomes must raise a limit instead of silently projecting captures");
}

void large_specialized_inputs() {
    crank::RunOptions tiny;
    tiny.match.max_states = tiny.match.max_steps = tiny.match.max_repeat = 1;
    crank::RunStatistics statistics;
    const std::size_t n = 30000;
    const crank::Program count("a{#n}b{=n}c{=n}");
    const auto good = std::string(n, 'a') + std::string(n, 'b') + std::string(n, 'c');
    require(count.full_match(good, tiny, &statistics), "large count plan must accept");
    require(statistics.backend == "count-plan", "large count input must dispatch to count plan");
    auto bad = good;
    std::swap(bad[n - 1], bad[n]);
    require(!count.full_match(bad, tiny, &statistics), "count arithmetic alone cannot prove membership");
    require(statistics.backend == "count-plan", "invalid count body must still use exact count plan");

    std::string half;
    half.reserve(2 * n);
    for (std::size_t i = 0; i < n; ++i) half += i % 2 ? "ab" : "ba";
    const crank::Program equality("\\k<x>#(?<x>(ab|ba)*)#\\k<x>");
    auto copies = half + '#' + half + '#' + half;
    require(equality.full_match(copies, tiny, &statistics), "large equality plan must accept");
    require(statistics.backend == "equality-plan", "large equality input must dispatch to equality plan");
    copies.back() = copies.back() == 'a' ? 'b' : 'a';
    require(!equality.full_match(copies, tiny, &statistics), "equality plan must compare every occurrence");
    require(statistics.backend == "equality-plan", "inequality must use exact equality plan");

    tiny.backend = crank::Backend::States;
    expect_exception<crank::ResourceLimit>([&] {
        (void)count.full_match(good, tiny);
    }, "forced branching backend must retain its explicit resource contract");
}
} // namespace

int main() {
    try {
        plan_eligibility();
        short_properties();
        independent_constructions();
        offsets_and_limits();
        large_specialized_inputs();
        std::cout << "PASS: " << checks << " specialized API checks; "
                  << differential_cases << " complete-state differential cases; "
                  << construction_cases << " independent construction cases\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
