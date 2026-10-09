#include "integer_row.hpp"
#include "ast_machine.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <future>
#include <iostream>
#include <limits>
#include <numeric>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
std::size_t checks = 0, integer_cases = 0, budget_cases = 0, any_cases = 0;
std::size_t grammars = 0, membership_cases = 0;

void require(bool value, const std::string& context) {
    ++checks;
    if (!value) throw std::runtime_error(context);
}
template<class Exception, class Function>
void expect_exception(Function&& function, const std::string& context) {
    bool caught = false;
    try { function(); } catch (const Exception&) { caught = true; }
    require(caught, context);
}

using Status = crank::detail::IntegerRowStatus;
using Row = crank::detail::IntegerRowResult;
using Bound = std::optional<std::size_t>;
using Pair = std::pair<std::size_t, std::size_t>;

// Enumerate bounded actual counts. This oracle does not use gcd, inverses,
// residue alignment or lower-bound shifts. Checked products let the same
// enumeration cover near-SIZE_MAX coefficients without extended integers.
std::vector<Pair> enumerate(std::size_t a, std::size_t b, std::size_t total,
    std::size_t lower_x, Bound upper_x, std::size_t lower_y, Bound upper_y) {
    std::vector<Pair> result;
    if (!a || !b || (upper_x && *upper_x < lower_x) || (upper_y && *upper_y < lower_y)) return result;
    const auto last_x = std::min(upper_x.value_or(total / a), total / a);
    const auto last_y = std::min(upper_y.value_or(total / b), total / b);
    if (lower_x > last_x || lower_y > last_y) return result;
    for (auto x = lower_x;; ++x) {
        const auto remaining = total - a * x;
        for (auto y = lower_y;; ++y) {
            if (y <= remaining / b && a * x + b * y == total) result.emplace_back(x, y);
            if (y == last_y) break;
        }
        if (x == last_x) break;
    }
    return result;
}

void check_row(std::size_t a, std::size_t b, std::size_t total,
    std::size_t lower_x, Bound upper_x, std::size_t lower_y, Bound upper_y) {
    ++integer_cases;
    std::size_t steps = 0;
    const auto row = crank::detail::solve_integer_row(a, b, total,
        lower_x, upper_x, lower_y, upper_y, [&] { ++steps; });
    require(steps > 0 && steps < 10000, "integer helper uses finite charged work");
    if (!a || !b) {
        require(row.status == Status::Unsupported, "zero coefficient is unsupported, including invalid bounds");
        return;
    }
    const auto actual = enumerate(a, b, total, lower_x, upper_x, lower_y, upper_y);
    require(row.status == (actual.empty() ? Status::None : actual.size() == 1 ? Status::Unique : Status::Multiple),
            "integer status agrees with independent count enumeration");
    if (actual.empty()) return;
    require(row.first_x == actual.front().first && row.corresponding_y == actual.front().second &&
            row.last_x == actual.back().first, "integer solution endpoints preserve actual unshifted counts");
    require(row.x_step == b / std::gcd(a, b), "integer lattice stride");
    for (std::size_t i = 1; i < actual.size(); ++i)
        require(actual[i].first - actual[i - 1].first == row.x_step,
                "integer lattice contains exactly all enumerated solutions");
}

void integer_enumeration() {
    const std::vector<std::pair<std::size_t, Bound>> bounds{
        {0, {}}, {0, 0}, {0, 1}, {1, {}}, {1, 1}, {2, 4}, {3, 2}, {4, 6}};
    for (std::size_t a = 0; a <= 8; ++a)
        for (std::size_t b = 0; b <= 8; ++b)
            for (std::size_t total = 0; total <= 40; ++total)
                for (const auto& [lx, ux] : bounds)
                    for (const auto& [ly, uy] : bounds)
                        check_row(a, b, total, lx, ux, ly, uy);

    // Large coefficients with small explicit count boxes remain exhaustively
    // enumerable. Alternate arbitrary totals and totals built from witnesses.
    constexpr auto maximum = std::numeric_limits<std::size_t>::max();
    std::uint64_t random = 0x3712fc0d9aeb6485ULL;
    auto next = [&] { random = random * 6364136223846793005ULL + 1442695040888963407ULL;
                     return static_cast<std::size_t>(random); };
    for (unsigned i = 0; i < 4096; ++i) {
        auto a = next() | 1U, b = next() | 1U;
        auto total = next();
        if (i & 1U) {
            a /= 16; b /= 16; ++a; ++b;
            const auto x = next() % 5, y = next() % 5;
            require(x <= maximum / a && y <= (maximum - a * x) / b,
                    "large-row witness construction is overflow-free");
            total = a * x + b * y;
        }
        check_row(a, b, total, i % 3, 4, (i / 3) % 3, 4);
    }
}

