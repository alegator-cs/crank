#include "ast_machine.hpp"
#include "tests/helpers/relation_reference.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
#if defined(_WIN32)
#include <malloc.h>
#endif

// Probe-only process-wide replacements. Hooks are thread-local and armed only
// around one synchronous native query. No logging/assertion allocation occurs
// while armed. These are not installed in libcrank or any engine source.
namespace allocation_gate {
enum class Mode { Off, Count, FailOnce, CancelPredictor };
struct State {
    Mode mode = Mode::Off;
    std::size_t calls = 0, target = 0;
    bool fired = false;
    crank::OccurrenceStatistics* stats = nullptr;
    std::atomic_bool* cancel = nullptr;
    std::size_t observed_work = 0, observed_predictive_steps = 0, observed_predictors = 0;
};
thread_local State state;
void before_allocation() {
    if (state.mode == Mode::Off) return;
    const auto ordinal = state.calls++;
    if (state.mode == Mode::FailOnce && ordinal == state.target) {
        state.fired = true;
        state.mode = Mode::Off; // One failure, including constructor cleanup.
        throw std::bad_alloc{};
    }
    if (state.mode == Mode::CancelPredictor && state.stats && state.cancel &&
        state.stats->regional_predictive_attempts > 0 && state.stats->predictive_steps >= 4) {
        state.observed_work = state.stats->work_steps;
        state.observed_predictive_steps = state.stats->predictive_steps;
        state.observed_predictors = state.stats->regional_predictive_attempts;
        state.fired = true;
        state.mode = Mode::Off;
        state.cancel->store(true, std::memory_order_relaxed);
    }
}
void arm(Mode mode, std::size_t target = 0, crank::OccurrenceStatistics* stats = nullptr,
         std::atomic_bool* cancel = nullptr) {
    state = {}; state.mode = mode; state.target = target; state.stats = stats; state.cancel = cancel;
}
State disarm() noexcept { const auto result = state; state.mode = Mode::Off; return result; }
void* allocate(std::size_t n) {
    before_allocation();
    if (auto* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc{};
}
void* allocate_aligned(std::size_t n, std::size_t alignment) {
    before_allocation();
    if (!n) n = 1;
#if defined(_WIN32)
    if (auto* p = _aligned_malloc(n, alignment)) return p;
#else
    if (n > std::numeric_limits<std::size_t>::max() - (alignment - 1)) throw std::bad_alloc{};
    n = (n + alignment - 1) / alignment * alignment;
    if (auto* p = std::aligned_alloc(alignment, n)) return p;
#endif
    throw std::bad_alloc{};
}
void free_aligned(void* p) noexcept {
#if defined(_WIN32)
    _aligned_free(p);
#else
    std::free(p);
#endif
}
} // namespace allocation_gate
void* operator new(std::size_t n) { return allocation_gate::allocate(n); }
void* operator new[](std::size_t n) { return allocation_gate::allocate(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { try { return allocation_gate::allocate(n); } catch (...) { return nullptr; } }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { try { return allocation_gate::allocate(n); } catch (...) { return nullptr; } }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
void* operator new(std::size_t n, std::align_val_t a) { return allocation_gate::allocate_aligned(n, std::size_t(a)); }
void* operator new[](std::size_t n, std::align_val_t a) { return allocation_gate::allocate_aligned(n, std::size_t(a)); }
void operator delete(void* p, std::align_val_t) noexcept { allocation_gate::free_aligned(p); }
void operator delete[](void* p, std::align_val_t) noexcept { allocation_gate::free_aligned(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { allocation_gate::free_aligned(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { allocation_gate::free_aligned(p); }
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    try { return allocation_gate::allocate_aligned(n, std::size_t(a)); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    try { return allocation_gate::allocate_aligned(n, std::size_t(a)); } catch (...) { return nullptr; }
}
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { allocation_gate::free_aligned(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { allocation_gate::free_aligned(p); }

using namespace crank;
namespace d = crank::detail;
namespace ref = certified_reference;
namespace {
std::string context;
std::size_t checks = 0, queries = 0, oracle_queries = 0;
std::size_t prep_fault_cases = 0, excluded_allocation_faults = 0, fault_ordinals = 0;
std::size_t predictor_rejections = 0, precancellations = 0, in_flight_cancellations = 0;
std::size_t quota_queries = 0, quota_fallbacks = 0;
void require(bool condition, const char* why) {
    ++checks;
    if (!condition) throw std::runtime_error(context + ": " + why);
}
OccurrenceMatchOptions options(bool regional = true) {
    OccurrenceMatchOptions value;
    value.bounded_word_prediction = value.deterministic_programs = false;
    value.normalized_exact_programs = false;
    value.exact_end_spines = value.predictive_recursion = value.predictive_exact_rims = false;
    value.regional_execution = regional;
    value.regional_constraints = false;
    value.local_regular_regions = false;
    // Public control_targets() is exactly the execution map for this probe;
    // structural equivalence does not add an unexposed private representative.
    value.merge_equivalent_nodes = false;
    value.max_states = 250'000; value.max_steps = 2'000'000;
    return value;
}

struct Fixture { Ast ast; NodeId producer, body; };
Fixture fixture() {
    // Independent AST syntax: S -> a S b S | x, root -> S ';'. Two recursive
    // children exclude exact-end spine preparation. A short rejecting word
    // with unchanged length/outer FIRST/LAST forces the predictor to execute.
    Ast ast;
    // Calls must target root or Capture in a valid production AST. Keep the
    // capture slot/name/map consistent; Boolean control collapse removes the
    // wrapper while witness queries retain its observable capture identity.
    const auto s = ref::add(ast, Kind::Capture);
    const auto body = ref::add(ast, Kind::Alt);
    ast.nodes[s].children = {body};
    ast.nodes[s].capture_id = 1;
    ast.capture_names.push_back("S");
    ast.capture_nodes.push_back(s);
    const auto a = ref::byte(ast, 'a'), b = ref::byte(ast, 'b'), x = ref::byte(ast, 'x');
    const auto recursive = ref::add(ast, Kind::Concat,
        {a, ref::call(ast, s), b, ref::call(ast, s)});
    ast.nodes[body].children = {recursive, x};
    const auto separator = ref::byte(ast, ';');
    ast.root = ref::add(ast, Kind::Concat, {ref::call(ast, s), separator});
    ast.capture_nodes[0] = ast.root;
    return {std::move(ast), s, body};
}
std::string accepted(unsigned depth) {
    std::string value(depth, 'a'); value += 'x';
    for (unsigned i = 0; i < depth; ++i) value += "bx";
    value += ';'; return value;
}

enum class Status { Match, NoMatch, ResourceLimit, Cancelled, BadAllocation };
const char* label(Status status) {
    switch (status) {
    case Status::Match: return "Match";
    case Status::NoMatch: return "NoMatch";
    case Status::ResourceLimit: return "ResourceLimit";
    case Status::Cancelled: return "Cancelled";
    case Status::BadAllocation: return "BadAllocation";
    }
    return "invalid";
}
Status query(const d::AstMachineProgram& program, const std::string& input,
             OccurrenceMatchOptions config, OccurrenceStatistics& stats,
             const std::atomic_bool* cancel = nullptr) {
    ++queries;
    try {
        return program.full_match_node(program.ast().root, input, config, &stats, cancel)
            ? Status::Match : Status::NoMatch;
    } catch (const ResourceLimit&) { return Status::ResourceLimit; }
    catch (const d::AstExecutionCancelled&) { return Status::Cancelled; }
    catch (const std::bad_alloc&) { return Status::BadAllocation; }
}
void output(const char* name, Status status, const OccurrenceStatistics& stats) {
    std::cout << "CASE name=" << name << " status=" << label(status)
        << " work=" << stats.work_steps << " states=" << stats.states
        << " requests=" << stats.memo_entries << " down=" << stats.down_steps
        << " regional_attempts=" << stats.regional_attempts << " predictor_attempts=" << stats.regional_predictive_attempts
        << " predictor_steps=" << stats.predictive_steps << " regional_failures=" << stats.regional_failures
        << " preparation_declines=" << stats.regional_declines << " regional_fallbacks=" << stats.native_regional_fallbacks
        << " discarded_work=" << stats.native_acceleration_discarded_steps
        << " regional_discarded_subset=" << stats.regional_discarded_steps << " limit=" << stats.limit_reached << '\n';
}

void observers(const d::AstMachineProgram& program, const std::vector<std::string>& words) {
    for (const auto& input : words) {
        context = "language and observers input=" + ref::escaped(input);
        require(input.size() <= 32, "oracle input exceeds explicit bound");
        const auto relation = ref::language(program.ast(), input); ++oracle_queries;
        const bool truth = relation.nodes[program.ast().root].at(0, input.size());
        OccurrenceStatistics on_stats, off_stats;
        const auto on = query(program, input, options(), on_stats);
        const auto off = query(program, input, options(false), off_stats);
        require(on == (truth ? Status::Match : Status::NoMatch) && on == off, "regional switch changed finite language/status");
        require(!on_stats.limit_reached && !off_stats.limit_reached, "completed Boolean retains limit flag");
        require(!off_stats.regional_attempts && !on_stats.proof_nodes, "route/observer accounting changed");
        for (std::size_t begin = 0; begin <= input.size(); ++begin) {
            std::vector<std::size_t> expected;
            for (std::size_t end = begin; end <= input.size(); ++end)
                if (relation.nodes[program.ast().root].at(begin, end)) expected.push_back(end);
            require(program.endpoints(input, begin, options(), &on_stats) == expected &&
                    program.endpoints(input, begin, options(false), &off_stats) == expected, "endpoint relation changed");
            queries += 2;
            require(!on_stats.regional_attempts && !off_stats.regional_attempts, "Boolean regional permission entered endpoints");
        }
        require(program.first_match(input, options(), &on_stats) == program.first_match(input, options(false), &off_stats),
            "first-witness observation changed");
        queries += 2;
        require(!on_stats.regional_attempts && !off_stats.regional_attempts, "Boolean regional permission entered witness query");
        const auto source = program.ast().capture_nodes.at(1);
        require(program.full_match_node(source, input, options(), &on_stats) == relation.nodes[source].at(0, input.size()) &&
            program.full_match_node(source, input, options(false), &off_stats) == relation.nodes[source].at(0, input.size()),
            "named source capture full-span observer changed");
        queries += 2;
        require(!on_stats.regional_attempts && !off_stats.regional_attempts, "root consumer permission entered source capture query");
    }
}

void executed_rejection(const Fixture& f) {
    context = "admitted predictor decides NoMatch without a second producer";
    const d::AstMachineProgram program(f.ast);
    require(program.control_targets().at(f.producer) == f.body &&
        program.control_targets().at(f.ast.root) == f.ast.root,
        "valid Capture wrapper or root did not have the expected transparent Boolean control map");
    const std::string input = "axcx;";
    require(!ref::language(f.ast, input).nodes[f.ast.root].at(0, input.size()), "rejection fixture belongs to language");
    ++oracle_queries;
    auto on_options = options(); on_options.prune = false;
    auto off_options = on_options; off_options.regional_execution = false;
    OccurrenceStatistics on, off;
    require(query(program, input, on_options, on) == Status::NoMatch &&
        query(program, input, off_options, off) == Status::NoMatch, "executed rejection changed status");
    require(on.regional_predictive_attempts == 1 && on.regional_attempts == 1 &&
        on.regional_certificate_uses == 1 && on.predictive_steps > 0, "rejecting predictor was not actually admitted/executed once");
    require(on.regional_failures == 1 && !on.regional_successes && !on.native_regional_fallbacks &&
        !on.limit_reached, "predictor NoMatch confused with preparation decline or resource fallback");
    // Calls and the valid Capture wrapper collapse to body before request
    // interning (merge_equivalent_nodes=false selects the exposed map). This
    // root has exactly one producer request. The two down events are root and
    // body; no ordinary interior or second producer follows admitted failure.
    require(on.down_steps == 2 && on.memo_entries == 2 && off.down_steps > on.down_steps && off.memo_entries > on.memo_entries,
        "admitted rejection expanded ordinary interior or another producer");
    ++predictor_rejections; output("executed_admitted_rejection", Status::NoMatch, on);
}

void preparation_faults(const Fixture& f) {
    context = "native lazy preparation allocation decline falls through to ordinary execution";
    const auto input = accepted(3);
    const d::AstMachineProgram baseline(f.ast);
    OccurrenceStatistics baseline_stats;
    allocation_gate::arm(allocation_gate::Mode::Count);
    const auto baseline_status = query(baseline, input, options(), baseline_stats);
    const auto trace = allocation_gate::disarm();
    require(baseline_status == Status::Match && baseline_stats.regional_predictive_attempts > 0,
        "allocation calibration did not use native predictor");
    const auto baseline_preparation = baseline.regional_preparation_statistics();
    require(!baseline_preparation.limit_declines && !baseline_preparation.allocation_declines,
        "calibration already had preparation resource failures");
    const auto count = std::min<std::size_t>(trace.calls, 192);
    std::unique_ptr<d::AstMachineProgram> selected;
    std::size_t selected_ordinal = 0;
    for (std::size_t ordinal = 0; ordinal < count && prep_fault_cases < 3; ++ordinal) {
        auto program = std::make_unique<d::AstMachineProgram>(f.ast);
        OccurrenceStatistics stats;
        allocation_gate::arm(allocation_gate::Mode::FailOnce, ordinal);
        const auto status = query(*program, input, options(), stats);
        const auto fault = allocation_gate::disarm();
        ++fault_ordinals;
        require(fault.fired, "calibrated allocation ordinal was not encountered");
        const auto prep = program->regional_preparation_statistics();
        if (status == Status::BadAllocation) { ++excluded_allocation_faults; continue; }
        require(status == Status::Match && !stats.limit_reached,
            "one-shot allocation fault became NoMatch, native ResourceLimit, or cancellation");
        if (!(prep.limit_declines || prep.allocation_declines) || !stats.regional_declines) continue;
        require(stats.down_steps > baseline_stats.down_steps, "caught preparation fault did not resume ordinary producer work");
        require(!stats.native_regional_fallbacks, "preparation unavailability was mislabeled as execution-budget retry");
        ++prep_fault_cases;
        std::cout << "PREPARATION_FAULT ordinal=" << ordinal << " query_status=" << label(status)
            << " runtime_declines=" << stats.regional_declines << " builds=" << prep.predictive_builds
            << " work=" << prep.analysis_steps << " limit_declines=" << prep.limit_declines
            << " allocation_declines=" << prep.allocation_declines << '\n';
        if (!selected) { selected_ordinal = ordinal; selected = std::move(program); }
    }
    require(prep_fault_cases > 0 && selected, "bounded native sweep found no caught preparation-decline/fallback evidence");
    // Fault is disarmed. A permanently unavailable source preparation must
    // continue to recognize both statuses and preserve all observed relations.
    observers(*selected, {"x;", "axbx;", accepted(3), "axcx;", "x", ""});
    std::cout << "PREPARATION_SWEEP allocation_calls=" << trace.calls << " ordinal_cap=192 tested=" << fault_ordinals
        << " classified=" << prep_fault_cases << " excluded_propagated_bad_alloc=" << excluded_allocation_faults
        << " observer_program_ordinal=" << selected_ordinal << '\n';
}

void resource_and_cancellation(const Fixture& f) {
    context = "native exhaustion and precancellation are separate statuses";
    const d::AstMachineProgram program(f.ast);
    const auto short_input = accepted(3);
    for (const bool regional : {false, true}) {
        auto config = options(regional); config.max_states = config.max_steps = 0;
        OccurrenceStatistics stats;
        const auto status = query(program, short_input, config, stats);
        require(status == Status::ResourceLimit && stats.limit_reached, "forced native cap returned NoMatch or wrong exception");
        output(regional ? "zero_cap_regional_on" : "zero_cap_regional_off", status, stats);
        std::atomic_bool cancel{true};
        const auto cancelled = query(program, short_input, options(regional), stats, &cancel);
        require(cancelled == Status::Cancelled && !stats.limit_reached && stats.work_steps == 0,
            "precancelled query became Boolean, ResourceLimit, or nonzero work");
        ++precancellations;
        output(regional ? "precancel_regional_on" : "precancel_regional_off", cancelled, stats);
    }

    context = "controlled in-flight cancellation during admitted predictor stack growth";
    const auto long_input = accepted(96); // 290 bytes; no dense oracle here.
    OccurrenceStatistics warm;
    require(query(program, long_input, options(), warm) == Status::Match &&
        query(program, long_input, options(false), warm) == Status::Match, "in-flight fixture failed unchanged native language controls");
    auto config = options(); config.predictive_continuation_runs = false;
    OccurrenceStatistics stats;
    std::atomic_bool cancel{false};
    allocation_gate::arm(allocation_gate::Mode::CancelPredictor, 0, &stats, &cancel);
    const auto status = query(program, long_input, config, stats, &cancel);
    const auto hook = allocation_gate::disarm();
    require(hook.fired && hook.observed_predictors > 0 && hook.observed_predictive_steps >= 4 && hook.observed_work > 0,
        "cancellation hook never observed admitted in-flight predictor work");
    require(status == Status::Cancelled && !stats.limit_reached && cancel.load(),
        "controlled in-flight cancellation became Boolean, ResourceLimit, or another exception");
    ++in_flight_cancellations;
    output("controlled_in_flight_cancel", status, stats);
    std::cout << "CANCEL_HOOK observed_predictors=" << hook.observed_predictors
        << " observed_predictive_steps=" << hook.observed_predictive_steps << " observed_work=" << hook.observed_work
        << " same_thread=1 sleeps=0\n";
}

void budget_fallback(const Fixture& f) {
    context = "bounded genuine native budget fallback";
    const d::AstMachineProgram program(f.ast);
    const std::vector<std::string> words{accepted(0), accepted(3), "axcx;", "aaaxbxbcxbx;"};
    std::size_t completed = 0, exhausted = 0;
    for (const auto& input : words) {
        require(input.size() <= 32, "quota oracle input exceeds bound");
        const bool truth = ref::language(f.ast, input).nodes[f.ast.root].at(0, input.size()); ++oracle_queries;
        for (const std::size_t state_cap : {0U, 1U, 2U, 4U, 8U, 16U, 32U, 64U, 128U, 512U})
            for (const std::size_t work_cap : {0U, 1U, 2U, 4U, 8U, 16U, 32U, 64U, 128U, 512U}) {
            auto on_config = options(); on_config.max_states = state_cap; on_config.max_steps = work_cap;
            auto off_config = on_config; off_config.regional_execution = false;
            OccurrenceStatistics on, off;
            const auto off_status = query(program, input, off_config, off);
            const auto on_status = query(program, input, on_config, on);
            quota_queries += 2;
            require(off_status != Status::BadAllocation && off_status != Status::Cancelled &&
                on_status != Status::BadAllocation && on_status != Status::Cancelled, "quota changed exception class");
            if (on_status == Status::ResourceLimit) {
                ++exhausted;
                require(on.limit_reached && off_status == Status::ResourceLimit, "optional regional tier stole successful baseline budget");
            } else {
                ++completed;
                require(on_status == (truth ? Status::Match : Status::NoMatch) && !on.limit_reached, "quota produced wrong completed status");
            }
            if (off_status != Status::ResourceLimit)
                require(off_status == (truth ? Status::Match : Status::NoMatch), "regional-off quota produced wrong status");
            require(on.native_regional_fallbacks <= 1 && on.regional_discarded_steps <= on.native_acceleration_discarded_steps,
                "retry/accounting contract changed");
            if (on.native_regional_fallbacks) {
                require(on.regional_discarded_steps > 0 && on.native_acceleration_discarded_steps > 0,
                    "regional execution retry omitted discarded work");
                ++quota_fallbacks;
                if (quota_fallbacks <= 4) output("genuine_budget_regional_retry", on_status, on);
            }
        }
    }
    std::cout << "QUOTA queries=" << quota_queries << " completed_on=" << completed << " limited_on=" << exhausted
        << " observed_genuine_regional_retries=" << quota_fallbacks
        << " successful_retry_not_required_by_grid=1\n";
    // The grid can fail to expose a successful retry at this particular size.
    // Its nonobservance is reported, not converted into an invented guarantee.
}
} // namespace

int main() {
    try {
        std::cout << "CONTRACT requires_authorized_slot=1 engine_edits=0 gpu=0 timing=0 allocation_hooks=probe_only\n";
        const auto f = fixture();
        require(d::AstMachineProgram::eligible(f.ast), "manual named-capture fixture violates production pure AST validation");
        const d::AstMachineProgram ordinary(f.ast);
        observers(ordinary, {"x;", "axbx;", accepted(3), "axcx;", "x", ""});
        executed_rejection(f);
        preparation_faults(f);
        resource_and_cancellation(f);
        budget_fallback(f);
        std::cout << "{\"status\":\"passed\",\"checks\":" << checks << ",\"queries\":" << queries
            << ",\"oracle_queries\":" << oracle_queries << ",\"classified_native_preparation_faults\":" << prep_fault_cases
            << ",\"excluded_propagated_bad_alloc\":" << excluded_allocation_faults << ",\"fault_ordinals\":" << fault_ordinals
            << ",\"executed_predictor_rejections\":" << predictor_rejections << ",\"precancellations\":" << precancellations
            << ",\"controlled_in_flight_cancellations\":" << in_flight_cancellations
            << ",\"quota_queries\":" << quota_queries << ",\"observed_genuine_regional_retries\":" << quota_fallbacks << "}\n";
    } catch (const std::exception& error) {
        allocation_gate::disarm();
        std::cerr << "SLOT EXECUTION PROBE FAILURE: " << error.what() << '\n'; return 1;
    }
}
