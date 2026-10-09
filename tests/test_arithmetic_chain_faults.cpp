#include "ast_machine.hpp"
#include "source_view_portfolio.hpp"

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#if defined(_WIN32)
#include <malloc.h>
#endif

// Optional serial supplement to test_arithmetic_chain.cpp. No timer, helper
// thread, engine instrumentation, or compiler-specific allocation ordinal is
// required. Replacement allocation functions affect this executable only.
// There are exactly ten recognition calls. View preparation has a separate,
// measured allocation sweep capped at 128 injection points.
namespace allocation_probe {
enum class Mode { Off, Count, FailIndex, FailLowering, CancelActive, FailActive };
struct Probe {
    std::size_t allocations = 0, steps_at_trigger = 0;
    std::size_t source_view_at_trigger = crank::no_id;
    bool fired = false;
};
struct State {
    Mode mode = Mode::Off;
    Probe* probe = nullptr;
    std::size_t index = 0;
    const crank::OccurrenceStatistics* statistics = nullptr;
    std::atomic_bool* cancel = nullptr;
};
thread_local State state;

// This function must not allocate. Statistics are read only by the query's
// foreground thread; no concurrent statistics read or timing race exists.
void before_allocation() {
    auto& current = state;
    if (current.mode == Mode::Off) return;
    auto& probe = *current.probe;
    const auto index = probe.allocations++;
    if (current.mode == Mode::Count) return;
    bool trigger = current.mode == Mode::FailIndex && index == current.index;
    if (current.statistics) {
        const auto& stats = *current.statistics;
        const bool predictive = stats.selected_source_view == 4 && stats.predictive_attempts == 1;
        if (current.mode == Mode::FailLowering)
            trigger = predictive && stats.predictive_steps == 0;
        if (current.mode == Mode::CancelActive || current.mode == Mode::FailActive)
            trigger = predictive && stats.predictive_steps > 0;
    }
    if (!trigger) return;
    const auto mode = current.mode;
    current.mode = Mode::Off; // Assertions, catches and exception reports may allocate.
    probe.fired = true;
    if (current.statistics) {
        probe.steps_at_trigger = current.statistics->predictive_steps;
        probe.source_view_at_trigger = current.statistics->selected_source_view;
    }
    if (mode == Mode::CancelActive) {
        if (!current.cancel) std::abort();
        current.cancel->store(true, std::memory_order_relaxed);
        return; // Cancellation does not cause an allocation failure.
    }
    throw std::bad_alloc{};
}

class Guard {
public:
    Guard(Mode mode, Probe& probe, std::size_t index = 0,
          const crank::OccurrenceStatistics* statistics = nullptr,
          std::atomic_bool* cancel = nullptr) {
        if (state.mode != Mode::Off) std::abort();
        probe = {};
        state = {mode, &probe, index, statistics, cancel};
    }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
    ~Guard() noexcept { state = {}; }
};

void* ordinary(std::size_t bytes) {
    before_allocation();
    if (void* pointer = std::malloc(bytes ? bytes : 1)) return pointer;
    throw std::bad_alloc{};
}
void* aligned(std::size_t bytes, std::align_val_t alignment) {
    before_allocation();
    const auto boundary = static_cast<std::size_t>(alignment);
    if (!boundary) throw std::bad_alloc{};
    bytes = bytes ? bytes : 1;
#if defined(_WIN32)
    if (void* pointer = _aligned_malloc(bytes, boundary)) return pointer;
#else
    if (bytes > std::numeric_limits<std::size_t>::max() - (boundary - 1))
        throw std::bad_alloc{};
    const auto rounded = ((bytes + boundary - 1) / boundary) * boundary;
    if (void* pointer = std::aligned_alloc(boundary, rounded)) return pointer;
#endif
    throw std::bad_alloc{};
}
void aligned_free(void* pointer) noexcept {
#if defined(_WIN32)
    _aligned_free(pointer);
#else
    std::free(pointer);
#endif
}
} // namespace allocation_probe