void integer_edges_and_budgets() {
    constexpr auto m = std::numeric_limits<std::size_t>::max();
    const auto inspect = [](std::size_t a, std::size_t b, std::size_t total,
        std::size_t lx, Bound ux, std::size_t ly, Bound uy,
        Status status, std::size_t first = 0, std::size_t y = 0,
        std::size_t step = 0, std::size_t last = 0) {
        ++integer_cases;
        const auto row = crank::detail::solve_integer_row(a, b, total, lx, ux, ly, uy, [] {});
        require(row.status == status, "SIZE_MAX/large-row exact status");
        if (status == Status::Unique || status == Status::Multiple)
            require(row.first_x == first && row.corresponding_y == y && row.x_step == step && row.last_x == last,
                    "SIZE_MAX/large-row exact lattice endpoints");
    };
    inspect(1, 1, m, 0, {}, 0, {}, Status::Multiple, 0, m, 1, m); // SIZE_MAX+1 solutions
    inspect(m, m, m, 0, {}, 0, {}, Status::Multiple, 0, 1, 1, 1);
    inspect(m, m - 1, m, 0, {}, 0, {}, Status::Unique, 1, 0, m - 1, 1);
    inspect(m - 1, m, m, 0, {}, 0, {}, Status::Unique, 0, 1, m, 0);
    inspect(m, 1, m, 0, {}, 0, {}, Status::Multiple, 0, m, 1, 1);
    inspect(1, m, m, 0, {}, 0, {}, Status::Multiple, 0, 1, m, m);
    inspect(m, m, m - 1, 0, {}, 0, {}, Status::None);
    inspect(2, 2, m, 0, {}, 0, {}, Status::None);
    inspect(1, 1, m, m - 7, {}, 7, {}, Status::Unique, m - 7, 7, 1, m - 7);
    inspect(1, 1, m, m, m, 0, 0, Status::Unique, m, 0, 1, m);
    inspect(1, 1, m, m, {}, 1, {}, Status::None);
    inspect(m / 2 + 1, 1, m, 2, {}, 0, {}, Status::None);
    inspect(m, m, 0, 0, {}, 0, {}, Status::Unique, 0, 0, 1, 0);
    inspect(1, 1, m, m, m - 1, 0, {}, Status::None);
    inspect(0, m, m, m, 0, 0, {}, Status::Unsupported);
    inspect(127, 131, 16379, 0, {}, 0, {}, Status::None);
    inspect(127, 131, 16380, 0, {}, 0, {}, Status::Unique, 97, 31, 131, 97);
    inspect(127, 131, 16637, 0, {}, 0, {}, Status::Multiple, 0, 127, 131, 131);

    struct BudgetStopped {};
    std::size_t charged = 0;
    const auto complete = crank::detail::solve_integer_row(127, 131, 16380, 0, {}, 0, {}, [&] { ++charged; });
    require(complete.status == Status::Unique && charged > 3, "budget fixture actually enters arithmetic loops");
    for (std::size_t budget = 0; budget < charged; ++budget) {
        ++budget_cases;
        std::size_t steps = 0;
        expect_exception<BudgetStopped>([&] {
            (void)crank::detail::solve_integer_row(127, 131, 16380, 0, {}, 0, {}, [&] {
                if (steps == budget) throw BudgetStopped{};
                ++steps;
            });
        }, "partial integer computation propagates budget exhaustion instead of None");
        require(steps == budget, "integer callback respects every cut point");
    }
    std::size_t steps = 0;
    const auto finished = crank::detail::solve_integer_row(127, 131, 16380, 0, {}, 0, {}, [&] {
        if (steps == charged) throw BudgetStopped{};
        ++steps;
    });
    require(finished.status == complete.status && steps == charged, "exact charged budget suffices");
}

