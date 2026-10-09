#include "crank.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

// Supplemental public-facade correctness check: exactly 64 serial production
// API calls, 32 Boolean and 32 observer calls. No timings or concurrency.
using namespace crank;
namespace {
const std::string arithmetic = R"((?(DEFINE)(?<E>(?&E)\+(?&E)|(?&E)\*(?&E)|\((?&E)\)|[a-z]+))(?:(?&E);)+)";
const std::string transfer = R"((?(DEFINE)(?<Q>(?&Q)/(?&Q)|(?&Q)\|(?&Q)|\[(?&Q)\]|[xy]+))(?:(?&Q)#)+)";
struct Subject { std::string bytes; bool truth; };
std::string context;
std::size_t checks = 0, calls = 0, boolean_calls = 0, observer_calls = 0;
std::size_t subject_matches = 0, subject_nonmatches = 0;
void require(bool value, const std::string& why) {
    ++checks;
    if (!value) throw std::runtime_error(context + ": " + why);
}
void counted(std::size_t& group) {
    require(calls < 64, "public production-call cap exceeded"); ++calls; ++group;
}
RunOptions options(bool original = false) {
    RunOptions result; result.backend = Backend::Auto;
    result.occurrence.max_states = 200000; result.occurrence.max_steps = 1000000;
    result.occurrence.max_occurrences = 200000;
    result.occurrence.max_call_depth = 32; result.occurrence.max_repeat = 16;
    if (original) {
        // The existing original owner executes with all optional source views
        // disabled, including the arithmetic-chain option under examination.
        result.occurrence.normalize_arithmetic_chains = false;
        result.occurrence.normalized_exact_programs = false;
    }
    return result;
}
void same_source(const Ast& expected, const Ast& actual) {
    require(expected.root == actual.root && expected.nodes.size() == actual.nodes.size() &&
        expected.capture_names == actual.capture_names && expected.capture_nodes == actual.capture_nodes &&
        expected.count_names == actual.count_names && expected.count_nodes == actual.count_nodes &&
        expected.count_paths.empty() && actual.count_paths.empty() &&
        expected.has_references == actual.has_references && expected.has_counts == actual.has_counts &&
        expected.has_calls == actual.has_calls && expected.has_occurrence_paths == actual.has_occurrence_paths &&
        expected.has_extended == actual.has_extended, "public source identity or metadata changed");
    for (NodeId id = 0; id < expected.nodes.size(); ++id) {
        const auto& a = expected.nodes[id]; const auto& b = actual.nodes[id];
        require(a.kind == b.kind && a.characters == b.characters && a.children == b.children &&
            a.lower == b.lower && a.upper == b.upper && a.capture_id == b.capture_id &&
            a.call_target == b.call_target && a.bind_count == b.bind_count && a.use_count == b.use_count &&
            a.count_path == b.count_path && a.forward == b.forward && a.nullable == b.nullable &&
            a.minimum_length == b.minimum_length && a.stateful == b.stateful,
            "public source node changed at " + std::to_string(id));
    }
}
void original_observation(const RunStatistics& stats) {
    require(stats.occurrence.normalized_exact_attempts == 0 && stats.occurrence.selected_source_view == no_id,
        "Boolean view entered original or observed public execution");
}
void selected_chain(const RunStatistics& stats) {
    require(stats.backend == "native-ast" && stats.occurrence.selected_source_view == 4 &&
        stats.occurrence.normalized_exact_attempts == 1 && stats.occurrence.lowered_predictive_attempts == 1,
        "public Auto did not execute the arithmetic-chain predictor");
}
bool same_trace(const std::optional<TracedOutcome>& a, const std::optional<TracedOutcome>& b) {
    if (bool(a) != bool(b)) return false; if (!a) return true;
    if (a->outcome != b->outcome || a->occurrences.size() != b->occurrences.size()) return false;
    for (std::size_t id = 0; id < a->occurrences.size(); ++id) {
        const auto& x = a->occurrences[id]; const auto& y = b->occurrences[id];
        if (x.id != y.id || x.parent != y.parent || x.kind != y.kind || x.source_id != y.source_id ||
            x.source_node != y.source_node || x.name != y.name || x.span != y.span || x.count != y.count ||
            x.iteration != y.iteration || x.depth != y.depth || x.height != y.height ||
            x.source_rule != y.source_rule || x.closed != y.closed || x.children != y.children) return false;
    }
    return true;
}
void observers(const Program& original, const Program& warmed, const std::vector<Subject>& inputs) {
    require(warmed.source_portfolio_snapshot().views[4].preparation.status == detail::SourcePreparationStatus::Ready,
        "public observer owner has not been warmed by Boolean preparation");
    for (const auto& input : inputs) {
        context = "public warm observer " + input.bytes;
        auto old_options = options(true), new_options = options(); RunStatistics a, b;
        counted(observer_calls); const auto first_a = original.first_match(input.bytes, old_options, &a);
        counted(observer_calls); const auto first_b = warmed.first_match(input.bytes, new_options, &b);
        require(first_a == first_b && bool(first_a) == input.truth, "first witness or capture equality failed");
        original_observation(a); original_observation(b);
        counted(observer_calls); const auto trace_a = original.first_trace(input.bytes, old_options, &a);
        counted(observer_calls); const auto trace_b = warmed.first_trace(input.bytes, new_options, &b);
        require(same_trace(trace_a, trace_b) && bool(trace_a) == input.truth,
            "original trace equality failed after Boolean preparation");
        original_observation(a); original_observation(b);
        // Public recursive enumeration explicitly requires Occurrences. This
        // query retains its existing history path and makes no priority claim.
        old_options.backend = new_options.backend = Backend::Occurrences;
        counted(observer_calls); const auto outcomes_a = original.outcomes(input.bytes, old_options, &a);
        counted(observer_calls); const auto outcomes_b = warmed.outcomes(input.bytes, new_options, &b);
        require(outcomes_a == outcomes_b && !outcomes_a.empty() == input.truth,
            "richer outcome equality failed after Boolean preparation");
        original_observation(a); original_observation(b);
        old_options.backend = new_options.backend = Backend::Auto;
        counted(observer_calls); const auto ends_a = original.endpoints(input.bytes, 0, old_options, &a);
        counted(observer_calls); const auto ends_b = warmed.endpoints(input.bytes, 0, new_options, &b);
        require(ends_a == ends_b, "endpoint equality failed after Boolean preparation");
        original_observation(a); original_observation(b);
    }
    require(original.source_portfolio_snapshot().views[4].preparation.status == detail::SourcePreparationStatus::Unattempted,
        "the original/observer path prepared the arithmetic view");
}
void fixture(const std::string& pattern, const std::vector<Subject>& inputs, const std::vector<Subject>& observer_inputs) {
    const auto source = parse(pattern);
    Program reused_original(pattern), reused_auto(pattern);
    same_source(source, reused_original.ast()); same_source(source, reused_auto.ast());
    for (const auto& input : inputs) {
        context = "public Boolean " + input.bytes;
        Program fresh_original(pattern), fresh_auto(pattern); RunStatistics a, b;
        counted(boolean_calls); const bool original_fresh = fresh_original.full_match(input.bytes, options(true), &a);
        counted(boolean_calls); const bool auto_fresh = fresh_auto.full_match(input.bytes, options(), &b);
        require(original_fresh == input.truth && auto_fresh == input.truth, "fresh public Boolean truth differs");
        original_observation(a); if (input.truth) selected_chain(b);
        counted(boolean_calls); const bool original_reused = reused_original.full_match(input.bytes, options(true), &a);
        counted(boolean_calls); const bool auto_reused = reused_auto.full_match(input.bytes, options(), &b);
        require(original_reused == input.truth && auto_reused == input.truth, "reused public Boolean truth differs");
        original_observation(a); if (input.truth) selected_chain(b);
        same_source(source, fresh_original.ast()); same_source(source, fresh_auto.ast());
        input.truth ? ++subject_matches : ++subject_nonmatches;
    }
    observers(reused_original, reused_auto, observer_inputs);
    same_source(source, reused_original.ast()); same_source(source, reused_auto.ast());
}
}
int main() {
    try {
        fixture(arithmetic, {{"a;", true}, {"a+a+a;", true}, {"a+;", false}, {"A;", false}},
            {{"a+a+a;", true}, {"a+;", false}});
        fixture(transfer, {{"x#", true}, {"xy/[x|y]#", true}, {"x/#", false}, {"z#", false}},
            {{"xy/[x|y]#", true}, {"x/#", false}});
        require(calls == 64 && boolean_calls == 32 && observer_calls == 32,
            "frozen supplemental call allocation changed");
        std::cout << "test_arithmetic_chain_dispatch passed checks=" << checks << " calls=" << calls
            << " public_boolean=" << boolean_calls << " public_observers=" << observer_calls
            << " boolean_subject_match=" << subject_matches << " boolean_subject_no_match=" << subject_nonmatches << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "test_arithmetic_chain_dispatch HARNESS_FAILURE calls=" << calls << " " << error.what() << '\n'; return 1;
    } catch (...) {
        std::cerr << "test_arithmetic_chain_dispatch HARNESS_FAILURE calls=" << calls << " unexpected nonstandard exception\n"; return 1;
    }
}
