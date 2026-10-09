#include "ast_machine.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace crank;
namespace {
std::size_t checks = 0, queries = 0, fallbacks = 0;
std::size_t boolean_fallbacks = 0, endpoint_fallbacks = 0;
std::size_t seed_fallbacks = 0, assisted_fallbacks = 0;
std::string context;

void require(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(context + ": " + message);
}

OccurrenceMatchOptions ordinary(bool prune) {
    OccurrenceMatchOptions result;
    result.max_states = 4'000'000;
    result.max_steps = 16'000'000;
    result.prune = prune;
    result.frontier_indexes = true;
    result.aggregate_returns = false;
    result.aggregate_suffix_returns = false;
    result.local_regular_regions = false;
    // Keep the historical retry ladder under test. New predictive execution
    // and consumer guards have a separate combined exact-budget gate.
    result.predictive_recursion = false; result.exact_end_spines=false; result.predictive_exact_rims=false;
    result.regional_execution = result.regional_constraints = false;
    // Preserve this historical representation under the newer root tiers.
    result.bounded_word_prediction=result.deterministic_programs=false;
    result.endpoint_consumer_guards = false;
    // This suite isolates the original aggregate/local representation tier.
    // New frontier accelerations are crossed separately in the combined gate;
    // leaving them implicit would change the purported ordinary control.
    result.inline_nullable_frontiers = false;
    result.aggregate_interval_runs = false;
    result.aggregate_prefix_blocks = false;
    return result;
}

std::vector<OccurrenceMatchOptions> accelerated(const OccurrenceMatchOptions& base,
                                               const OccurrenceStatistics& budget) {
    std::vector<OccurrenceMatchOptions> result;
    // Frontier indexing is intentionally fixed: acceleration may not borrow
    // any states or steps from the ordinary algorithm being accelerated.
    for (unsigned mask = 0; mask != 8; ++mask) for (bool cursor : {false, true}) {
        auto option = base;
        option.max_states = budget.states;
        option.max_steps = budget.work_steps;
        option.aggregate_returns = mask & 1U;
        option.aggregate_suffix_returns = mask & 2U;
        option.local_regular_regions = mask & 4U;
        option.regular_cursor_frontiers = cursor;
        result.push_back(option);
    }
    return result;
}

void resolved(const OccurrenceStatistics& got, const OccurrenceStatistics& budget,
              std::size_t& api_fallbacks) {
    require(!got.limit_reached, "successful result retained unresolved status");
    require(got.states <= budget.states, "ordinary state allowance exceeded");
    require(got.work_steps <= budget.work_steps, "ordinary work allowance exceeded");
    require(got.native_acceleration_fallbacks <= 1, "fallback repeated");
    if (got.native_acceleration_fallbacks) {
        ++fallbacks; ++api_fallbacks;
        require(got.native_acceleration_discarded_states || got.native_acceleration_discarded_steps,
                "discarded attempt was not reported");
        // Restart must reproduce the ordinary successful execution exactly;
        // abandoned optional work has its own counters and its own allowance.
        require(got.states == budget.states && got.work_steps == budget.work_steps,
                "fallback did not restore the complete ordinary execution");
    }
}

struct Fixture {
    std::string name, pattern;
    std::vector<std::string> inputs;
};

std::vector<std::string> dense_inputs() {
    std::vector<std::string> result;
    for (std::size_t n : {15, 16, 17}) {
        result.push_back(std::string(n, 'a'));
        result.push_back(std::string(n - 1, 'a') + 'b');
    }
    return result;
}

void check(const Fixture& fixture) {
    const auto ast = parse(fixture.pattern);
    detail::AstMachineProgram program(ast);
    detail::AtomicExclusions empty_exclusions(ast.nodes.size());
    std::vector<NodeId> seeds{ast.root};
    for (std::size_t id = 1; id < ast.capture_nodes.size(); ++id)
        if (ast.capture_nodes[id] != no_id) seeds.push_back(ast.capture_nodes[id]);
    // Duplicated and reordered seeds exercise accepting-request protection
    // when the acceleration fails after those seeds have been installed.
    std::reverse(seeds.begin(), seeds.end());
    seeds.push_back(seeds.front());
    for (const auto& input : fixture.inputs) for (bool prune : {false, true}) {
        const auto base = ordinary(prune);
        const auto prefix = fixture.name + " / " + input + " / prune=" + std::to_string(prune);
        context = prefix + " / Boolean control";
        OccurrenceStatistics budget, got;
        const auto expected = program.full_match(input, base, &budget); ++queries;
        std::size_t ordinal = 0;
        for (const auto& option : accelerated(base, budget)) {
            context = prefix + " / Boolean mode=" + std::to_string(ordinal);
            require(program.full_match(input, option, &got) == expected, "Boolean result changed"); ++queries;
            resolved(got, budget, boolean_fallbacks);
            context = prefix + " / assisted mode=" + std::to_string(ordinal++);
            require(program.full_match_assisted(input, option, empty_exclusions, &got) == expected,
                    "empty proof table changed Boolean result"); ++queries;
            resolved(got, budget, assisted_fallbacks);
        }

        std::vector<std::size_t> starts;
        for (std::size_t start = 0; start <= input.size(); ++start)
            if (input.size() <= 17 || start <= 1 || start == input.size() / 2 || start == input.size())
                starts.push_back(start);
        for (const auto start : starts) {
            context = prefix + " / endpoint control start=" + std::to_string(start);
            const auto ends = program.endpoints(input, start, base, &budget); ++queries;
            ordinal = 0;
            for (const auto& option : accelerated(base, budget)) {
                context = prefix + " / endpoints start=" + std::to_string(start) + " mode=" + std::to_string(ordinal++);
                require(program.endpoints(input, start, option, &got) == ends, "endpoint row changed"); ++queries;
                resolved(got, budget, endpoint_fallbacks);
            }
        }

        context = prefix + " / seed control";
        const auto any = program.full_match_any(seeds, input, base, &budget); ++queries;
        ordinal = 0;
        for (const auto& option : accelerated(base, budget)) {
            context = prefix + " / seeds mode=" + std::to_string(ordinal++);
            require(program.full_match_any(seeds, input, option, &got) == any, "accepting seed union changed"); ++queries;
            resolved(got, budget, seed_fallbacks);
        }

        context = prefix + " / witness control";
        const auto witness = program.first_match(input, base, &budget); ++queries;
        auto rich = accelerated(base, budget).back();
        require(program.first_match(input, rich, &got) == witness, "witness identity changed"); ++queries;
        require(got.states == budget.states && got.work_steps == budget.work_steps,
                "Boolean acceleration changed witness work");
        require(!got.native_acceleration_fallbacks && !got.aggregate_word_tasks && !got.suffix_word_tasks &&
                !got.regular_region_requests, "witness entered optional pure acceleration");
    }
}
} // namespace