std::vector<std::string> words(std::string_view alphabet, unsigned maximum) {
    std::vector<std::string> result{std::string{}};
    std::size_t begin = 0, end = 1;
    for (unsigned length = 0; length < maximum; ++length) {
        for (auto i = begin; i < end; ++i)
            for (char c : alphabet) result.push_back(result[i] + c);
        begin = end; end = result.size();
    }
    return result;
}

void native_multi_entry() {
    const std::vector<std::pair<std::string, std::string>> fixtures{
        {"a(?R)b|x", "abx"},
        {"(?<S>a(?&S)b|)|ab|ba", "ab"},
        {"(?<S>(?&S)(?&S)|a|)|b", "ab"},
        {"(?(DEFINE)(?<A>a|ab))(?:(?&A)b|(?&A)bb|(?&A))", "ab"},
        {"(?(DEFINE)(?<A>(?&B)|a)(?<B>(?&A)|))(?&A)|(?&B)", "ab"},
        {"(?<D>(?&D))|a|", "ab"},
        {"(?:aa|!!)|(?:a!|!a)", "a!"},
        {"(?:\\x00\\xff|\\x80\\x00)|(?:\\x00|)", std::string{char(0), char(128), char(255)}}
    };
    for (const auto& [pattern, alphabet] : fixtures) {
        const crank::detail::AstMachineProgram native(crank::parse(pattern));
        const auto& ast = native.ast();
        std::vector<std::vector<crank::NodeId>> sets{{}, {ast.root}, {ast.root, ast.root}};
        for (const auto& node : ast.nodes)
            if (node.kind == crank::Kind::Alt) {
                sets.push_back(node.children);
                auto reverse = node.children;
                std::reverse(reverse.begin(), reverse.end());
                sets.push_back(reverse);
                if (!reverse.empty()) reverse.push_back(reverse.front());
                sets.push_back(std::move(reverse));
            }
        for (const auto& subject : words(alphabet, 3)) {
            for (const auto& entries : sets) {
                for (unsigned flags = 0; flags < 4; ++flags) {
                    crank::OccurrenceMatchOptions options;
                    options.prune = (flags & 1U) != 0;
                    options.collapse_control = (flags & 2U) != 0;
                    options.compress_returns = (flags & 2U) != 0;
                    bool expected = false;
                    for (const auto entry : entries)
                        expected = native.full_match_node(entry, subject, options, nullptr) || expected;
                    crank::OccurrenceStatistics stats;
                    stats.states = 123;
                    ++any_cases;
                    require(native.full_match_any(entries, subject, options, &stats) == expected,
                            "multi-entry query equals individual original-node OR, including duplicates");
                    require(!stats.limit_reached && stats.states <= options.max_states && stats.work_steps <= options.max_steps,
                            "multi-entry query respects one aggregate native budget");
                    if (entries.empty()) require(stats.states == 0 && stats.work_steps == 0,
                                                "empty entry set is false with zero statistics");
                }
            }
        }
        const std::vector<crank::NodeId> invalid{ast.root, ast.nodes.size()};
        expect_exception<std::out_of_range>([&] { (void)native.full_match_any(invalid, "", {}, nullptr); },
                                            "all entry IDs are validated before an early success");
    }

    const crank::detail::AstMachineProgram recursive(crank::parse("a(?R)b|x"));
    const auto root = recursive.control_targets()[recursive.ast().root];
    require(recursive.ast().nodes[root].kind == crank::Kind::Alt, "root recursion fixture has direct alternatives");
    const auto entries = recursive.ast().nodes[root].children;
    require(recursive.full_match_any(entries, "aaxbb", {}, nullptr),
            "multi-entry execution retains the original whole-pattern recursion target");
    std::atomic_bool cancelled{true};
    expect_exception<crank::detail::AstExecutionCancelled>([&] {
        (void)recursive.full_match_any(entries, "aaxbb", {}, nullptr, &cancelled);
    }, "multi-entry cancellation stays distinct from a negative verdict");
    require(!recursive.full_match_any({}, "aaxbb", {}, nullptr, &cancelled),
            "empty entry set has no unfinished obligation to cancel");
    cancelled.store(false);
    require(recursive.full_match_any(entries, "axb", {}, nullptr, &cancelled),
            "cancellation does not poison a later multi-entry query");
    for (bool work : {false, true}) {
        crank::OccurrenceMatchOptions options;
        if (work) options.max_steps = 0;
        else options.max_states = 0;
        crank::OccurrenceStatistics stats;
        expect_exception<crank::ResourceLimit>([&] { (void)recursive.full_match_any(entries, "aaxbb", options, &stats); },
                                              "unfinished multi-entry OR reports ResourceLimit");
        require(stats.limit_reached && stats.states <= options.max_states && stats.work_steps <= options.max_steps,
                "multi-entry exhaustion respects aggregate quotas");
    }

    // Shared leading calls must actually reuse native requests, not merely run
    // a serial loop over independent task entries with a different API name.
    const crank::detail::AstMachineProgram shared(crank::parse(
        "(?<R>a(?&R)b|)ccAdd|(?&R)ccBdd|(?&R)ccCdd|(?&R)ccDdd"));
    const auto shared_root = shared.control_targets()[shared.ast().root];
    const auto alternatives = shared.ast().nodes[shared_root].children;
    require(shared.ast().nodes[shared_root].kind == crank::Kind::Alt && alternatives.size() == 4,
            "shared-call control has four native entries");
    const std::string subject = std::string(24, 'a') + std::string(24, 'b') + "ccZdd";
    std::size_t independent_states = 0;
    for (const auto entry : alternatives) {
        crank::OccurrenceStatistics stats;
        require(!shared.full_match_node(entry, subject, {}, &stats), "independent shared-prefix rejection");
        independent_states += stats.states;
    }
    crank::OccurrenceStatistics together;
    require(!shared.full_match_any(alternatives, subject, {}, &together), "combined shared-prefix rejection");
    require(together.states < independent_states && together.memo_hits > 0,
            "multi-entry execution genuinely retains common native request reuse");
}

