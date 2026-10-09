#include "assisted_matcher.hpp"
#include "private_culler.hpp"

#include <atomic>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
std::size_t checks = 0;
void require(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}

// try_lock admission may legitimately lose to a starting/retiring idle worker.
// Rejection of optional work is part of the nonblocking API, even after wait_idle.
// Tests assert the attempt and its invariants, not a scheduler-dependent launch.
bool admission_attempted(const crank::AssistedStatistics& stats, std::size_t lanes = 0) {
    if (stats.decision == crank::AssistanceDecision::Busy)
        return !stats.helpers_admitted && !stats.copied_input_bytes;
    return stats.decision == crank::AssistanceDecision::Admitted &&
        stats.helpers_admitted && (!lanes || stats.helpers_admitted == lanes);
}

crank::AssistanceOptions options(crank::AssistancePolicy policy) {
    crank::AssistanceOptions result;
    result.policy = policy;
    result.workers = 4;
    result.min_input_bytes = 0;
    return result;
}

void static_capabilities() {
    require(crank::AssistanceOptions{}.policy == crank::AssistancePolicy::Progress,
            "default admission observes current native progress, not query history");
    for (const auto* pattern : {"[\\x00-\\xff]*", "[\\x00-\\xff][\\x00-\\xff]",
                               "(?<S>[\\x00-\\xff](?&S)|)"}) {
        const auto ast = crank::parse(pattern);
        const crank::OccurrenceAnalysis analysis(ast);
        const crank::detail::PrivateCuller culler(ast, analysis);
        require(culler.available(), "broad grammar has a compiled culler");
        require(!culler.can_reject_bytes(), "universal byte/pair graph cannot reject byte scans");
        require(!culler.can_exclude_nodes(), "universal productive leaves yield no absence facts");
        for (const std::string& input : {std::string("x"), std::string("ab"), std::string(1, '\0')}) {
            std::atomic_bool cancel{false};
            crank::detail::AtomicExclusions exclusions(ast.nodes.size());
            crank::detail::PrivateCuller::Stats stats;
            culler.scan_presence(input, cancel, exclusions, {}, stats);
            require(stats.completed && !stats.declined, "incapable proof scan completes");
            require(!exclusions.root_rejected(), "capability false produces no root fact");
            for (crank::NodeId id = 0; id != ast.nodes.size(); ++id)
                require(!exclusions.test(id), "capability false produces no node fact");
        }
    }
    {
        const auto ast = crank::parse("(?<U>(?&U))|[\\x00-\\xff]*");
        const crank::OccurrenceAnalysis analysis(ast);
        const crank::detail::PrivateCuller culler(ast, analysis);
        require(culler.can_exclude_nodes(), "unproductive cycle still supports a node exclusion");
    }
    {
        const auto ast = crank::parse("[\\x00-\\xff]*|abc");
        const crank::OccurrenceAnalysis analysis(ast);
        const crank::detail::PrivateCuller culler(ast, analysis);
        require(!culler.can_reject_bytes() && culler.can_exclude_nodes(),
                "unrestricted whole language still permits absent branch pruning");
        crank::AssistedProgram selective(ast, options(crank::AssistancePolicy::Selective));
        crank::AssistedStatistics stats;
        require(selective.full_match(std::string(128, 'q'), {}, &stats), "broad alternative accepts");
        require(admission_attempted(stats, 1), "an admitted presence-only query uses exactly one helper");
        selective.wait_idle();
    }
    {
        crank::AssistedProgram selective(crank::parse("[\\x00-\\xff]*"), options(crank::AssistancePolicy::Selective));
        crank::AssistedStatistics stats;
        require(selective.full_match("any bytes", {}, &stats), "universal root accepts");
        require(stats.decision == crank::AssistanceDecision::NoUsefulProof,
                "statically useless assistance is skipped");
        require(stats.helpers_admitted == 0 && stats.copied_input_bytes == 0,
                "static gate wakes no worker and copies no subject");
        require(selective.pool_statistics().queries_no_useful_proof == 1,
                "static decline is visible in pool counters");
        // The capability theorem requires a nonempty subject: universal leaves
        // are absent from epsilon, even if their enclosing star accepts it.
        require(selective.full_match("", {}, &stats), "epsilon remains correct");
        require(stats.decision != crank::AssistanceDecision::NoUsefulProof,
                "nonempty capability theorem is not reused for epsilon");
        selective.wait_idle();
    }
}

