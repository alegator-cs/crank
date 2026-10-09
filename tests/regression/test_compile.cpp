#include "ast_machine.hpp"
#include "occurrence_analysis.hpp"
#include "occurrence_matcher.hpp"

#include <atomic>
#include <barrier>
#include <cstddef>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
std::atomic<std::size_t> checks{0};

void require(bool condition, const std::string& context) {
    checks.fetch_add(1, std::memory_order_relaxed);
    if (!condition) throw std::runtime_error(context);
}

// Traces deliberately have no scalar-only equality operator. Compare every
// public record field so lost names, rule flags, tree edges, and invocation
// metrics cannot disappear behind an equal final capture environment.
bool same_record(const crank::OccurrenceRecord& a, const crank::OccurrenceRecord& b) {
    return std::tie(a.id, a.parent, a.kind, a.source_id, a.source_node, a.name,
                    a.span, a.count, a.iteration, a.depth, a.height, a.source_rule,
                    a.closed, a.children) ==
           std::tie(b.id, b.parent, b.kind, b.source_id, b.source_node, b.name,
                    b.span, b.count, b.iteration, b.depth, b.height, b.source_rule,
                    b.closed, b.children);
}

bool same_trace(const std::optional<crank::TracedOutcome>& a,
                const std::optional<crank::TracedOutcome>& b) {
    if (bool(a) != bool(b)) return false;
    if (!a) return true;
    if (a->outcome != b->outcome || a->occurrences.size() != b->occurrences.size())
        return false;
    for (std::size_t index = 0; index < a->occurrences.size(); ++index)
        if (!same_record(a->occurrences[index], b->occurrences[index])) return false;
    return true;
}

struct Snapshot {
    bool matched;
    std::vector<std::size_t> endpoints;
    std::optional<crank::Outcome> first;
    std::vector<crank::Outcome> full_outcomes;
    std::vector<crank::Outcome> prefix_outcomes;
    std::optional<crank::TracedOutcome> trace;
};

crank::OccurrenceMatchOptions options(bool shared, bool prune = true) {
    crank::OccurrenceMatchOptions result;
    result.share_calls = shared;
    result.prune = prune;
    return result;
}

Snapshot snapshot(const crank::OccurrenceProgram& program, std::string_view subject,
                  crank::OccurrenceMatchOptions execution) {
    const std::string wrapped = "!" + std::string(subject) + "!";
    return {program.full_match(subject, execution),
            program.endpoints(wrapped, 1, execution),
            program.first_match(subject, execution),
            program.match_outcomes(subject, 0, true, execution),
            program.match_outcomes(wrapped, 1, false, execution),
            program.first_traced_match(subject, execution)};
}

void compare(const Snapshot& actual, const Snapshot& expected, const std::string& label) {
    require(actual.matched == expected.matched, label + ": membership");
    require(actual.endpoints == expected.endpoints, label + ": offset endpoints");
    require(actual.first == expected.first, label + ": scalar first witness");
    require(actual.full_outcomes == expected.full_outcomes, label + ": every full outcome");
    require(actual.prefix_outcomes == expected.prefix_outcomes, label + ": every prefix outcome");
    require(same_trace(actual.trace, expected.trace), label + ": exact occurrence trace");
}

struct Fixture {
    std::string name, pattern, subject, mismatch;
    bool shared;
};

const std::vector<Fixture> fixtures{
    {"pure recursive captures", "(?(DEFINE)(?<S>a(?&S)b|)(?<unused>x))(?&S)",
     "aaabbb", "aaabbbx", true},
    {"stateful height equation", "(?(DEFINE)(?<S>a(?&S)b|))(?&S)c{=S[0].@height}",
     "aabbcc", "aabbc", false},
    {"stateful mixed observations",
     "(?(DEFINE)(?<S>a(?&S)b|))(?<W>[xy]{#n:1,2})(?&S)\\k<W>d{=n}c{=S[0].@height}",
     "xyaabbxyddcc", "xyaabbyxddcc", false}
};

void invalidate_source(crank::Ast& source) {
    // Change terminal data while preserving indices first, then destroy all
    // original vectors. A compiled value must retain neither kind of alias.
    for (auto& node : source.nodes) {
        node.characters.reset();
        node.children.clear();
        node.call_target = crank::no_id;
    }
    source = crank::parse("z");
}