struct Symbol { bool terminal; std::size_t id; std::string text; };
Symbol n(std::size_t id) { return {false, id, {}}; }
Symbol t(std::string text) { return {true, 0, std::move(text)}; }
struct Production { std::size_t lhs; std::vector<Symbol> rhs; };
using Language = std::set<std::string>;

// Independent grammar-language fixed point, without arithmetic or AST/input
// work lists. Positive string concatenation makes length truncation sound.
std::vector<Language> bounded_languages(std::size_t count,
    const std::vector<Production>& productions, std::size_t maximum) {
    std::vector<Language> result(count);
    bool changed;
    do {
        changed = false;
        for (const auto& production : productions) {
            Language partial{std::string{}};
            for (const auto& symbol : production.rhs) {
                const Language terminal{symbol.text};
                const auto& suffixes = symbol.terminal ? terminal : result.at(symbol.id);
                Language next;
                for (const auto& prefix : partial)
                    for (const auto& suffix : suffixes)
                        if (prefix.size() <= maximum && suffix.size() <= maximum - prefix.size())
                            next.insert(prefix + suffix);
                partial = std::move(next);
            }
            auto& destination = result.at(production.lhs);
            const auto previous = destination.size();
            destination.insert(partial.begin(), partial.end());
            changed |= destination.size() != previous;
        }
    } while (changed);
    return result;
}

struct Fixture {
    std::string pattern;
    std::size_t nonterminals;
    std::vector<Production> productions;
    std::string alphabet = "ab";
};