void bounded_backoff() {
    const auto ast = crank::parse("[ab]*");
    auto selected = options(crank::AssistancePolicy::Selective);
    crank::AssistedProgram program(ast, selected);
    const auto copy = program;
    std::size_t admitted = 0, skipped = 0, run = 0, longest = 0;
    unsigned misses = 0, expected_skip = 0;
    for (unsigned query = 0; query != 260; ++query) {
        crank::AssistedStatistics stats;
        require((query % 2 ? copy : program).full_match(std::string(128, 'a'), {}, &stats),
                "learning and copied handles preserve the accepted language");
        program.wait_idle();
        require(!stats.consumption.culled_requests && !stats.consumption.culled_tasks &&
                !stats.consumption.root_rejections, "accepted all-live grammar yields no exclusion");
        if (expected_skip) {
            require(stats.decision == crank::AssistanceDecision::PolicyBackoff,
                    "each learned countdown step skips exactly one eligible query");
            --expected_skip;
            ++skipped;
            longest = std::max(longest, ++run);
        } else {
            require(admission_attempted(stats),
                    "an expired countdown attempts admission without waiting");
            if (stats.decision == crank::AssistanceDecision::Admitted) {
                ++admitted;
                misses = std::min(6U, misses + 1);
                expected_skip = (1U << misses) - 1;
                run = 0;
            }
            // Busy changes neither learned state nor native recognition.
        }
    }
    require(admitted < 16, "ineffective queries cannot exceed the bounded probe rate");
    require(longest <= 63 && (admitted < 7 || longest == 63),
            "backoff is capped at 63 and reaches the cap after enough completed probes");
    require(program.pool_statistics().queries_admitted == admitted,
            "pool records only actual launches, not busy admission attempts");
    require(program.pool_statistics().queries_policy_skipped == skipped,
            "pool reports each learned skip");
    crank::AssistedStatistics stats;
    require(program.full_match(std::string(512, 'b'), {}, &stats), "different length class accepts");
    require(admission_attempted(stats),
            "one size class cannot suppress the first admission attempt in another");
    program.wait_idle();

    crank::AssistedProgram always(ast, options(crank::AssistancePolicy::Always));
    for (unsigned query = 0; query != 12; ++query) {
        require(always.full_match(std::string(128, 'a'), {}, &stats), "always-on control accepts");
        require(admission_attempted(stats, 3),
                "always-on control attempts each query and keeps configured lanes when admitted");
        always.wait_idle();
    }
}

