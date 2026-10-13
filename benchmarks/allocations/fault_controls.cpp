// Source-only companion to runner.cpp. Build with the same engine sources and
// -DCRANK_ALLOC_PROBE; this translation unit supplies executable-only new/delete.
// Each sweep measures its own allocation count, then tests at most 96 one-shot
// failure ordinals spread across that range, including the first and last.
#ifndef CRANK_ALLOC_PROBE
#error "fault_controls.cpp requires CRANK_ALLOC_PROBE"
#endif
#define main allocation_runner_unused_main
#include "runner.cpp"
#undef main

#include <array>
#include <optional>
#include <stdexcept>

namespace fault_controls {
using namespace crank;
using crank::detail::AstMachineProgram;
constexpr std::size_t failure_cap = 96;
std::size_t checks = 0;

void require(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}

struct Attempt {
    std::size_t calls = 0, bytes = 0, peak_extra = 0;
    bool injected = false, propagated_bad_alloc = false;
};

// Disarm before assertions/reporting or post-failure normal queries. The probe
// changes fail from zero to -1 when it throws, so an intercepted bad_alloc can
// be distinguished from an ordinal that was never reached. It can fire once.
class Guard {
public:
    explicit Guard(long long index) {
        require(!probe::enabled.load(), "nested allocation probe");
        probe::fail = index;
        probe::peak = probe::live.load();
        for (auto& counter : probe::c) {
            counter.calls = 0;
            counter.bytes = 0;
            counter.peak = counter.live.load();
        }
        probe::enabled = true;
    }
    ~Guard() noexcept { probe::enabled = false; probe::fail = -1; }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
};

template<class Action>
Attempt measure(Action&& action, long long index = -1) {
    Attempt result;
    const auto before = probe::live.load();
    {
        Guard guard(index);
        try { action(); }
        catch (const std::bad_alloc&) { result.propagated_bad_alloc = true; }
        result.injected = index >= 0 && probe::fail.load() < 0;
        result.calls = probe::c[0].calls.load();
        result.bytes = probe::c[0].bytes.load();
        result.peak_extra = probe::peak.load() - before;
    }
    return result;
}

struct Summary {
    const char* phase = "";
    std::size_t baseline_calls = 0, baseline_bytes = 0, baseline_peak_extra = 0;
    std::size_t sweep_points = 0, injected = 0, propagated_bad_alloc = 0;
    std::size_t safe_answers_after_injection = 0, unreached = 0;
    std::size_t largest_attempt_peak_extra = 0, cleanup_checks = 0;
    std::size_t scope_start_live = 0, scope_end_live = 0;
};

void baseline(Summary& summary, const Attempt& attempt) {
    require(!attempt.propagated_bad_alloc, "baseline unexpectedly exhausted allocation");
    require(attempt.calls > 0, "failure sweep has no measured allocation sites");
    summary.baseline_calls = attempt.calls;
    summary.baseline_bytes = attempt.bytes;
    summary.baseline_peak_extra = attempt.peak_extra;
    summary.sweep_points = std::min(failure_cap, attempt.calls);
}

long long ordinal(const Summary& summary, std::size_t point) {
    if (summary.sweep_points == 1) return 0;
    return static_cast<long long>(point * (summary.baseline_calls - 1) /
                                  (summary.sweep_points - 1));
}

void observe(Summary& summary, const Attempt& attempt) {
    summary.injected += attempt.injected;
    summary.propagated_bad_alloc += attempt.propagated_bad_alloc;
    summary.safe_answers_after_injection += attempt.injected && !attempt.propagated_bad_alloc;
    summary.unreached += !attempt.injected;
    summary.largest_attempt_peak_extra = std::max(summary.largest_attempt_peak_extra,
                                                attempt.peak_extra);
}

void clean(Summary& summary, std::size_t before) {
    ++summary.cleanup_checks;
    require(probe::live.load() == before, "scope retained allocation payload after destruction");
}

void print(const Summary& s) {
    std::cout << "{\"control\":\"" << s.phase << "\",\"baseline_calls\":" << s.baseline_calls
        << ",\"baseline_bytes\":" << s.baseline_bytes << ",\"baseline_peak_extra\":" << s.baseline_peak_extra
        << ",\"sweep_points\":" << s.sweep_points << ",\"injected\":" << s.injected
        << ",\"propagated_bad_alloc\":" << s.propagated_bad_alloc
        << ",\"safe_answers_after_injection\":" << s.safe_answers_after_injection
        << ",\"unreached\":" << s.unreached
        << ",\"largest_attempt_peak_extra\":" << s.largest_attempt_peak_extra
        << ",\"cleanup_checks\":" << s.cleanup_checks << ",\"scope_start_live\":" << s.scope_start_live
        << ",\"scope_end_live\":" << s.scope_end_live << "}\n";
}

Summary equivalence() {
    Summary summary;
    summary.phase = "equivalence_preparation";
    summary.scope_start_live = probe::live.load();
    {
        const auto ast = parse(R"((?<S>[a-c](?&S)|)(?<T>[a-c](?&T)|))");
        const auto control = detail::pure_control_targets(ast);
        std::vector<NodeId> expected;
        detail::EquivalenceStatistics stats;
        baseline(summary, measure([&] {
            expected = detail::pure_ast_equivalent_targets(ast, control, &stats);
        }));
        require(stats.eligible && !stats.limit_reached && stats.merged_nodes > 0,
                "equivalence fixture did not prepare a nontrivial quotient");
        for (std::size_t point = 0; point < summary.sweep_points; ++point) {
            const auto before = probe::live.load();
            {
                std::vector<NodeId> got;
                detail::EquivalenceStatistics failed_stats;
                const auto attempt = measure([&] {
                    got = detail::pure_ast_equivalent_targets(ast, control, &failed_stats);
                }, ordinal(summary, point));
                observe(summary, attempt);
                if (!attempt.propagated_bad_alloc)
                    require(got == expected || got == control,
                            "equivalence fault published a partial or changed quotient");
            }
            clean(summary, before);
            {
                const auto retry = detail::pure_ast_equivalent_targets(ast, control);
                require(retry == expected, "equivalence fault contaminated subsequent preparation");
            }
            clean(summary, before);
        }
        require(summary.injected > 0, "equivalence sweep did not inject a failure");
    }
    summary.scope_end_live = probe::live.load();
    clean(summary, summary.scope_start_live);
    return summary;
}

void verify_regular(const RegularProgram& program) {
    require(program.full_match(""), "regular owner lost empty membership");
    require(program.full_match("abacba"), "regular owner lost accepted membership");
    require(!program.full_match("abacbax"), "regular owner changed rejected membership");
    require(program.endpoints("abacbax") == std::vector<std::size_t>({0, 2, 4, 6}),
            "regular owner changed exported endpoints");
}

Summary regular() {
    Summary summary;
    summary.phase = "regular_construction";
    summary.scope_start_live = probe::live.load();
    {
        const auto ast = parse(R"((?:ab|ac|ba)*)");
        const RegularProgram retained(ast);
        verify_regular(retained);
        const auto before_baseline = probe::live.load();
        {
            std::optional<RegularProgram> counted;
            baseline(summary, measure([&] { counted.emplace(ast); }));
            require(counted.has_value(), "regular baseline failed to create an owner");
            verify_regular(*counted);
        }
        clean(summary, before_baseline);
        for (std::size_t point = 0; point < summary.sweep_points; ++point) {
            const auto before = probe::live.load();
            {
                std::optional<RegularProgram> candidate;
                const auto attempt = measure([&] { candidate.emplace(ast); }, ordinal(summary, point));
                observe(summary, attempt);
                if (attempt.propagated_bad_alloc)
                    require(!candidate, "failed regular construction published a program");
                else {
                    require(candidate.has_value(), "safe regular construction lost its owner");
                    verify_regular(*candidate);
                }
            }
            clean(summary, before);
            verify_regular(retained); // Previously owned DFA must remain usable.
            {
                const RegularProgram retry(ast); // Same source, after failed construction/destruction.
                verify_regular(retry);
            }
            clean(summary, before);
        }
        require(summary.injected > 0, "regular sweep did not inject a failure");
    }
    summary.scope_end_live = probe::live.load();
    clean(summary, summary.scope_start_live);
    return summary;
}

OccurrenceMatchOptions ordinary_options() {
    OccurrenceMatchOptions o;
    o.predictive_recursion = o.bounded_word_prediction = false;
    o.deterministic_programs = o.compiled_exact_programs = o.native_slr = false;
    o.normalized_exact_programs = o.normalize_arithmetic_chains = false;
    o.exact_end_spines = o.predictive_exact_rims = false;
    o.regional_execution = o.regional_constraints = o.regional_consumers = false;
    o.local_regular_regions = false;
    // Retain all five root results and ordinary terminal/epsilon requests. This
    // exercises inline-first spill, rather than allowing closed-result reuse.
    o.compact_closed_results = o.inline_terminals = o.inline_nullable_frontiers = false;
    return o;
}

enum class Query { Membership, Endpoints, Witness };
struct Answer {
    bool membership = false;
    std::vector<std::size_t> endpoints;
    std::optional<Outcome> witness;
};

void query(const AstMachineProgram& program, Query kind, Answer& result,
           OccurrenceMatchOptions options, OccurrenceStatistics& stats) {
    switch (kind) {
    case Query::Membership: result.membership = program.full_match("aaaa", options, &stats); break;
    case Query::Endpoints: result.endpoints = program.endpoints("aaaa", 0, options, &stats); break;
    case Query::Witness: result.witness = program.first_match("aaaa", options, &stats); break;
    }
}

void verify_answer(Query kind, const Answer& answer, const Outcome& witness) {
    switch (kind) {
    case Query::Membership: require(answer.membership, "native allocation fault changed membership"); break;
    case Query::Endpoints:
        require(answer.endpoints == std::vector<std::size_t>({0, 1, 2, 3, 4}),
                "native allocation fault changed endpoint enumeration");
        break;
    case Query::Witness:
        require(answer.witness && *answer.witness == witness,
                "native allocation fault changed deterministic witness/captures");
        break;
    }
}

void verify_native(const AstMachineProgram& program, OccurrenceMatchOptions options,
                   const Outcome& witness) {
    require(program.full_match("aaaa", options), "reused native owner lost accepted membership");
    require(!program.full_match("aaaab", options), "reused native owner changed rejected membership");
    require(program.endpoints("aaaa", 0, options) == std::vector<std::size_t>({0, 1, 2, 3, 4}),
            "reused native owner changed endpoints after failure");
    const auto result = program.first_match("aaaa", options);
    require(result && *result == witness, "reused native owner changed witness after failure");
}

struct NativeReport {
    std::array<Summary, 3> sweeps;
    std::size_t owner_start_live = 0, owner_end_live = 0;
    std::size_t constructed_owner_extra = 0, warm_cache_extra = 0;
};

NativeReport native() {
    NativeReport report;
    report.owner_start_live = probe::live.load();
    {
        const auto ast = parse(R"((?<S>a(?&S)|))");
        const auto before_owner = probe::live.load();
        const AstMachineProgram program(ast);
        const auto constructed = probe::live.load();
        report.constructed_owner_extra = constructed - before_owner;
        const auto options = ordinary_options();
        {
            const auto warm_witness = program.first_match("aaaa", options);
            require(warm_witness.has_value(), "native fixture warmup did not produce a witness");
            verify_native(program, options, *warm_witness);
        } // All warmup result containers are destroyed before sampling cache bytes.
        const auto cache_warmed = probe::live.load();
        report.warm_cache_extra = cache_warmed - constructed;
        const auto expected = program.first_match("aaaa", options);
        require(expected && expected->end == 4 && !expected->captures.empty() &&
                expected->captures[0] == std::optional<Span>(Span{0, 4}),
                "native fixture did not produce a valid full witness");
        verify_native(program, options, *expected);
        const auto warmed = probe::live.load();

        for (std::size_t kind_index = 0; kind_index < report.sweeps.size(); ++kind_index) {
            const auto kind = static_cast<Query>(kind_index);
            auto& summary = report.sweeps[kind_index];
            summary.phase = kind == Query::Membership ? "native_membership" :
                            kind == Query::Endpoints ? "native_endpoints_spill" : "native_witness";
            summary.scope_start_live = warmed;
            {
                Answer answer;
                OccurrenceStatistics stats;
                baseline(summary, measure([&] { query(program, kind, answer, options, stats); }));
                verify_answer(kind, answer, *expected);
                require(stats.memo_entries > 0 && stats.predictive_attempts == 0 &&
                        stats.bounded_word_attempts == 0 && stats.exact_program_attempts == 0 &&
                        stats.normalized_exact_attempts == 0 && stats.regional_attempts == 0 &&
                        stats.regular_region_transitions == 0,
                        "native fixture bypassed the ordinary request graph");
                if (kind == Query::Endpoints)
                    require(stats.native_result_slots >= 5, "native fixture did not retain multiple results");
            }
            clean(summary, warmed);
            for (std::size_t point = 0; point < summary.sweep_points; ++point) {
                {
                    Answer answer;
                    OccurrenceStatistics stats;
                    const auto attempt = measure([&] { query(program, kind, answer, options, stats); },
                                                 ordinal(summary, point));
                    observe(summary, attempt);
                    if (!attempt.propagated_bad_alloc) verify_answer(kind, answer, *expected);
                }
                clean(summary, warmed);
                verify_native(program, options, *expected);
                clean(summary, warmed);
            }
            require(summary.injected > 0, "native sweep did not inject a failure");
            summary.scope_end_live = probe::live.load();
        }
    }
    report.owner_end_live = probe::live.load();
    require(report.owner_end_live == report.owner_start_live,
            "native owner/caches/witness retained payload after final destruction");
    return report;
}
} // namespace fault_controls

