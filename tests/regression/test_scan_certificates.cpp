#include "crank.hpp"
#include "fragment_matcher.hpp"
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
std::size_t checks = 0;
void require(bool ok, const char* message) {
    ++checks;
    if (!ok) throw std::runtime_error(message);
}
std::vector<std::string> words(std::string alphabet, std::size_t depth) {
    std::vector<std::string> r{std::string{}};
    std::size_t begin = 0, end = 1;
    for (std::size_t n = 0; n != depth; ++n) {
        for (std::size_t i = begin; i != end; ++i)
            for (const auto c : alphabet) r.push_back(r[i] + c);
        begin = end; end = r.size();
    }
    return r;
}
void exhaustive_boundaries() {
    const std::vector<std::string> bases{
        "", "a", "[ab]", ".", "[\\x00\\xff]", "(?:ab|ba)", "a?", "(?:a|)",
        "(?:ab|b|)", "(?:a?b?)*", "(?:[ab]c){0,3}", "(?:ab|ca){2}",
        "(?(DEFINE)(?<A>a|b))(?&A)*",
        "(?(DEFINE)(?<A>a?)(?<B>(?&A)b?))(?&B){0,3}",
        "(?(DEFINE)(?<A>ab|ba)(?<B>(?&A)?))(?&B)*",
        "(?(DEFINE)(?<A>)(?<B>(?&A)(?&A)))(?&B)*",
        "(?(DEFINE)(?<unused>(?&unused)))(?:a[bc])*"};
    const auto subjects = words(std::string{"abc\0\xff", 5}, 5);
    crank::RunOptions off; off.boundary_proofs = false;
    for (const auto& base : bases) {
        // Correlation, nullable concatenations and lower>1 repetition exercise
        // every boundary transfer separately from Cartesian eligibility.
        for (const auto& pattern : std::vector<std::string>{base, "(?:" + base + ")a?",
                 "a?(?:" + base + ")", "(?:" + base + "){2,3}"}) {
            const crank::Program program(pattern);
            const crank::RegularProgram dfa(program.ast());
            const crank::RegularFragment fragment(program.ast(), program.ast().root);
            require(fragment.has_boundary_certificate(), "small pure grammar lost boundary proof");
            for (const auto& subject : subjects) {
                const auto expected = dfa.full_match(subject);
                require(!expected || !fragment.boundary_impossible(subject), "unsound boundary proof");
                require(program.full_match(subject) == expected, "Auto boundary changes membership");
                require(program.full_match(subject, off) == expected, "boundary ablation changes membership");
            }
        }
    }
}
void calls_and_contracts() {
    const std::string flat = "(?:[A-Z]{4}:[0-9]{6}\\n)+";
    const std::string called = "(?(DEFINE)(?<L>[A-Z]{4})(?<D>[0-9]{6})(?<R>(?&L):(?&D)\\n))(?&R)+";
    const crank::Program a(flat), b(called);
    std::string subject;
    for (unsigned i = 0; i != 4096; ++i) subject += "ABCD:123456\n"; // width 12
    crank::RunStatistics stats;
    require(b.full_match(subject, {}, &stats) && stats.backend == "regular-scan", "called record does not expose scanner");
    crank::RunOptions no_call; no_call.call_aware_scan = false;
    require(b.full_match(subject, no_call, &stats) && stats.backend == "dfa", "call scan ablation is not DFA");
    for (const auto pos : {std::size_t{0}, std::size_t{1}, subject.size()/2,
                          subject.size()-3, subject.size()-2, subject.size()-1}) {
        auto changed = subject; changed[pos] = '!';
        require(!a.full_match(changed) && !b.full_match(changed), "record mutation accepted");
    }
    auto bad = subject; bad[bad.size()-2] = '!';
    require(!b.full_match(bad, {}, &stats) && stats.boundary_rejected && stats.boundary_checks == 1 &&
            stats.regular.transition_steps == 0, "suffix proof should omit all scan transitions");
    crank::RunOptions no_proof; no_proof.boundary_proofs = false;
    require(!b.full_match(bad, no_proof, &stats) && !stats.boundary_rejected &&
            stats.backend == "regular-scan" && stats.regular.transition_steps == bad.size(), "proof ablation should restore scan");
    bad = subject; bad[bad.size()-3] = '!';
    require(!b.full_match(bad, {}, &stats) && !stats.boundary_rejected,
            "mutation outside two-byte guard should reach scanner");

    const auto ast = crank::parse("(?(DEFINE)(?<A>ab|ba))(?&A)*");
    const crank::RegularFragment correlated(ast, ast.root);
    require(!correlated.direct() && !correlated.full_match("aa"), "multi-byte call alternatives lost correlation");
    require(correlated.full_match("abba") && correlated.full_match("baab"), "call alternatives became sticky");
    const crank::Program endpoints("(?(DEFINE)(?<A>a))(?&A)*");
    require(endpoints.endpoints("aa!") == std::vector<std::size_t>{0,1,2}, "suffix proof escaped Boolean contract");
    const auto prefix_ast = crank::parse("[ab]+");
    const crank::RegularFragment prefix(prefix_ast, prefix_ast.root);
    require(prefix.byte_class_prefix("aa!b!") == 2, "boundary proof changed earliest failure");
    require(prefix.matching_copies("aa!b!b", 2, 3) == 1, "boundary proof changed earliest copy");
    require(b.first_trace("ABCD:123456\n").has_value(), "direct call scan erased history graph");

    crank::CompileOptions tiny;
    tiny.regular.max_fragment_analysis_steps = 0;
    const crank::Program fallback(called, tiny);
    require(fallback.full_match(subject, {}, &stats) && stats.backend == "dfa" && !stats.boundary_checks,
            "analysis budget must decline to retained DFA");
    const crank::RegularFragment no_analysis(ast, ast.root, tiny.regular);
    require(!no_analysis.has_boundary_certificate() && !no_analysis.boundary_impossible("aa"),
            "unproved boundary must mean unknown");
    const crank::Program recursive("(?<A>a(?&A)?b)");
    require(recursive.full_match("aaabbb") && !recursive.full_match("aaabb"), "recursive fallback changed");
}
void integer_integration() {
    const crank::Program program("(?:a{127})*(?:a{131})*");
    const std::string hole(16379, 'a');
    crank::RunStatistics stats;
    require(!program.full_match(hole, {}, &stats) && stats.integer_length_rejected &&
            stats.backend == "integer-length-filter" && stats.integer_length_checks == 1 &&
            stats.occurrence.states == 0, "integer proof did not precede native fallback");
    crank::RunOptions limited; limited.integer_length_max_steps = 0;
    require(!program.full_match(hole, limited, &stats) && !stats.integer_length_rejected &&
            stats.integer_length_exhaustions == 1 && stats.occurrence.states != 0,
            "exhausted integer proof must retain complete native allowance");
    crank::CompileOptions no_prepare; no_prepare.prepare_integer_row = false;
    const crank::Program unprepared("(?:a{127})*(?:a{131})*", no_prepare);
    require(!unprepared.full_match(hole, {}, &stats) && !stats.integer_length_checks,
            "unprepared integer row attempted proof");
    require(program.full_match(std::string(16380, 'a')), "feasible row misclassified as rejection");
    crank::RunOptions invalid; invalid.backend = crank::Backend::Greedy;
    bool caught = false;
    try { (void)program.full_match(hole, invalid); }
    catch (const std::invalid_argument&) { caught = true; }
    require(caught, "negative proof bypassed unsupported forced backend");
}
void dag_budget() {
    // An epsilon DAG has exponentially many paths, but one memo summary per
    // node and aliases remove it before period emission. Do not expand calls.
    std::string pattern = "(?(DEFINE)(?<E0>)";
    for (unsigned i = 1; i != 8; ++i)
        pattern += "(?<E" + std::to_string(i) + ">(?&E" + std::to_string(i-1) + ")(?&E" + std::to_string(i-1) + "))";
    pattern += ")(?&E7)(?:ab)*";
    const crank::Program program(pattern);
    crank::RunStatistics stats;
    require(program.full_match("abab", {}, &stats) && stats.backend == "regular-scan", "epsilon DAG alias not exposed");
    require(stats.regular.fragment_analysis_steps < 2048, "epsilon DAG analysis expanded paths");
}
}
int main() {
    try { exhaustive_boundaries(); calls_and_contracts(); dag_budget(); integer_integration();
        std::cout << "scan certificates: " << checks << " checks passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