void* operator new(std::size_t bytes) { return allocation_probe::ordinary(bytes); }
void* operator new[](std::size_t bytes) { return allocation_probe::ordinary(bytes); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }
void* operator new(std::size_t bytes, std::align_val_t alignment) {
    return allocation_probe::aligned(bytes, alignment);
}
void* operator new[](std::size_t bytes, std::align_val_t alignment) {
    return allocation_probe::aligned(bytes, alignment);
}
void operator delete(void* pointer, std::align_val_t) noexcept { allocation_probe::aligned_free(pointer); }
void operator delete[](void* pointer, std::align_val_t) noexcept { allocation_probe::aligned_free(pointer); }
void operator delete(void* pointer, std::size_t, std::align_val_t) noexcept { allocation_probe::aligned_free(pointer); }
void operator delete[](void* pointer, std::size_t, std::align_val_t) noexcept { allocation_probe::aligned_free(pointer); }
void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept {
    try { return allocation_probe::ordinary(bytes); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t bytes, const std::nothrow_t&) noexcept {
    try { return allocation_probe::ordinary(bytes); } catch (...) { return nullptr; }
}
void operator delete(void* pointer, const std::nothrow_t&) noexcept { std::free(pointer); }
void operator delete[](void* pointer, const std::nothrow_t&) noexcept { std::free(pointer); }
void* operator new(std::size_t bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    try { return allocation_probe::aligned(bytes, alignment); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    try { return allocation_probe::aligned(bytes, alignment); } catch (...) { return nullptr; }
}
void operator delete(void* pointer, std::align_val_t, const std::nothrow_t&) noexcept {
    allocation_probe::aligned_free(pointer);
}
void operator delete[](void* pointer, std::align_val_t, const std::nothrow_t&) noexcept {
    allocation_probe::aligned_free(pointer);
}

using namespace crank;
using namespace crank::detail;
namespace {
using allocation_probe::Guard;
using allocation_probe::Mode;
using allocation_probe::Probe;
constexpr std::size_t allocation_cap = 128, recognition_cap = 10;
const SourceCandidateId chain_id{4, SourceRepresentation::Predictive};
const std::string arithmetic = R"((?(DEFINE)(?<E>(?&E)\+(?&E)|(?&E)\*(?&E)|\((?&E)\)|[a-z]+))(?:(?&E);)+)";
std::string context;
std::size_t checks = 0, recognition_calls = 0, preparation_faults = 0;
std::size_t fact_stage_faults = 0, cancelled_calls = 0, runtime_allocation_failures = 0;

void require(bool condition, const std::string& why) {
    if (allocation_probe::state.mode != Mode::Off) std::abort();
    ++checks;
    if (!condition) throw std::runtime_error(context + ": " + why);
}
OccurrenceMatchOptions options(bool lowered = true) {
    OccurrenceMatchOptions result;
    result.prune = false;
    result.max_states = 200000; result.max_steps = 1000000;
    result.compiled_exact_programs = result.lowered_predictive = lowered;
    return result;
}
bool same_node(const Node& a, const Node& b) {
    return a.kind == b.kind && a.characters == b.characters && a.children == b.children &&
        a.lower == b.lower && a.upper == b.upper && a.capture_id == b.capture_id &&
        a.call_target == b.call_target && a.bind_count == b.bind_count && a.use_count == b.use_count &&
        a.count_path == b.count_path && a.forward == b.forward && a.nullable == b.nullable &&
        a.minimum_length == b.minimum_length && a.stateful == b.stateful;
}
void same_source(const Ast& a, const Ast& b) {
    require(a.nodes.size() == b.nodes.size() && a.root == b.root &&
        a.capture_names == b.capture_names && a.capture_nodes == b.capture_nodes &&
        a.count_names == b.count_names && a.count_nodes == b.count_nodes &&
        a.count_paths.empty() && b.count_paths.empty() && a.has_references == b.has_references &&
        a.has_counts == b.has_counts && a.has_calls == b.has_calls &&
        a.has_occurrence_paths == b.has_occurrence_paths && a.has_extended == b.has_extended,
        "source ownership or metadata changed");
    for (NodeId id = 0; id < a.nodes.size(); ++id)
        require(same_node(a.nodes[id], b.nodes[id]), "caller source node changed");
}
void same_charges(const SourcePortfolioSnapshot& a, const SourcePortfolioSnapshot& b) {
    require(a.view_attempts == b.view_attempts && a.candidate_attempts == b.candidate_attempts &&
        a.transformed_nodes == b.transformed_nodes && a.retained_bytes == b.retained_bytes &&
        a.analysis_steps == b.analysis_steps && a.candidate_analysis_steps == b.candidate_analysis_steps &&
        a.reserved_candidate_work == b.reserved_candidate_work, "cached failure was charged again");
}
enum class CallResult { Match, NoMatch, Cancelled, AllocationFailure, ResourceLimit, Unavailable };
template<class Action> CallResult call(Action action) {
    require(recognition_calls < recognition_cap, "recognition-call cap exceeded");
    ++recognition_calls;
    try { return action() ? CallResult::Match : CallResult::NoMatch; }
    catch (const AstExecutionCancelled&) { ++cancelled_calls; return CallResult::Cancelled; }
    catch (const std::bad_alloc&) { ++runtime_allocation_failures; return CallResult::AllocationFailure; }
    catch (const ResourceLimit&) { return CallResult::ResourceLimit; }
    catch (const std::invalid_argument&) { return CallResult::Unavailable; }
}

void view_allocation_sweep(const Ast& source) {
    context = "bounded view/fresh-fact allocation sweep";
    const auto untouched = source;
    SourceViewPortfolio counted_portfolio(source);
    Probe count;
    const BooleanSourceView* prepared = nullptr;
    { Guard guard(Mode::Count, count); prepared = counted_portfolio.prepare_view(4); }
    require(prepared && prepared->ready(), "allocation-count baseline did not prepare view");
    require(count.allocations > 0 && count.allocations <= allocation_cap,
        "view allocation count exceeds explicit cap: " + std::to_string(count.allocations));
    for (std::size_t index = 0; index < count.allocations; ++index) {
        SourceViewPortfolio portfolio(source);
        Probe failure;
        const BooleanSourceView* view = nullptr;
        { Guard guard(Mode::FailIndex, failure, index); view = portfolio.prepare_view(4); }
        require(failure.fired && !view, "view allocation failure produced a ready value");
        const auto before = portfolio.snapshot();
        const auto& row = before.views[4].preparation;
        require(row.status == SourcePreparationStatus::AllocationFailure && row.retained_bytes == 0 &&
            !row.nullable_complete && !row.first_complete && !row.productive_complete && !row.minimum_complete,
            "failed view published completed facts or an incorrect status");
        if (row.arithmetic_chain_rewrites == 1) {
            require(row.cloned_nodes == 5, "post-rewrite allocation fault lost construction statistics");
            ++fact_stage_faults;
        }
        Probe cached;
        const BooleanSourceView* repeated = nullptr;
        { Guard guard(Mode::Count, cached); repeated = portfolio.prepare_view(4); }
        require(!repeated && cached.allocations == 0, "negative view cache attempted allocation again");
        const auto after = portfolio.snapshot();
        same_charges(before, after);
        require(after.views[4].preparation.status == row.status, "cached view status changed");
        ++preparation_faults;
    }
    require(fact_stage_faults > 0, "sweep did not reach allocation of fresh facts after helper construction");
    same_source(untouched, source);
    std::cout << "view_allocation_points=" << count.allocations << " fact_stage_faults=" << fact_stage_faults << '\n';
}

void candidate_allocation_failure(const Ast& source) {
    context = "first candidate allocation failure and cached unavailability";
    SourceViewPortfolio portfolio(source);
    require(portfolio.prepare_view(4) != nullptr, "candidate fault requires prepared source view");
    Probe failure;
    const SourceViewCandidate* candidate = nullptr;
    { Guard guard(Mode::FailIndex, failure, 0); candidate = portfolio.prepare(chain_id); }
    require(failure.fired && !candidate, "candidate allocation fault became a certificate");
    const auto before = portfolio.snapshot();
    const auto& row = before.views[4].candidates[static_cast<unsigned>(SourceRepresentation::Predictive)];
    require(row.status == SourcePreparationStatus::AllocationFailure && row.retained_bytes == 0 &&
        before.views[4].preparation.status == SourcePreparationStatus::Ready &&
        before.candidate_attempts == 1 && before.reserved_candidate_work == 1048576,
        "failed first candidate allocation lost its separate source/candidate accounting");
    Probe cached;
    { Guard guard(Mode::Count, cached); candidate = portfolio.prepare(chain_id); }
    require(!candidate && cached.allocations == 0, "negative candidate cache allocated again");
    same_charges(before, portfolio.snapshot());
    require(portfolio.snapshot().views[4].candidates[static_cast<unsigned>(SourceRepresentation::Predictive)].status == row.status,
        "candidate failure cache status changed");
}

const SourceViewCandidate* prepare_candidate(const AstMachineProgram& program, bool lowered) {
    const auto portfolio = program.shared_source_portfolio();
    const auto* candidate = portfolio->prepare(chain_id);
    require(candidate && candidate->ready(), "optional check requires admitted arithmetic candidate");
    if (lowered) {
        const auto* code = candidate->lowered_predictive.get(candidate->source_view().ast(),
            candidate->source_view().analysis(), *candidate->predictive());
        require(code && code->ready(), "optional lowered check requires successful portable lowering");
    }
    return candidate;
}
void selected_without_fallback(const OccurrenceStatistics& stats) {
    require(stats.selected_source_view == 4 && stats.normalized_exact_attempts == 1 &&
        stats.predictive_attempts == 1 && stats.native_normalized_program_fallbacks == 0 && !stats.limit_reached,
        "optional check changed source identity or converted interruption to a fallback/resource limit");
}

void lowering_allocation_failure(const Ast& source) {
    context = "one-shot lowering failure preserves interpreter and negative lowering cache";
    AstMachineProgram program(source);
    const auto* candidate = prepare_candidate(program, false);
    require(!candidate->lowered_predictive.statistics().attempted, "lowering fault requires cold lowering cache");
    OccurrenceStatistics stats;
    Probe failure;
    const auto first = call([&] {
        Guard guard(Mode::FailLowering, failure, 0, &stats);
        return program.full_match_candidate(chain_id, "a+a;", options(), &stats);
    });
    require(failure.fired && failure.steps_at_trigger == 0 && first == CallResult::Match,
        "first lowering allocation failure escaped or changed the Boolean answer");
    selected_without_fallback(stats);
    require(stats.lowered_predictive_attempts == 0 && stats.predictive_steps > 0,
        "failed lowering did not execute its certificate interpreter");
    const auto lowering = candidate->lowered_predictive.statistics();
    require(lowering.attempted && !lowering.ready && lowering.limited && lowering.retained_bytes == 0,
        "failed lowering was not cached separately from the ready certificate");
    require(call([&] { return program.full_match_candidate(chain_id, "a+a;", options(), &stats); }) == CallResult::Match,
        "repeated failed-lowering query changed answer");
    selected_without_fallback(stats);
    const auto repeated = candidate->lowered_predictive.statistics();
    require(stats.lowered_predictive_attempts == 0 && repeated.attempted && !repeated.ready && repeated.limited &&
        repeated.compilation_steps == lowering.compilation_steps && repeated.retained_bytes == lowering.retained_bytes,
        "negative lowering cache changed on repeated query");
}

void in_flight_cancellation(const Ast& source, bool lowered) {
    context = lowered ? "lowered deterministic in-flight cancellation" : "interpreted deterministic in-flight cancellation";
    AstMachineProgram program(source);
    (void)prepare_candidate(program, lowered); // Preparation happens before arming injection.
    const std::string input = std::string(512, 'a') + ';';
    std::atomic_bool cancel{false};
    OccurrenceStatistics stats;
    Probe interruption;
    const auto result = call([&] {
        Guard guard(Mode::CancelActive, interruption, 0, &stats, &cancel);
        return program.full_match_candidate(chain_id, input, options(lowered), &stats, &cancel);
    });
    require(interruption.fired && interruption.steps_at_trigger > 0 &&
        interruption.source_view_at_trigger == 4 && cancel.load(std::memory_order_relaxed) &&
        result == CallResult::Cancelled && stats.predictive_steps >= interruption.steps_at_trigger,
        "running predictor did not propagate allocation-triggered cancellation");
    selected_without_fallback(stats);
    require(bool(stats.lowered_predictive_attempts) == lowered, "cancellation used the wrong predictive executor");
    cancel.store(false, std::memory_order_relaxed);
    require(call([&] { return program.full_match_candidate(chain_id, "a;", options(lowered), &stats, &cancel); }) == CallResult::Match,
        "cancelled query contaminated the reused immutable owner");
    selected_without_fallback(stats);
}

void runtime_allocation_failure(const Ast& source, bool lowered) {
    context = lowered ? "lowered runtime bad_alloc remains distinct" : "interpreted runtime bad_alloc remains distinct";
    AstMachineProgram program(source);
    (void)prepare_candidate(program, lowered);
    OccurrenceStatistics stats;
    Probe failure;
    const auto result = call([&] {
        Guard guard(Mode::FailActive, failure, 0, &stats);
        return program.full_match_candidate(chain_id, "a;", options(lowered), &stats);
    });
    require(failure.fired && failure.steps_at_trigger > 0 && result == CallResult::AllocationFailure,
        "runtime allocation failure became a Boolean, resource limit, or unavailable preparation");
    selected_without_fallback(stats);
    require(bool(stats.lowered_predictive_attempts) == lowered, "runtime fault used the wrong predictive executor");
    require(call([&] { return program.full_match_candidate(chain_id, "a;", options(lowered), &stats); }) == CallResult::Match,
        "runtime allocation failure contaminated subsequent recognition");
    selected_without_fallback(stats);
}
} // namespace

int main() {
    try {
        const auto source = parse(arithmetic), untouched = source;
        view_allocation_sweep(source);
        candidate_allocation_failure(source);
        lowering_allocation_failure(source);
        for (const bool lowered : {false, true}) in_flight_cancellation(source, lowered);
        for (const bool lowered : {false, true}) runtime_allocation_failure(source, lowered);
        require(recognition_calls == recognition_cap && cancelled_calls == 2 && runtime_allocation_failures == 2,
            "frozen supplemental outcome/call allocation changed");
        same_source(untouched, source);
        std::cout << "test_arithmetic_chain_faults passed checks=" << checks
            << " recognition_calls=" << recognition_calls << " preparation_faults=" << preparation_faults
            << " fact_stage_faults=" << fact_stage_faults << " cancelled=" << cancelled_calls
            << " runtime_bad_alloc=" << runtime_allocation_failures << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "test_arithmetic_chain_faults HARNESS_FAILURE recognition_calls=" << recognition_calls
            << " " << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "test_arithmetic_chain_faults HARNESS_FAILURE recognition_calls=" << recognition_calls
            << " unexpected nonstandard exception\n";
        return 1;
    }
}