int main() {
    probe::foreground = true;
    probe::enabled = false;
    probe::fail = -1;
    try {
        // Initialize stream formatting before the first strict live-byte scope.
        std::cout << "{\"control\":\"allocation_failure_controls\",\"max_points_per_sweep\":96}\n";
        fault_controls::print(fault_controls::equivalence());
        fault_controls::print(fault_controls::regular());
        const auto native = fault_controls::native();
        for (const auto& sweep : native.sweeps) fault_controls::print(sweep);
        std::cout << "{\"control\":\"native_owner_lifetime\",\"constructed_owner_extra\":"
            << native.constructed_owner_extra << ",\"warm_cache_extra\":" << native.warm_cache_extra
            << ",\"owner_start_live\":" << native.owner_start_live
            << ",\"owner_end_live\":" << native.owner_end_live << "}\n";
        std::cout << "{\"status\":\"passed\",\"checks\":" << fault_controls::checks << "}\n";
        return 0;
    } catch (const std::exception& error) {
        probe::enabled = false;
        probe::fail = -1;
        std::cerr << "allocation_failure_controls HARNESS_FAILURE checks=" << fault_controls::checks
            << " " << error.what() << '\n';
        return 1;
    } catch (...) {
        probe::enabled = false;
        probe::fail = -1;
        std::cerr << "allocation_failure_controls HARNESS_FAILURE unexpected exception\n";
        return 1;
    }
}