int main() {
    try {
        const std::vector<Fixture> fixtures{
            {"nullable Catalan", "(?<S>(?&S)(?&S)|a|)", dense_inputs()},
            {"bounded Catalan repetition", "(?<S>(?&S)(?&S)|a){2,4}", dense_inputs()},
            {"bounded nullable Catalan repetition", "(?<S>(?&S)(?&S)|a|){2,4}", dense_inputs()},
            {"mixed regular alternatives", "(?<S>a(?&S)?b|ab|aab)",
             {"", "ab", "aab", "aaabbb", "aaabab", "abab", "aabbba"}},
            {"nullable local cursor", "(?<S>(?:a?b?){2,3}(?&S)?a?b?)",
             {"", "ab", "ba", "aaaabbbb", "abababab", "abababbac"}},
            {"ambiguous regular island", "(?<S>(?&S)(?&S)|a(?:b|a)?)",
             {"", "a", "ab", "aaaaaaa", "aabaaab", "aaaaaab", "aaaacaa"}},
            {"shared island distinct callers", "(?(DEFINE)(?<R>a(?&R)?b)(?<T>a?|ab))(?&T)(?&R)?(?&T)",
             {"", "a", "ab", "aab", "aaabbbab", "abaaabbb", "aababb"}},
            {"long regular late rejection", "(?<S>a[ab]*b(?&S)?ba*|)",
             {std::string(48, 'a') + "bb", std::string(48, 'a') + "bc", "aabababba"}},
        };
        for (const auto& fixture : fixtures) check(fixture);
        context = "coverage";
        require(fallbacks != 0, "no exhausted optional representation was exercised");
        std::cout << "{\"status\":\"passed\",\"fixtures\":" << fixtures.size()
                  << ",\"checks\":" << checks << ",\"native_queries\":" << queries
                  << ",\"fallbacks\":" << fallbacks << ",\"boolean_fallbacks\":" << boolean_fallbacks
                  << ",\"endpoint_fallbacks\":" << endpoint_fallbacks
                  << ",\"seed_fallbacks\":" << seed_fallbacks
                  << ",\"assisted_fallbacks\":" << assisted_fallbacks << "}\n";
    } catch (const std::exception& error) {
        std::cerr << "REPRESENTATION BUDGET FAILURE: " << context << ": " << error.what() << '\n';
        return 1;
    }
}
