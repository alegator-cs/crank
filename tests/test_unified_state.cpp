#include "occurrence_matcher.hpp"
#include "state_matcher.hpp"

#include <algorithm>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
std::size_t checks = 0;
void require(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
template<class Exception, class Function>
void raises(Function&& function, const char* message) {
    bool caught = false;
    try { function(); } catch (const Exception&) { caught = true; }
    require(caught, message);
}
std::vector<std::string> words(unsigned maximum) {
    std::vector<std::string> result{""};
    std::size_t first = 0, end = 1;
    for (unsigned length = 0; length < maximum; ++length) {
        for (std::size_t i = first; i < end; ++i)
            for (char byte : {'a', 'b'}) result.push_back(result[i] + byte);
        first = end; end = result.size();
    }
    return result;
}

void scalar_observations() {
    auto ast = crank::parse("(a?){3}");
    const auto outcomes = crank::match_states(ast, "a", 0, false);
    std::set<std::pair<std::size_t, crank::Span>> actual;
    for (const auto& outcome : outcomes) {
        require(outcome.captures.size() == 2 && outcome.captures[1].has_value(),
                "nullable capture is set even when empty");
        actual.emplace(outcome.end, *outcome.captures[1]);
    }
    require(actual == std::set<std::pair<std::size_t, crank::Span>>{
            {0, {0, 0}}, {1, {0, 1}}, {1, {1, 1}}},
            "epsilon padding must preserve final capture placement");
    ast = crank::parse("((a?))*");
    crank::MatchStatistics boolean_stats, scalar_stats;
    require(crank::match_exists(ast, "aaaaaaaa", 0, true, {}, &boolean_stats),
            "nullable projected closure accepts");
    require(!crank::match_states(ast, "aaaaaaaa", 0, true, {}, &scalar_stats).empty(),
            "nullable scalar closure terminates with all outcomes");
    require(boolean_stats.states < scalar_stats.states,
            "unread capture projection saves real allocation units");
    crank::MatchOptions exact;
    exact.max_states = boolean_stats.states;
    require(crank::match_exists(ast, "aaaaaaaa", 0, true, exact),
            "projected query fits measured budget");
    raises<crank::ResourceLimit>([&] { (void)crank::match_states(ast, "aaaaaaaa", 0, true, exact); },
            "full capture reporting may not silently project at a limit");

    crank::OccurrenceMatchOptions history_options;
    history_options.share_calls = false;
    const crank::OccurrenceProgram program(crank::parse("(?<x>a|b){3}"));
    crank::OccurrenceStatistics scalar, trace;
    require(program.first_match("aba", history_options, &scalar).has_value(), "scalar witness exists");
    const auto witness = program.first_traced_match("aba", history_options, &trace);
    require(witness && witness->occurrences.size() == 4, "trace keeps each group invocation");
    require(scalar.occurrences == 0 && trace.occurrences > 0,
            "scalar query erases history allocations; explicit trace retains them");
}

void independent_languages() {
    const auto repeated_forward = crank::parse("(?:\\k<x>(?<x>a|b))*");
    const auto optional_back = crank::parse("(?<x>a)?\\k<x>");
    const auto same_count = crank::parse("a{#n}b{=n}");
    const auto nullable_count = crank::parse("(?:){#n:2,3}a{=n}");
    const auto path_count = crank::parse("(?<x>a{1,3})b{=x[0].@bytes}");
    for (const auto& word : words(7)) {
        bool pairs = word.size() % 2 == 0;
        for (std::size_t i = 0; pairs && i < word.size(); i += 2) pairs = word[i] == word[i + 1];
        require(crank::match_exists(repeated_forward, word) == pairs,
                "forward reference binds next close on each iteration");
        require(crank::match_exists(optional_back, word) == (word == "aa"),
                "unset capture differs from captured epsilon");
        std::size_t split = word.find_first_not_of('a');
        if (split == std::string::npos) split = word.size();
        bool balanced = split * 2 == word.size() &&
            std::all_of(word.begin() + static_cast<std::ptrdiff_t>(split), word.end(),
                        [](char byte) { return byte == 'b'; });
        const auto counted = crank::match_states(same_count, word);
        require(!counted.empty() == balanced, "count equality matches independent construction");
        if (!counted.empty()) require(counted.front().counts.at(0) == split,
                                      "scalar count remains observable");
        require(crank::match_exists(nullable_count, word) == (word == "aa" || word == "aaa"),
                "finite nullable binder preserves count despite no byte progress");
        require(crank::match_exists(path_count, word) == (balanced && split >= 1 && split <= 3),
                "facade honors occurrence paths");
    }
    auto stale = path_count;
    stale.has_extended = stale.has_occurrence_paths = false;
    require(crank::match_exists(stale, "aabb") && !crank::match_exists(stale, "aab"),
            "actual path nodes require history even with stale advisory metadata");
}

void absolute_positions_and_limits() {
    const auto ast = crank::parse("(?<x>a*)\\k<x>");
    require(crank::match_endpoints(ast, "xaaaab", 1) == std::vector<std::size_t>{1, 3, 5},
            "prefix endpoints retain absolute offsets");
    for (bool full : {false, true}) {
        const auto all = crank::match_states(ast, "xaaaa", 1, full);
        const auto first = crank::match_first(ast, "xaaaa", 1, full);
        require(first && std::binary_search(all.begin(), all.end(), *first),
                "first result is a complete observable result");
        for (const auto& result : all)
            require(result.captures[0] == crank::Span{1, result.end},
                    "whole-match capture uses original coordinates");
    }
    raises<std::out_of_range>([&] { (void)crank::match_exists(ast, "a", 2); }, "invalid start throws");
    for (int budget = 0; budget < 3; ++budget) {
        crank::MatchOptions options;
        if (budget == 0) options.max_steps = 0;
        if (budget == 1) options.max_states = 0;
        if (budget == 2) options.max_repeat = 0;
        raises<crank::ResourceLimit>([&] {
            (void)crank::match_exists(crank::parse("(a){2}"), "aa", 0, true, options);
        }, "exhausted query remains unresolved, never no-match");
    }
    // An early proof is enough for a Boolean query, even if another branch has
    // exhausted its repeat budget. Exhaustive outcome enumeration must report it.
    crank::MatchOptions options;
    options.max_repeat = 1;
    const auto choice = crank::parse("a{2}|aa");
    require(crank::match_exists(choice, "aa", 0, true, options), "independent witness survives deferred branch");
    raises<crank::ResourceLimit>([&] { (void)crank::match_states(choice, "aa", 0, true, options); },
            "exhaustive query cannot hide unexplored outcomes");
    const auto recursive = crank::parse("(?<x>a(?&x)?b)");
    require(crank::match_exists(recursive, "aaabbb") && !crank::match_exists(recursive, "aabbb"),
            "same state facade supports source calls");
}
}
int main() {
    scalar_observations();
    independent_languages();
    absolute_positions_and_limits();
    std::cout << "unified state VM: " << checks << " checks passed\n";
}
