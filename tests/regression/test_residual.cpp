#include "affine_matcher.hpp"
#include "crank.hpp"
#include "fragment_matcher.hpp"

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
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

std::string bytes(std::string_view text) {
    std::ostringstream result;
    result << '"';
    for (unsigned char byte : text) {
        if (byte >= 32 && byte < 127 && byte != '\\' && byte != '"') result << byte;
        else result << "\\x" << std::hex << std::setw(2) << std::setfill('0') << unsigned(byte);
    }
    return result.str() + '"';
}

std::vector<std::string> words(std::string_view alphabet, unsigned maximum) {
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

void differential(const crank::Program& program, const crank::AffinePlan& plan,
                  std::string_view pattern, std::string_view subject) {
    // Keep every observable capture/count environment and disable all arithmetic
    // pruning: this is independent of both residual plans and Boolean projection.
    const bool expected = !crank::match_states(program.ast(), subject, 0, true, unpruned()).empty();
    const auto context = bytes(pattern) + " on " + bytes(subject);
    require(plan.match(subject) == expected, "residual differs: " + context);
    require(program.full_match(subject) == expected, "Auto differs: " + context);
    ++differential_cases;
}

crank::AffinePlan residual(std::string_view pattern) {
    const auto plan = crank::make_affine_plan(crank::parse(pattern));
    require(plan.has_value(), "missing residual plan: " + bytes(pattern));
    require(plan->residual(), "expected exactly one free variable: " + bytes(pattern));
    return *plan;
}

void short_properties() {
    struct Group {
        std::vector<std::string> patterns;
        std::string alphabet;
        unsigned maximum;
    };
    const std::vector<Group> groups{
        {{"(?<x>[ab]*)(?<y>[ab]*)\\k<y>\\k<x>",
          "(?<x>[ab]*)(?<y>[ab]*)\\k<x>\\k<y>",
          "\\k<x>\\k<y>(?<y>[ab]*)(?<x>[ab]*)",
          "\\k<x>(?<y>[ab]*)(?<x>[ab]*)\\k<y>",
          "(?<x>[ab]+)(?<y>[ab]+)\\k<y>\\k<x>",
          "(?<x>[ab]{1,3})(?<y>[ab]{2,4})\\k<y>\\k<x>",
          "(?<x>[ab]{0,2})[ab]{1,3}\\k<x>",
          "(?<x>[ab]*)(?:[ab]{2}){#n}[ab]{=n}\\k<x>",
          "(?:[ab]{2}){#n}(?:[ab]{3}){#m}",
          "(?:[ab]{2}){#n}(?:[ab]{3}){#m}(?:[ab]{2}){=n}(?:[ab]{3}){=m}",
          "(?:ab){#n}(?:ba){#m}(?:ba){=n}(?:ab){=m}",
          "(?:ab){#n}(?:aab){#m}(?:abb){#k}",
          "(?:aa){#n}(?:bbb){#m}(?:ab){#k}",
          "(?:aaaa){#n}(?:bbbbbb){#m}(?:ab){#k}"}, "ab", 7},
        {{"(?<x>[ab]*)(?<y>[ab]*)(?<z>c*)\\k<z>\\k<y>\\k<x>",
          "(?<x>[ab]*)(?<y>[ab]*)c{#n}c{=n}\\k<y>\\k<x>",
          "(?<x>[ab]*)(?<y>[bc]*)\\k<y>\\k<x>",
          "(?<x>[ab]*)(?<y>[bc]*)\\k<x>\\k<y>"}, "abc", 5},
        {{"(?<x>[ab:]*):(?<y>[ab:]*)\\k<x>\\k<y>",
          "(?:a:|ab)(?<x>[ab]*):(?<y>[ab]*)\\k<y>\\k<x>"}, "ab:", 5},
        {{"(?<x>[\\x00\\x80\\xff]*)(?<y>[\\x00\\x80\\xff]*)\\k<y>\\k<x>",
          "\\k<x>\\k<y>(?<y>[\\x00\\x80\\xff]*)(?<x>[\\x00\\x80\\xff]*)",
          "(?:[\\x00\\x80\\xff]{2}){#n}(?:[\\x00\\x80\\xff]{3}){#m}"},
          std::string{"\0\x80\xff", 3}, 5},
    };
    for (const auto& group : groups) {
        const auto subjects = words(group.alphabet, group.maximum);
        for (const auto& pattern : group.patterns) {
            const crank::Program program(pattern);
            const auto plan = residual(pattern);
            for (const auto& subject : subjects) differential(program, plan, pattern, subject);
        }
    }
}

void constructed(std::string_view pattern, std::string_view subject, bool expected) {
    const crank::Program program{std::string(pattern)};
    const auto plan = residual(pattern);
    const auto context = bytes(pattern) + " on " + bytes(subject);
    require(plan.match(subject) == expected, "constructed residual answer: " + context);
    require(program.full_match(subject) == expected, "constructed Auto answer: " + context);
    require(!crank::match_states(program.ast(), subject, 0, true, unpruned()).empty() == expected,
            "constructed complete answer: " + context);
    ++construction_cases;
}

void independent_constructions() {
    const std::string pattern = "(?<x>[ab]*)(?<y>[ab]*)\\k<y>\\k<x>";
    for (const auto& x : words("ab", 3)) {
        for (const auto& y : words("ab", 3)) {
            constructed(pattern, x + y + y + x, true);
            constructed("\\k<x>\\k<y>(?<y>[ab]*)(?<x>[ab]*)", x + y + y + x, true);
        }
    }
    // Independent fixed-width coin-membership oracle, including unattainable
    // lengths and offsets that would make naive unsigned subtraction wrap.
    for (std::size_t size = 0; size <= 31; ++size) {
        bool reachable = false;
        for (std::size_t n = 0; 2 * n <= size; ++n)
            for (std::size_t m = 0; 2 * n + 3 * m <= size; ++m)
                reachable = reachable || 2 * n + 3 * m == size;
        std::string subject;
        for (std::size_t i = 0; i < size; ++i) subject += i % 3 ? 'a' : 'b';
        constructed("(?:[ab]{2}){#n}(?:[ab]{3}){#m}", subject, reachable);
        constructed("!(?:[ab]{2}){#n}(?:[ab]{3}){#m}", '!' + subject, reachable);
    }
    constructed("!(?:[ab]{2}){#n}(?:[ab]{3}){#m}", "", false);
    constructed(pattern, "abba", true); // Every zero-length capture choice fails.
    constructed(pattern, "aaab", false); // Every arithmetic assignment fails equality.
    constructed(pattern, "aabb", false);
    constructed("(?<x>[ab]{1,2})(?<y>[ab]{2,3})\\k<y>\\k<x>", "abba", false);
    constructed("(?<x>[ab]{1,2})(?<y>[ab]{2,3})\\k<y>\\k<x>", "abbbba", true);
    constructed("(?<x>[ab]{1,2})(?<y>[ab]{2,3})\\k<y>\\k<x>", "abbbbbbbbbba", false);
    constructed("(?<x>[ab]*)(?<y>[bc]*)\\k<y>\\k<x>", "abccab", true);
    constructed("(?<x>[ab]*)(?<y>[bc]*)\\k<y>\\k<x>", "acbca", false);

    // A body with the right length and histogram can still violate its internal
    // alternation. Both repetitions have identical alphabets and byte counts.
    constructed("(?:ab|ba){#n}(?:ab|ba){#m}(?:ba|ab){=n}(?:ba|ab){=m}", "abba", true);
    constructed("(?:ab|ba){#n}(?:ab|ba){#m}(?:ba|ab){=n}(?:ba|ab){=m}", "aabb", false);
}

void eligibility_boundaries() {
    for (const std::string pattern : {
             "(?<x>[ab]*)(?<y>[ab]*)(?<z>[ab]*)\\k<z>\\k<y>\\k<x>",
             "[ab]{#n}[ab]{#m}[ab]{#k}[ab]{=n}[ab]{=m}[ab]{=k}",
             "(?:(?<x>[ab]*)\\k<x>)*", "(?<x>[ab]*)?\\k<x>",
             "(?<x>(?<y>[ab]*))\\k<y>\\k<x>",
             "(?:a|ab){#n}[ab]{#m}[ab]{=n}[ab]{=m}"}) {
        require(!crank::make_affine_plan(crank::parse(pattern)),
                "unsupported residual shape should decline: " + bytes(pattern));
        const crank::Program program(pattern);
        for (const auto& subject : words("ab", 4)) {
            const bool expected = !crank::match_states(program.ast(), subject, 0, true, unpruned()).empty();
            require(program.full_match(subject) == expected,
                    "declined plan must retain complete Auto behavior: " + bytes(pattern));
            ++differential_cases;
        }
    }
    crank::RegularOptions tiny;
    tiny.max_nfa_states = tiny.max_dfa_states = 1;
    require(!crank::make_affine_plan(crank::parse("(?<x>[ab]*)(?<y>[ab]*)\\k<y>\\k<x>"), tiny),
            "residual fragments must retain compilation resource limits");
}

void congruence_properties() {
    crank::AffineStatistics stats;
    const std::string fractional_pattern = "(?:abbbbb){#n}(?:aaaabbbbb){#m}(?:aaabbbbbbb){#k}";
    const crank::Program fractional_program(fractional_pattern);
    const auto fractional = residual(fractional_pattern);
    for (unsigned n = 0; n <= 3; ++n) {
        for (unsigned m = 0; m <= 3; ++m) {
            for (unsigned k = 0; k <= 3; ++k) {
                std::string value;
                for (unsigned i = 0; i < n; ++i) value += "abbbbb";
                for (unsigned i = 0; i < m; ++i) value += "aaaabbbbb";
                for (unsigned i = 0; i < k; ++i) value += "aaabbbbbbb";
                constructed(fractional_pattern, value, true);
                require(fractional.match(value, &stats) && stats.candidate_stride == 15,
                        "fractional multi-observation formulas must retain stride fifteen");
                if (!value.empty()) {
                    value[value.size() / 2] = value[value.size() / 2] == 'a' ? 'b' : 'a';
                    differential(fractional_program, fractional, fractional_pattern, value);
                }
            }
        }
    }
    const auto parity = residual("(?:[ab]b){#n}(?:a[ab]{2}){#m}");
    require(parity.match("aaaaaa", &stats), "later congruence-compatible assignment must accept");
    require(stats.candidate_stride == 2 && stats.candidate_assignments == 2,
            "parity constraints must skip inadmissible integer assignments");
    require(stats.candidate_lower == 0 && stats.candidate_upper == 2,
            "candidate interval must report its first and last admissible values");

    // Observed #a=2n+k and #b=3m+k impose two congruences on k. Their
    // intersection advances by six, and only the later assignment is valid.
    const auto crt = residual("(?:aa){#n}(?:bbb){#m}(?:ab){#k}");
    std::string subject = "aabbb";
    for (unsigned i = 0; i < 6; ++i) subject += "ab";
    require(crt.match(subject, &stats), "joint congruence construction must accept");
    require(stats.candidate_stride == 6 && stats.candidate_assignments == 2,
            "independent mod-two and mod-three constraints must combine exactly");
    constructed("(?:aa){#n}(?:bbb){#m}(?:ab){#k}", subject, true);
    crank::AffineMatchOptions budget;
    budget.max_candidates = 1;
    expect_exception<crank::ResourceLimit>([&] { (void)crt.match(subject, &stats, budget); },
            "CRT skipping must not discard the later valid assignment under a budget");

    const auto shared_factor = residual("(?:aaaa){#n}(?:bbbbbb){#m}(?:ab){#k}");
    subject = "aaaabbbbbb";
    for (unsigned i = 0; i < 12; ++i) subject += "ab";
    require(shared_factor.match(subject, &stats), "non-coprime congruence construction must accept");
    require(stats.candidate_stride == 12 && stats.candidate_assignments == 2,
            "mod-four and mod-six constraints require lcm twelve, not product twenty-four");
    constructed("(?:aaaa){#n}(?:bbbbbb){#m}(?:ab){#k}", subject, true);
    budget.max_candidates = budget.max_validation_bytes = 0;
    require(!shared_factor.match("abb", &stats, budget) && stats.arithmetic_rejected,
            "inconsistent non-coprime congruences must reject without validating candidates");
    require(stats.candidate_assignments == 0 && stats.search_complete && !stats.resource_limited,
            "inconsistent congruences must complete their arithmetic rejection");
}

void budgets_and_statistics() {
    const std::string pattern = "(?<x>[ab]*)(?<y>[ab]*)\\k<y>\\k<x>";
    const auto plan = residual(pattern);
    const crank::Program program(pattern);
    crank::AffineStatistics stats;
    crank::AffineMatchOptions budget;
    budget.max_candidates = 1;
    expect_exception<crank::ResourceLimit>([&] { (void)plan.match("abba", &stats, budget); },
        "a failing first assignment must not become a false rejection");
    require(stats.resource_limited && !stats.search_complete,
            "candidate exhaustion must publish an incomplete resource-limited search");
    require(stats.candidate_assignments == 1 && stats.free_variables == 1,
            "candidate budget must count validated assignments exactly");
    budget.max_candidates = 2;
    require(plan.match("abba", &stats, budget), "second assignment must remain reachable");
    require(!stats.resource_limited && stats.candidate_assignments == 2,
            "successful reuse must reset resource-limit statistics");
    budget.max_candidates = 2;
    expect_exception<crank::ResourceLimit>([&] { (void)plan.match("aaab", &stats, budget); },
        "a not-yet-tested final assignment prevents definitive rejection");
    budget.max_candidates = 3;
    require(!plan.match("aaab", &stats, budget), "exactly exhausting all candidates permits false");
    require(stats.search_complete && !stats.resource_limited && stats.candidate_assignments == 3,
            "final failed assignment must complete the search without a spurious limit");

    budget.max_candidates = 0;
    expect_exception<crank::ResourceLimit>([&] { (void)plan.match("abba", &stats, budget); },
        "zero candidates cannot silently disable residual matching");
    require(!plan.match("a", &stats, budget) && stats.arithmetic_rejected && stats.search_complete,
            "impossible arithmetic may reject without spending a candidate budget");
    budget.max_candidates = 100;
    budget.max_validation_bytes = 0;
    expect_exception<crank::ResourceLimit>([&] { (void)plan.match("abba", &stats, budget); },
        "validation-byte exhaustion must throw rather than guess");
    require(stats.resource_limited && !stats.search_complete,
            "validation exhaustion must preserve its explicit status");
    require(plan.match("", &stats, budget), "empty spans require no validation bytes");

    crank::RunOptions options;
    options.affine.max_candidates = 1;
    crank::RunStatistics run_stats;
    expect_exception<crank::ResourceLimit>([&] { (void)program.full_match("abba", options, &run_stats); },
        "Auto must forward the residual candidate budget");
    require(run_stats.backend == "affine-search" && run_stats.affine.resource_limited,
            "Auto must expose residual backend and exhaustion statistics");
    options.affine.max_candidates = 2;
    options.match.max_steps = options.match.max_states = options.match.max_repeat = 0;
    require(program.full_match("abba", options, &run_stats) && run_stats.backend == "affine-search",
            "residual plans use their own budget and do not invoke the complete evaluator");
    options.affine.max_validation_bytes = 0;
    expect_exception<crank::ResourceLimit>([&] { (void)program.full_match("abba", options); },
        "Auto must forward the residual validation-byte budget");

    // Full-rank arithmetic still has exactly one determined assignment; none of
    // the new search budgets should degrade the previous constant-candidate path.
    const std::string unique_pattern = "(a*)(b*)\\2\\1";
    const auto unique = crank::make_affine_plan(crank::parse(unique_pattern));
    require(unique.has_value() && !unique->residual(), "full-rank plan must remain distinguished");
    budget.max_candidates = budget.max_validation_bytes = 0;
    require(unique->match("aabbbbaa", &stats, budget), "full-rank match must ignore residual budgets");
    require(!unique->match("aabbbaba", &stats, budget), "full-rank rejection must ignore residual budgets");
    const crank::Program unique_program(unique_pattern);
    options.affine = budget;
    require(unique_program.full_match("aabbbbaa", options, &run_stats) && run_stats.backend == "affine-plan",
            "full-rank Auto must retain its backend under zero search/evaluator budgets");
    require(!unique_program.full_match("aabbbaba", options), "invalid full-rank Auto must remain exact");

    // Returning captures or endpoints still enumerates all relevant outcomes,
    // independently of the Boolean residual search and its candidate budget.
    options = {};
    options.affine.max_candidates = options.affine.max_validation_bytes = 0;
    const auto expected_outcomes = crank::match_states(program.ast(), "aaaa", 0, true, unpruned());
    require(program.outcomes("aaaa", options) == expected_outcomes,
            "residual search must not truncate observable capture outcomes");
    std::vector<std::size_t> expected_ends;
    for (const auto& outcome : crank::match_states(program.ast(), "aaaab", 0, false, unpruned()))
        expected_ends.push_back(outcome.end);
    std::sort(expected_ends.begin(), expected_ends.end());
    expected_ends.erase(std::unique(expected_ends.begin(), expected_ends.end()), expected_ends.end());
    require(program.endpoints("aaaab", 0, options) == expected_ends,
            "residual search must not truncate prefix endpoints");
}

void periodic_literal_scans() {
    // Literal periods span arbitrary widths and byte values, including NUL.
    // Compare against independently sliced copies, including multiple failures.
    for (const std::size_t width : {1U, 2U, 3U, 7U, 17U, 31U, 33U, 65U, 127U}) {
        std::string literal, pattern;
        constexpr char hex[] = "0123456789abcdef";
        for (std::size_t i = 0; i < width; ++i) {
            const auto byte = static_cast<unsigned char>((i * 73 + width * 19) % 256);
            literal += static_cast<char>(byte);
            pattern += "\\x";
            pattern += hex[byte >> 4]; pattern += hex[byte & 15];
        }
        const auto ast = crank::parse(pattern);
        const crank::RegularFragment fragment(ast, ast.root);
        for (const std::size_t copies : {0U, 1U, 2U, 31U, 32U, 33U, 73U}) {
            std::string subject;
            for (std::size_t i = 0; i < copies; ++i) subject += literal;
            require(fragment.matching_copies(subject, width, copies) == copies,
                    "valid arbitrary-width literal period must accept");
            if (!copies) continue;
            const auto verify = [&] {
                std::size_t expected = 0;
                while (expected < copies && std::string_view(subject).substr(expected * width, width) == literal)
                    ++expected;
                require(fragment.matching_copies(subject, width, copies) == expected,
                        "periodic scan must report the first independently invalid copy");
            };
            for (const std::size_t copy : {std::size_t{0}, copies / 2, copies - 1}) {
                for (const std::size_t phase : {std::size_t{0}, width / 2, width - 1}) {
                    const auto position = copy * width + phase;
                    subject[position] ^= 1;
                    verify();
                    subject[position] ^= 1;
                }
            }
            // A uniformly altered word is still periodic, but not this literal.
            for (std::size_t copy = 0; copy < copies; ++copy) subject[copy * width] ^= 1;
            verify();
            subject = literal;
            for (std::size_t copy = 1; copy < copies; ++copy) subject += literal;
            if (copies >= 3) {
                subject[width + width - 1] ^= 1;
                subject[(copies - 1) * width] ^= 1;
                verify();
            }
        }
    }
}
} // namespace

int main() {
    try {
        periodic_literal_scans();
#if defined(__SIZEOF_INT128__)
        short_properties();
        independent_constructions();
        eligibility_boundaries();
        congruence_properties();
        budgets_and_statistics();
#else
        require(!crank::make_affine_plan(crank::parse("(?<x>[ab]*)(?<y>[ab]*)\\k<y>\\k<x>")),
                "unsupported arithmetic platform must decline residual certification");
        std::cout << "SKIP: residual arithmetic requires compiler 128-bit integers\n";
#endif
        std::cout << "PASS: " << checks << " residual/periodic checks; "
                  << differential_cases << " complete-state differential cases; "
                  << construction_cases << " independent construction cases\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