void ownership_and_query_routes() {
    for (const auto& fixture : fixtures) {
        const crank::OccurrenceProgram reference(crank::parse(fixture.pattern));
        require(reference.supports_shared_calls() == fixture.shared,
                fixture.name + ": intended execution certificate");
        const auto expected = snapshot(reference, fixture.subject, options(false));
        require(expected.matched && expected.first && expected.trace,
                fixture.name + ": nonvacuous successful fixture");
        require(!reference.full_match(fixture.mismatch), fixture.name + ": specified mismatch");
        for (const bool shared : {false, true})
            for (const bool prune : {false, true})
                compare(snapshot(reference, fixture.subject, options(shared, prune)), expected,
                        fixture.name + ": all query routes before mutation");

        std::optional<crank::OccurrenceProgram> survivor;
        {
            auto source = crank::parse(fixture.pattern);
            const crank::OccurrenceProgram original(source);
            crank::OccurrenceProgram copied(original);
            crank::OccurrenceProgram moved(std::move(copied));
            crank::OccurrenceProgram assigned(crank::parse("z"));
            assigned = original;
            crank::OccurrenceProgram move_assigned(crank::parse("z"));
            move_assigned = std::move(assigned);
            survivor.emplace(std::move(move_assigned));

            invalidate_source(source);
            const crank::OccurrenceProgram* compiled_values[] = {&original, &moved, &*survivor};
            for (const auto* program : compiled_values)
                for (const bool shared : {false, true})
                    compare(snapshot(*program, fixture.subject, options(shared)), expected,
                            fixture.name + ": compiled value survives source mutation");
        }
        // The last program copy now outlives its source and every constructor
        // or assignment donor. Its history path still needs the same analysis.
        for (const bool shared : {false, true})
            for (const bool prune : {false, true}) {
                compare(snapshot(*survivor, fixture.subject, options(shared, prune)), expected,
                        fixture.name + ": final owner after donor destruction");
                require(!survivor->full_match(fixture.mismatch, options(shared, prune)),
                        fixture.name + ": negative query after donor destruction");
            }
    }
}

void direct_native_ownership() {
    std::optional<crank::detail::AstMachineProgram> survivor;
    {
        auto source = crank::parse(fixtures.front().pattern);
        const crank::detail::AstMachineProgram original(source);
        auto copied = original;
        crank::detail::AstMachineProgram moved(std::move(copied));
        auto assigned = crank::detail::AstMachineProgram(crank::parse("z"));
        assigned = original;
        moved = std::move(assigned);
        survivor.emplace(std::move(moved));
        invalidate_source(source);
        require(original.full_match("aabb"), "direct native source mutation");
    }
    require(survivor->full_match("aabb"), "direct native final owner matches");
    require(!survivor->full_match("aabbb"), "direct native final owner rejects");
    require(survivor->endpoints("!aabb!", 1) == std::vector<std::size_t>{1, 5},
            "direct native final owner enumerates exact offset endpoints");
    const auto witness = survivor->first_match("aabb");
    require(witness && witness->captures.at(0) == crank::Span{0, 4} &&
            witness->captures.at(1) == crank::Span{0, 4} && !witness->captures.at(2),
            "direct native final owner retains executed and unused captures");
}

void analysis_handle_ownership() {
    static_assert(std::is_same_v<
        decltype(std::declval<const crank::detail::AstMachineProgram&>().shared_analysis()),
        std::shared_ptr<const crank::OccurrenceAnalysis>>);
    std::shared_ptr<const crank::OccurrenceAnalysis> retained;
    crank::NodeId root = crank::no_id;
    {
        auto source = crank::parse("(?<S>a(?&S)b|)");
        root = source.root;
        const crank::detail::AstMachineProgram original(source);
        auto copied_program = original;
        crank::detail::AstMachineProgram moved_program(std::move(copied_program));
        auto handle = moved_program.shared_analysis();
        require(bool(handle), "compiled native program provides an immutable analysis handle");
        retained = handle;
        handle.reset();
        invalidate_source(source);
    }
    // Every AST and program handle is gone. The alias must keep the storage
    // containing the completed analysis alive, not merely its raw address.
    require(retained->length(root).accepts(0) && retained->length(root).accepts(6),
            "analysis alias survives its source AST and every compiled program");
    require(!retained->length(root).accepts(7),
            "retained analysis still rejects impossible recursive lengths");
    auto copied_handle = retained;
    auto moved_handle = std::move(retained);
    moved_handle.reset();
    require(copied_handle->length(root).accepts(100) &&
            !copied_handle->length(root).accepts(101),
            "copied analysis handle owns a lifetime independent of its donor");
}