void budgets_and_distribution_changes() {
    const auto ast = crank::parse("(?<S>a(?&S)b|)");
    crank::detail::AstMachineProgram native(ast);
    crank::AssistedProgram selective(ast, options(crank::AssistancePolicy::Selective));
    const std::string good = std::string(64, 'a') + std::string(64, 'b');
    std::string bad = good;
    std::swap(bad[60], bad[67]); // same size, alphabet and root boundary
    crank::OccurrenceStatistics baseline;
    require(native.full_match(good, {}, &baseline), "native work budget reference accepts");
    crank::OccurrenceMatchOptions exact;
    exact.max_steps = baseline.work_steps;
    exact.max_states = baseline.states;
    for (unsigned query = 0; query != 96; ++query) {
        const bool expected = query < 64 || query % 2 == 0;
        crank::AssistedStatistics stats;
        // The trained class encounters same-size negative examples. Policy may
        // miss a profitable launch, but every completed verdict remains exact.
        if (expected) require(selective.full_match(good, exact, &stats),
                              "optional assistance never consumes native's exact budget");
        else require(!selective.full_match(bad, {}, &stats),
                     "distribution change preserves rejection despite stale policy");
        selective.wait_idle();
    }
    crank::OccurrenceMatchOptions zero;
    zero.max_steps = 0;
    bool limited = false;
    try { (void)selective.full_match(good, zero); }
    catch (const crank::ResourceLimit&) { limited = true; }
    require(limited, "learning cannot convert an unresolved valid query into rejection");
    selective.wait_idle();

    auto retained = selective;
    std::vector<std::future<bool>> callers;
    for (unsigned lane = 0; lane != 4; ++lane)
        callers.push_back(std::async(std::launch::async, [retained, lane] {
            for (unsigned query = 0; query != 32; ++query) {
                const auto n = 8 + (query + lane) % 16;
                if (!retained.full_match(std::string(n, 'a') + std::string(n, 'b'))) return false;
                if (retained.full_match(std::string(n, 'a') + std::string(n + 1, 'b'))) return false;
            }
            return true;
        }));
    for (auto& caller : callers) require(caller.get(), "racing policy updates cannot change membership");
    selective.wait_idle();
    require(selective.pool_statistics().active_helpers == 0, "concurrent policy leaves no stranded helper");
}

void progress_admission() {
    auto selected = options(crank::AssistancePolicy::Progress);
    selected.min_native_steps = 512;
    {
        const auto ast = crank::parse("ab[cd]*yz");
        const std::string input = "abq" + std::string(2048, 'c') + "yz";
        crank::detail::AstMachineProgram native(ast);
        crank::OccurrenceStatistics baseline;
        require(!native.full_match(input, {}, &baseline), "interior mismatch rejects natively");
        require(baseline.work_steps < selected.min_native_steps,
                "large cheap rejection completes below the progress threshold");
        crank::AssistedProgram progress(ast, selected);
        crank::AssistedStatistics stats;
        require(!progress.full_match(input, {}, &stats), "progress preserves cheap rejection");
        require(stats.decision == crank::AssistanceDecision::BelowWorkThreshold,
                "large input alone never wakes progress helpers");
        require(!stats.helpers_admitted && !stats.copied_input_bytes &&
                !stats.native_steps_before_admission, "cheap foreground copies and publishes nothing");
        require(stats.matching.work_steps == baseline.work_steps &&
                stats.matching.states == baseline.states,
                "deferred path neither restarts native work nor charges hidden native states");
    }
    {
        const auto ast = crank::parse("(?:ab)*");
        std::string input;
        for (unsigned n = 0; n != 1024; ++n) input += "ab";
        crank::detail::AstMachineProgram native(ast);
        crank::OccurrenceStatistics baseline;
        require(native.full_match(input, {}, &baseline), "progress budget control accepts");
        require(baseline.work_steps > selected.min_native_steps, "valid control reaches admission threshold");
        crank::OccurrenceMatchOptions exact;
        exact.max_steps = baseline.work_steps;
        exact.max_states = baseline.states;
        crank::AssistedProgram progress(ast, selected);
        for (unsigned query = 0; query != 12; ++query) {
            crank::AssistedStatistics stats;
            require(progress.full_match(input, exact, &stats), "progress retains exact native budget");
            require(admission_attempted(stats),
                    "every expensive query attempts admission regardless of previous unused probes");
            require(stats.native_steps_before_admission >= selected.min_native_steps &&
                    stats.native_steps_before_admission - selected.min_native_steps < 64,
                    "deferred admission occurs at the next bounded native polling point");
            require(stats.matching.work_steps == baseline.work_steps &&
                    stats.matching.states == baseline.states, "no native work is restarted at admission");
            progress.wait_idle();
        }
        auto bad = input;
        bad[bad.size() - 7] = 'a'; // unchanged input size and root boundary
        crank::AssistedStatistics stats;
        require(!progress.full_match(bad, {}, &stats), "same-size late error survives valid-query training");
        require(admission_attempted(stats),
                "progress has no learned blackout after a distribution shift");
        progress.wait_idle();

        crank::OccurrenceMatchOptions tiny;
        tiny.max_steps = 64;
        bool limited = false;
        try { (void)progress.full_match(input, tiny, &stats); }
        catch (const crank::ResourceLimit&) { limited = true; }
        require(limited && stats.matching.limit_reached, "small native budget remains unresolved");
        require(stats.decision == crank::AssistanceDecision::BelowWorkThreshold &&
                !stats.helpers_admitted, "limit below threshold cannot spuriously launch helpers");
    }
    {
        auto immediate = selected;
        immediate.min_native_steps = 0;
        crank::AssistedProgram program(crank::parse("[ab]*"), immediate);
        crank::AssistedStatistics stats;
        require(program.full_match("ab", {}, &stats), "zero threshold is allowed");
        require(admission_attempted(stats) && stats.native_steps_before_admission == 0,
                "zero threshold attempts admission at the initial poll");
        program.wait_idle();
    }
}

