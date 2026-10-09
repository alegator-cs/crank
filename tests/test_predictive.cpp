#include "ast_machine.hpp"
#include "predictive_regions.hpp"
#include <iostream>
#include <stdexcept>
#include <chrono>
#include <functional>
using namespace crank;
void check(bool c, const char* what) { if (!c) throw std::runtime_error(what); }
int main(int argc, char**) {
    const std::vector<std::string> patterns{
        "(?<S>a(?&S)b|)", "(?<S>a(?&S)?b)",
        "(?<S>\\((?&S)*\\)|x)", "(?<S>a(?&T)b|)(?(DEFINE)(?<T>c(?&S)d|))",
        "(?<S>a(?&S)b|c(?&S)d|)", "(?<S>a{2,4}(?&S)b|)",
        "(?<S>a*(?&S)b|)", "(?<S>a?(?&S)b|)", "(?<S>(?&S)a|)",
        "(?<S>a(?&S)b|a)", "(?<S>a(?&S)a|)", "(?<S>a(?&S)b|)(?&S)",
        "(?<S>a(?&S)b|)b", "(?<S>a(?&S)b|)(?<T>c(?&T)d|)",
        "(?<S>a(?&S)b|c?)", "(?<S>a(?&S)b|c*)", "(?<S>a(?&S)b|c{2})",
        "(?<S>a(?&S)b|(?:c?){2})", "(?<S>a(?&S)b|(?:c?){0})",
        "(?<S>a(?&S)b|x{0,3})", "(?<S>a(?&S)b|x{2,3})",
        "(?<S>a(?&S)b|x{2,3})x", "(?<S>a(?&S)b|x{2})x",
        "(?<S>a(?&S)b|)(?(DEFINE)(?<T>(?&T)|))"
    };
    std::size_t comparisons = 0, attempts = 0;
    OccurrenceMatchOptions on, off; on.exact_end_spines=off.exact_end_spines=false; on.predictive_exact_rims=off.predictive_exact_rims=false; off.predictive_recursion = false; off.exact_end_spines=false; off.predictive_exact_rims=false;
    // This suite isolates the whole-root predictor from later regional tiers.
    on.regional_execution=on.regional_constraints=off.regional_execution=off.regional_constraints=false;
    // Preserve this historical representation under the newer root tiers.
    on.bounded_word_prediction=on.deterministic_programs=false;off.bounded_word_prediction=off.deterministic_programs=false;
    on.max_states = off.max_states = 4'000'000;
    on.max_steps = off.max_steps = 32'000'000;
    for (const auto& pattern : patterns) {
        const auto ast = parse(pattern);
        detail::AstMachineProgram p(ast);
        OccurrenceAnalysis a(ast); detail::PredictiveRegion certificate(ast,a);
        std::cerr << pattern << " certified=" << certificate.certified << '\n';
        const auto verify = [&](std::string_view s) {
            OccurrenceStatistics x,y;
            const bool expected = p.full_match(s,off,&x), actual = p.full_match(s,on,&y);
            if (expected != actual) { std::cerr << "Mismatch " << pattern << " / " << s << '\n'; std::abort(); }
            attempts += y.predictive_attempts; ++comparisons;
            auto scalar = on; scalar.predictive_continuation_runs = false;
            check(p.full_match(s, scalar) == expected, "predictive run ablation mismatch");
            const auto ends = p.endpoints(s,0,on,&y);
            check(y.predictive_attempts == 0,"predictor reached endpoint API");
            check(ends == p.endpoints(s,0,off),"endpoint equality");
        };
        std::string s;
        std::function<void(int)> all = [&](int depth) {
            verify(s); if (!depth) return;
            for (const char c : std::string("abcx")) { s += c; all(depth-1); s.pop_back(); }
        }; all(argc > 1 ? 4 : 6);
        for (const auto& sample : {"ad","acdb","aacddbb","x","(x)","((x)x)","()","(())","(()x)","()x","((())"}) verify(sample);
    }
    check(attempts > 0,"no activation");
    auto ast = parse("(?<S>a(?&S)b|)"); detail::AstMachineProgram p(ast);
    for (const std::size_t n : {0,1,2,8,64,512,2048,16384}) {
        auto s = std::string(n,'a') + std::string(n,'b');
        OccurrenceStatistics reference, accelerated;
        check(p.full_match(s,off,&reference),"reference nesting");
        check(p.full_match(s,on,&accelerated),"predictive nesting");
        auto scalar = on; scalar.predictive_continuation_runs = false;
        check(p.full_match(s,scalar), "unfolded predictive nesting");
        if (n >= 2) check(accelerated.states == 4, "equal continuations were materialized");
        std::cout << "nest " << n << " oldstates=" << reference.states << " states=" << accelerated.states
            << " oldsteps=" << reference.work_steps << " steps=" << accelerated.work_steps
            << " requests=" << accelerated.memo_entries << " frames=" << accelerated.predictive_frames << '\n';
    }
    // Exact ordinary allowances remain available after a failed optional attempt.
    for (std::size_t states=0;states<80;++states) for(std::size_t steps=0;steps<100;++steps) {
        off.max_states = on.max_states = states; off.max_steps = on.max_steps = steps;
        std::optional<bool> expected;
        try { expected = p.full_match("aabb",off); } catch (const ResourceLimit&) {}
        if (expected) check(p.full_match("aabb",on)==*expected,"prior resource allowance lost");
    }
    // The local regular cursor may use fewer work steps than direct AST
    // expansion of a fixed repetition. Preserve its exact successful quota.
    detail::AstMachineProgram prefixed(parse("x{100}(?<S>a(?&S)b|)"));
    on = OccurrenceMatchOptions{}; on.exact_end_spines=false; on.predictive_exact_rims=false; off = on; off.predictive_recursion = false; off.exact_end_spines=false; off.predictive_exact_rims=false;
    on.regional_execution=on.regional_constraints=off.regional_execution=off.regional_constraints=false;
    // Preserve this historical representation under the newer root tiers.
    on.bounded_word_prediction=on.deterministic_programs=false;off.bounded_word_prediction=off.deterministic_programs=false;
    std::string prefix_subject = std::string(100, 'x') + "aabb";
    OccurrenceStatistics previous, retried;
    check(prefixed.full_match(prefix_subject, off, &previous), "prefix native");
    on.max_steps = previous.work_steps;
    check(prefixed.full_match(prefix_subject, on, &retried), "predictive allowance retry");
    check(retried.native_predictive_fallbacks == 1, "no predictive fallback exercised");
    check(retried.work_steps == previous.work_steps, "retry did not restore previous work");
    check(retried.native_acceleration_discarded_steps == on.max_steps, "discarded work accounting");
    std::cout << "predictive_fallback_steps=" << retried.native_acceleration_discarded_steps << '\n';
    std::cout << "comparisons=" << comparisons << " attempts=" << attempts << '\n';
}