std::vector<Fixture> grammar_fixtures() {
    return {
        {"(?<S>a(?&S)b|)", 1, {{0, {t("a"), n(0), t("b")}}, {0, {}}}},
        {"a(?R)b|x", 1, {{0, {t("a"), n(0), t("b")}}, {0, {t("x")}}}, "abx"},
        {"(?<S>(?&S)(?&S)|a|)", 1, {{0, {n(0), n(0)}}, {0, {t("a")}}, {0, {}}}},
        {"(?(DEFINE)(?<A>(?&B)|a)(?<B>(?&A)|))(?&A)", 2,
         {{0, {n(1)}}, {0, {t("a")}}, {1, {n(0)}}, {1, {}}}},
        {"(?<D>(?&D))|a|", 2, {{0, {n(1)}}, {0, {t("a")}}, {0, {}}, {1, {n(1)}}}},
        {"(?<R>a(?&R)b|)aa|(?&R)ab", 2,
         {{0, {n(1), t("aa")}}, {0, {n(1), t("ab")}}, {1, {t("a"), n(1), t("b")}}, {1, {}}}},
        {"(?<R>a(?&R)b|)aa|(?&R)ab|(?<T>b(?&T)a|)aa|(?&T)ab", 3,
         {{0, {n(1), t("aa")}}, {0, {n(1), t("ab")}}, {0, {n(2), t("aa")}}, {0, {n(2), t("ab")}},
          {1, {t("a"), n(1), t("b")}}, {1, {}}, {2, {t("b"), n(2), t("a")}}, {2, {}}}},
        {"(?:aaa)*(?:aaaaaaa)*", 3,
         {{0, {n(1), n(2)}}, {1, {t("aaa"), n(1)}}, {1, {}}, {2, {t("aaaaaaa"), n(2)}}, {2, {}}}},
        {"(?:a|){1,3}(?:bb)*", 3,
         {{0, {n(1), n(2)}}, {0, {n(1), n(1), n(2)}}, {0, {n(1), n(1), n(1), n(2)}},
          {1, {t("a")}}, {1, {}}, {2, {t("bb"), n(2)}}, {2, {}}}},
        {"(?:aa|!!)(?:aa|!!)", 2,
         {{0, {n(1), n(1)}}, {1, {t("aa")}}, {1, {t("!!")}}}, "a!"}
    };
}

void public_membership_oracles() {
    for (const auto& fixture : grammar_fixtures()) {
        ++grammars;
        const crank::OccurrenceProgram program(crank::parse(fixture.pattern));
        const auto languages = bounded_languages(fixture.nonterminals, fixture.productions, 4);
        for (const auto& subject : words(fixture.alphabet, 4)) {
            ++membership_cases;
            for (const bool prune : {false, true}) {
                crank::OccurrenceMatchOptions options;
                options.prune = prune;
                crank::OccurrenceStatistics stats;
                require(program.full_match(subject, options, &stats) == languages[0].contains(subject),
                        "native membership agrees with independent finite-language oracle");
                require(!stats.limit_reached && stats.states <= options.max_states &&
                        stats.work_steps <= options.max_steps, "oracle query stays within native budgets");
            }
        }
    }
}

void nonadjacent_integer_membership() {
    // Preserve semantic coverage formerly embedded in the joined planner suite.
    for (const bool bounded : {false, true}) {
        const std::string pattern = bounded ? "x(?:aaa){1,4}y(?:aaaaaaa){1,3}z"
                                            : "x(?:aaa)*y(?:aaaaaaa)*z";
        const crank::detail::AstMachineProgram program(crank::parse(pattern));
        for (const std::size_t a : {0U,1U,3U,6U,8U,9U,12U,15U})
            for (const std::size_t b : {0U,1U,7U,8U,14U,21U,28U})
                for (const char marker : {'y','q'}) {
                    const auto subject = "x" + std::string(a,'a') + marker + std::string(b,'a') + "z";
                    const bool expected = marker == 'y' && a % 3 == 0 && b % 7 == 0 &&
                        (!bounded || (a >= 3 && a <= 12 && b >= 7 && b <= 21));
                    ++membership_cases;
                    require(program.full_match(subject) == expected,
                            "separated integer repetitions agree with run-length specification");
                }
    }
}
} // namespace

int main() {
    try {
        integer_enumeration();
        integer_edges_and_budgets();
        native_multi_entry();
        public_membership_oracles();
        nonadjacent_integer_membership();
        std::cout << "constraint tests passed: " << checks << " checks; "
                  << integer_cases << " integer rows; " << budget_cases << " budget cuts; "
                  << any_cases << " native multi-entry cases; " << grammars << " grammar oracles; "
                  << membership_cases << " membership cases\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "constraint tests failed after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