void convenience_flags_are_not_semantics() {
    const auto& fixture = fixtures.front();
    const crank::OccurrenceProgram reference(crank::parse(fixture.pattern));
    const auto expected = snapshot(reference, fixture.subject, options(false));
    for (const bool forged : {false, true}) {
        auto source = crank::parse(fixture.pattern);
        source.has_references = source.has_counts = source.has_calls = forged;
        source.has_extended = source.has_occurrence_paths = forged;
        for (auto& node : source.nodes) {
            node.stateful = forged;
            node.nullable = !forged;
            node.minimum_length = forged ? 99999 : 0;
        }
        require(crank::detail::AstMachineProgram::eligible(source),
                "forged positive and negative convenience flags preserve syntax eligibility");
        const crank::OccurrenceProgram compiled(source);
        require(compiled.supports_shared_calls(), "pure syntax still uses shared requests");
        invalidate_source(source);
        for (const bool shared : {false, true})
            for (const bool prune : {false, true})
                compare(snapshot(compiled, fixture.subject, options(shared, prune)), expected,
                        "forged metadata leaves shared and history observables unchanged");
    }
}

void ambiguous_outcomes_survive_history_dispatch() {
    const crank::OccurrenceProgram program(crank::parse("(a{0,2})(a{0,2})"));
    std::vector<crank::Outcome> expected;
    for (std::size_t split = 0; split <= 2; ++split) {
        crank::Outcome outcome;
        outcome.end = 2;
        outcome.captures = {crank::Span{0, 2}, crank::Span{0, split}, crank::Span{split, 2}};
        expected.push_back(std::move(outcome));
    }
    const auto reference_trace = program.first_traced_match("aa", options(false));
    require(bool(reference_trace), "ambiguous fixture retains a complete trace");
    for (const bool shared : {false, true})
        for (const bool prune : {false, true}) {
            const auto execution = options(shared, prune);
            require(program.match_outcomes("aa", 0, true, execution) == expected,
                    "all three equal-end unequal-capture outcomes survive compile sharing");
            require(same_trace(program.first_traced_match("aa", execution), reference_trace),
                    "exact ambiguous history selection is independent of shared-call option");
        }
}

void concurrent_read_only_queries() {
    // Programs and expected values are fully initialized before publication.
    // Each query owns its subject, result, options, and exception slot. Only
    // the const compiled program is shared; no mutable statistics are shared.
    std::vector<crank::OccurrenceProgram> programs;
    std::vector<Snapshot> expected;
    for (const auto& fixture : fixtures) {
        programs.emplace_back(crank::parse(fixture.pattern));
        expected.push_back(snapshot(programs.back(), fixture.subject, options(false)));
    }
    const auto& published = std::as_const(programs);
    constexpr std::size_t reader_count = 4;
    constexpr unsigned rounds = 8;
    std::barrier start(static_cast<std::ptrdiff_t>(reader_count));
    std::vector<std::exception_ptr> failures(reader_count);
    std::vector<std::jthread> readers;
    readers.reserve(reader_count);
    for (std::size_t reader = 0; reader < reader_count; ++reader)
        readers.emplace_back([&, reader] {
            start.arrive_and_wait();
            try {
                for (unsigned round = 0; round < rounds; ++round)
                    for (std::size_t index = 0; index < fixtures.size(); ++index) {
                        const auto& fixture = fixtures[index];
                        const auto execution = options((reader + round) % 2 != 0,
                                                       (reader + round / 2) % 2 != 0);
                        compare(snapshot(published[index], fixture.subject, execution), expected[index],
                                fixture.name + ": simultaneous reader " + std::to_string(reader));
                        require(!published[index].full_match(fixture.mismatch, execution),
                                fixture.name + ": simultaneous negative query");
                    }
            } catch (...) {
                failures[reader] = std::current_exception();
            }
        });
    readers.clear(); // jthread destruction joins before examining exception slots.
    for (const auto& failure : failures) if (failure) std::rethrow_exception(failure);
}
} // namespace

int main() {
    try {
        ownership_and_query_routes();
        direct_native_ownership();
        analysis_handle_ownership();
        convenience_flags_are_not_semantics();
        ambiguous_outcomes_survive_history_dispatch();
        concurrent_read_only_queries();
        std::cout << "PASS: " << checks.load(std::memory_order_relaxed)
                  << " compile ownership and query isolation checks; 4 concurrent readers\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
