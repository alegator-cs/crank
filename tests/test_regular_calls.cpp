#include "crank.hpp"
#include "occurrence_matcher.hpp"
#include "regular_matcher.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace {
std::size_t checks = 0;
void require(bool value, const std::string& context) {
    ++checks;
    if (!value) throw std::runtime_error(context);
}
template<class Exception, class Function>
void throws(Function f, const std::string& context) {
    bool caught = false;
    try { f(); } catch (const Exception&) { caught = true; }
    require(caught, context);
}
std::vector<std::string> words(std::string_view alphabet, unsigned length) {
    std::vector<std::string> all{std::string{}};
    std::size_t begin = 0, end = 1;
    for (unsigned n = 0; n < length; ++n) {
        for (auto i = begin; i < end; ++i)
            for (char byte : alphabet) all.push_back(all[i] + byte);
        begin = end;
        end = all.size();
    }
    return all;
}
struct Case { std::string called, flattened; };
const std::vector<Case> cases{
    {R"((?(DEFINE)(?<A>a|ab))(?&A)b|(?&A)c)", R"((?:a|ab)b|(?:a|ab)c)"},
    {R"((?(DEFINE)(?<A>a|ab))(?&A)(?&A))", R"((?:a|ab)(?:a|ab))"},
    {R"((?(DEFINE)(?<A>a|ab))(?&A){0,3})", R"((?:a|ab){0,3})"},
    {R"((?(DEFINE)(?<A>a|bc))(?&A)*)", R"((?:a|bc)*)"},
    {R"((?(DEFINE)(?<A>a?|b?))(?&A){2,4})", R"((?:a?|b?){2,4})"},
    {R"((?(DEFINE)(?<A>a|)(?<B>(?&A)b))(?&B)*(?&A)c?)", R"((?:(?:a|)b)*(?:a|)c?)"},
    {R"((?(DEFINE)(?<A>(?&B)c)(?<B>a|b))(?&A)(?&B)?)", R"((?:a|b)c(?:a|b)?)"},
    // A cycle in an uncalled definition is irrelevant to the executed graph.
    {R"((?(DEFINE)(?<dead>x(?&dead))(?<A>a|b))(?&A)*c)", R"((?:a|b)*c)"},
    // A declaration is non-consuming even when its body contains an effect.
    {R"((?(DEFINE)(?<dead>(a)\2)(?<A>a|b))(?&A)*c)", R"((?:a|b)*c)"},
    {R"((?(DEFINE)(?<A>))(?&A)*)", R"((?:)*)"},
    // The callee is also entered structurally, with a distinct return site.
    {R"((?<A>a|ab)(?&A)?c)", R"((?:a|ab)(?:a|ab)?c)"},
};
void membership_and_endpoints() {
    const auto subjects = words("abc", 5);
    for (const auto& fixture : cases) {
        const crank::Program program(fixture.called);
        const crank::OccurrenceProgram native(program.ast());
        const crank::RegularProgram flat(crank::parse(fixture.flattened));
        require(crank::RegularProgram::eligible(program.ast()), fixture.called + ": certificate");
        const crank::RegularProgram direct(program.ast());
        crank::RunOptions forced, ordinary;
        forced.backend = crank::Backend::Dfa;
        ordinary.backend = crank::Backend::Occurrences;
        require(program.explain().find("acyclic executed calls") != std::string::npos,
                fixture.called + ": explain certificate");
        for (const auto& subject : subjects) {
            const bool expected = flat.full_match(subject);
            require(direct.full_match(subject) == expected, fixture.called + ": direct vs flat");
            require(native.full_match(subject) == expected, fixture.called + ": native vs flat");
            crank::RunStatistics stats;
            require(program.full_match(subject, {}, &stats) == expected, fixture.called + ": Auto answer");
            require(stats.backend == "dfa" || stats.backend == "length-filter" ||
                    stats.backend == "boundary-filter" || stats.backend == "regular-scan",
                    fixture.called + ": Auto exact regular route");
            crank::RunOptions legacy_route;
            legacy_route.boundary_proofs = legacy_route.call_aware_scan = false;
            require(program.full_match(subject, legacy_route, &stats) == expected,
                    fixture.called + ": prior regular dispatch answer");
            require(stats.backend == "dfa" || stats.backend == "length-filter",
                    fixture.called + ": prior regular dispatch route");
            require(program.full_match(subject, forced) == expected, fixture.called + ": forced DFA");
            crank::RunOptions unoptimized;
            unoptimized.optimize = false;
            require(program.full_match(subject, unoptimized) == expected, fixture.called + ": unoptimized Auto");
            const auto wrapped = '!' + subject + '!';
            for (std::size_t start = 0; start <= wrapped.size(); ++start) {
                const auto ends = flat.endpoints(wrapped, start);
                require(direct.endpoints(wrapped, start) == ends, fixture.called + ": direct endpoints");
                require(program.endpoints(wrapped, start) == ends, fixture.called + ": Auto endpoints");
                require(program.endpoints(wrapped, start, forced) == ends, fixture.called + ": forced endpoints");
                require(native.endpoints(wrapped, start) == ends, fixture.called + ": native endpoints");
            }
        }
    }
    // Independent finite oracle for the different-continuation trap.
    const crank::Program trap(cases.front().called);
    const std::vector<std::string> exact{"ab", "abb", "ac", "abc"};
    for (const auto& subject : words("abc", 5))
        require(trap.full_match(subject) == (std::find(exact.begin(), exact.end(), subject) != exact.end()),
                "independent four-word continuation oracle");
}
void binary_alphabet() {
    const crank::Program program(R"((?(DEFINE)(?<A>[\x00\xff]))(?&A){1,3})");
    for (unsigned byte = 0; byte < 256; ++byte) {
        for (unsigned n = 0; n < 5; ++n) {
            const std::string input(n, static_cast<char>(byte));
            require(program.full_match(input) == (n >= 1 && n <= 3 && (byte == 0 || byte == 255)),
                    "independent byte/count oracle");
        }
    }
    require(program.full_match(std::string{"\0\xff\0", 3}), "mixed NUL/high bytes");
}
void observations_stay_native() {
    const std::string pattern = R"((?(DEFINE)(?<A>(a|ab)))(?<W>(?&A){1,2})c)";
    const crank::Program program(pattern);
    const crank::OccurrenceProgram native(program.ast());
    crank::RunOptions force;
    force.backend = crank::Backend::Occurrences;
    for (const auto& subject : {"ac", "abc", "aabc", "abac", "ababc", "bc"}) {
        require(program.first_match(subject) == native.first_match(subject), "full capture witness preserved");
        require(program.outcomes(subject, force) == native.match_outcomes(subject), "all capture outcomes preserved");
        const auto a = program.first_trace(subject);
        const auto b = native.first_traced_match(subject);
        require(bool(a) == bool(b), "history presence preserved");
        if (a) {
            require(a->outcome == b->outcome && a->occurrences.size() == b->occurrences.size(),
                    "history outcome and record count preserved");
            for (std::size_t i = 0; i < a->occurrences.size(); ++i) {
                const auto& x = a->occurrences[i]; const auto& y = b->occurrences[i];
                require(std::tie(x.id, x.parent, x.kind, x.source_id, x.source_node, x.name,
                                 x.span, x.count, x.iteration, x.depth, x.height, x.source_rule,
                                 x.closed, x.children) ==
                        std::tie(y.id, y.parent, y.kind, y.source_id, y.source_node, y.name,
                                 y.span, y.count, y.iteration, y.depth, y.height, y.source_rule,
                                 y.closed, y.children), "complete history record preserved");
            }
        }
    }
    for (const auto& pattern : {R"((?(DEFINE)(?<A>a|ab))(?&A)\k<A>)",
                                R"((?(DEFINE)(?<A>a{#n}))(?&A)b{=n})",
                                R"((?(DEFINE)(?<A>a))(?&A)b{=A[0].@depth})"}) {
        const crank::Program effectful(pattern);
        require(!crank::RegularProgram::eligible(effectful.ast()), "effects exclude DFA");
        crank::RunOptions dfa; dfa.backend = crank::Backend::Dfa;
        throws<std::invalid_argument>([&] { (void)effectful.full_match("aa", dfa); }, "forced effects rejected");
    }
}
void recursion_and_budgets() {
    for (const auto& pattern : {R"((?<A>a(?&A)?))", R"((?<A>(?&A)|a))",
                                R"((?(DEFINE)(?<A>(?&B))(?<B>(?&A)|a))(?&A))"}) {
        const crank::Program recursive(pattern);
        require(!crank::RegularProgram::eligible(recursive.ast()), "reachable recursive cycle is not certified");
        crank::RunStatistics stats;
        require(recursive.full_match("a", {}, &stats) && stats.backend == "native-ast", "recursive Auto stays native");
        throws<std::invalid_argument>([&] { crank::RegularProgram rejected(recursive.ast()); }, "recursive direct rejected");
    }
    const auto pattern = cases.front().called;
    for (unsigned cap = 0; cap != 3; ++cap) {
        crank::CompileOptions compile;
        if (cap == 0) compile.regular.max_nfa_states = 0;
        if (cap == 1) compile.regular.max_dfa_states = 0;
        if (cap == 2) compile.regular.max_expansion_depth = 2;
        const crank::Program bounded(pattern, compile);
        crank::RunStatistics stats;
        require(bounded.full_match("abc", {}, &stats) && stats.backend == "native-ast", "Auto cap fallback");
        require(!bounded.full_match("acb"), "fallback mismatch");
        require(bounded.endpoints("!abc!", 1) == std::vector<std::size_t>({3, 4}), "fallback endpoints");
        crank::RunOptions dfa; dfa.backend = crank::Backend::Dfa;
        throws<crank::ResourceLimit>([&] { (void)bounded.full_match("abc", dfa); }, "forced cap reports limit");
        crank::RunOptions native_budget; native_budget.occurrence.max_states = 0;
        throws<crank::ResourceLimit>([&] { (void)bounded.full_match("abc", native_budget); }, "native cap remains independent");
    }
    // A size-O(depth) straight-line grammar denotes a word of length 2^depth.
    // Its NFA expansion must stop at the cap without materializing that AST.
    std::string exponential = "(?(DEFINE)(?<A0>a)";
    for (unsigned i = 1; i < 30; ++i)
        exponential += "(?<A" + std::to_string(i) + ">(?&A" + std::to_string(i - 1) + ")(?&A" + std::to_string(i - 1) + "))";
    exponential += ")(?&A29)";
    crank::CompileOptions compile; compile.regular.max_nfa_states = 64;
    const crank::Program capped(exponential, compile);
    require(capped.ast().nodes.size() < 160, "original compact AST retained");
    require(!capped.full_match("a"), "exponential expansion cap uses native rejection");
    require(capped.explain().find("NFA state limit exceeded") != std::string::npos, "expansion cap explained");
}
void actual_graph_and_depth() {
    for (bool forged : {false, true}) {
        auto ast = crank::parse(cases.front().called);
        ast.has_calls = ast.has_references = ast.has_counts = ast.has_extended = ast.has_occurrence_paths = forged;
        for (auto& node : ast.nodes) { node.stateful = forged; node.nullable = forged; node.minimum_length = 999; }
        const crank::RegularProgram regular(ast);
        require(regular.full_match("abc") && !regular.full_match("acb"), "metadata is not semantics");
    }
    auto effects = crank::parse(R"((a)\1)");
    effects.has_references = effects.has_counts = effects.has_calls = effects.has_extended = effects.has_occurrence_paths = false;
    require(!crank::RegularProgram::eligible(effects), "hidden actual reference still excluded");
    auto invalid = crank::parse("a");
    invalid.nodes[invalid.root].kind = crank::Kind::Call;
    invalid.nodes[invalid.root].call_target = invalid.nodes.size();
    require(!crank::RegularProgram::eligible(invalid), "invalid call target excluded");
    invalid.nodes[invalid.root].call_target = invalid.root;
    require(!crank::RegularProgram::eligible(invalid), "actual self-cycle excluded");
    // Iterative certification handles arbitrarily deep compact acyclic graphs;
    // bounded compilation stops before transparent wrappers overflow the stack.
    auto deep = crank::parse("a");
    for (unsigned n = 0; n < 100000; ++n) {
        crank::Node wrapper;
        wrapper.kind = n % 2 ? crank::Kind::Capture : crank::Kind::Call;
        if (wrapper.kind == crank::Kind::Capture) wrapper.children.push_back(deep.root);
        else wrapper.call_target = deep.root;
        deep.root = deep.nodes.size();
        deep.nodes.push_back(std::move(wrapper));
    }
    require(crank::RegularProgram::eligible(deep), "iterative deep certificate");
    crank::RegularOptions cap; cap.max_expansion_depth = 32;
    throws<std::length_error>([&] { crank::RegularProgram bounded(deep, cap); }, "deep compilation budget");
    throws<std::length_error>([&] { crank::RegularProgram bounded(deep); }, "default deep compilation guard");
}
}
int main() {
    try {
        membership_and_endpoints(); binary_alphabet(); observations_stay_native();
        recursion_and_budgets(); actual_graph_and_depth();
        std::cout << "PASS: " << checks << " regular call-graph checks\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n'; return 1;
    }
}