void deferred_hook_contract() {
    const auto ast = crank::parse("(?:ab)*");
    crank::detail::AstMachineProgram native(ast);
    std::string input;
    for (unsigned n = 0; n != 256; ++n) input += "ab";
    struct Probe {
        unsigned calls = 0;
        std::size_t at = 0;
        const crank::detail::AtomicExclusions* result = nullptr;
    } probe;
    crank::detail::DeferredAssistance hook{65, &probe,
        [](void* context, std::size_t steps) noexcept {
            auto& value = *static_cast<Probe*>(context);
            ++value.calls;
            value.at = steps;
            return value.result;
        }};
    crank::OccurrenceStatistics baseline, deferred;
    require(native.full_match(input, {}, &baseline), "deferred hook baseline accepts");
    require(native.full_match_deferred(input, {}, hook, &deferred),
            "a declined callback leaves native execution authoritative");
    require(probe.calls == 1 && probe.at == 128,
            "null callback result is attempted exactly once at rounded threshold");
    require(deferred.work_steps == baseline.work_steps && deferred.states == baseline.states,
            "declined callback preserves all native work and state charges");

    hook.after_steps = 0;
    probe = {};
    require(native.full_match_deferred(input, {}, hook, &deferred),
            "zero-threshold null callback preserves recognition");
    require(probe.calls == 1 && probe.at == 0,
            "zero threshold deterministically calls once at the initial poll");
    require(deferred.work_steps == baseline.work_steps && deferred.states == baseline.states,
            "initial callback does not alter native work or state charges");
    hook.after_steps = 65;

    crank::detail::AtomicExclusions wrong(ast.nodes.size() + 1);
    probe = {0, 0, &wrong};
    bool rejected_shape = false;
    try { (void)native.full_match_deferred(input, {}, hook); }
    catch (const std::invalid_argument&) { rejected_shape = true; }
    require(rejected_shape && probe.calls == 1, "deferred tables must belong to the compiled AST shape");

    input[input.size() / 2] = 'b'; // known impossible pair, unchanged root boundary
    crank::detail::AtomicExclusions proof(ast.nodes.size());
    proof.reject_root();
    probe = {0, 0, &proof};
    crank::detail::NativeAssistanceStatistics consumption;
    require(!native.full_match_deferred(input, {}, hook, &deferred, &consumption),
            "a ready monotone root proof can end a deferred native query");
    require(probe.calls == 1 && consumption.root_rejections == 1,
            "deferred proof is consumed without another speculative step");
}
}

int main() {
    try {
        static_capabilities();
        bounded_backoff();
        budgets_and_distribution_changes();
        progress_admission();
        deferred_hook_contract();
        std::cout << "assistance policy: " << checks << " checks\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
