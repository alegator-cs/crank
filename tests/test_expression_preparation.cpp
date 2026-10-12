#include "ast_machine.hpp"
#include "crank.hpp"
#include "equality_matcher.hpp"
#include "fragment_matcher.hpp"

#include <atomic>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace crank;
std::size_t checks = 0;

void require(bool value, const std::string& context) {
    ++checks;
    if (!value) throw std::runtime_error(context);
}

template<class Exception, class Function>
std::string failure(Function function, const std::string& context) {
    try { function(); }
    catch (const Exception& error) { ++checks; return error.what(); }
    throw std::runtime_error(context + ": expected exception");
}

bool same_statistics(const RegularStatistics& a, const RegularStatistics& b) {
    return a.nfa_states == b.nfa_states && a.dfa_states == b.dfa_states &&
        a.workers == b.workers && a.transition_steps == b.transition_steps &&
        a.fragment_analysis_steps == b.fragment_analysis_steps &&
        a.dfa_table_bytes == b.dfa_table_bytes && a.paired_table_bytes == b.paired_table_bytes &&
        a.paired_analysis_steps == b.paired_analysis_steps;
}

std::vector<std::string> words(std::string_view alphabet, unsigned depth) {
    std::vector<std::string> result{std::string{}};
    std::size_t begin = 0, end = 1;
    for (unsigned n = 0; n < depth; ++n) {
        for (auto i = begin; i < end; ++i)
            for (char byte : alphabet) result.push_back(result[i] + byte);
        begin = end; end = result.size();
    }
    return result;
}

void compare_fragments(const Ast& source, NodeId root, RegularOptions options = {}) {
    const RegularFragment selected(source, root, options);
    const RegularFragment roots(source, std::vector<NodeId>{root}, options);
    Ast independent = source;
    // Force the existing component-copy route without changing actual nodes.
    // component_ast clears this convenience flag before compiling the view.
    independent.has_references = true;
    const RegularFragment copied(independent, std::vector<NodeId>{root}, options);
    require(selected.direct() == roots.direct() && selected.direct() == copied.direct(),
            "fragment overload changed direct-scan admission");
    require(selected.has_boundary_certificate() == roots.has_boundary_certificate() &&
            selected.has_boundary_certificate() == copied.has_boundary_certificate(),
            "fragment overload changed boundary admission");
    require(same_statistics(selected.compiled_statistics(), roots.compiled_statistics()) &&
            same_statistics(selected.compiled_statistics(), copied.compiled_statistics()),
            "fragment overload changed compile statistics");
    for (const auto& subject : words("abc", 3)) {
        RegularStatistics a, b, c;
        const bool answer = selected.full_match(subject, &a, ExternalRegularPolicy::Native);
        require(answer == roots.full_match(subject, &b, ExternalRegularPolicy::Native) &&
                answer == copied.full_match(subject, &c, ExternalRegularPolicy::Native),
                "fragment overload changed membership");
        require(same_statistics(a, b) && same_statistics(a, c),
                "fragment overload changed scan statistics");
        require(selected.boundary_impossible(subject) == roots.boundary_impossible(subject) &&
                selected.boundary_impossible(subject) == copied.boundary_impossible(subject),
                "fragment overload changed necessary boundary proof");
        require(!answer || !selected.boundary_impossible(subject),
                "boundary proof rejected a recognized word");
        require(selected.regular_program().endpoints(subject) == roots.regular_program().endpoints(subject) &&
                selected.regular_program().endpoints(subject) == copied.regular_program().endpoints(subject),
                "retained DFA changed prefix endpoints");
    }
}

void regular_components_and_ownership() {
    for (const auto& pattern : {"", "abc", "[ab]+", "(?:ab|c)*", "(?<x>[ab]*)c?",
                                "(?:a?|b?)*", "(?:ab){0}"}) {
        const auto ast = parse(pattern);
        compare_fragments(ast, ast.root);
    }
    // A declined optional boundary/direct analysis must retain the same DFA.
    auto ast = parse("[ab]{2}c");
    RegularOptions analysis_cap; analysis_cap.max_fragment_analysis_steps = 0;
    compare_fragments(ast, ast.root, analysis_cap);
    const RegularFragment capped_analysis(ast, ast.root, analysis_cap);
    require(!capped_analysis.direct() && !capped_analysis.has_boundary_certificate() &&
            capped_analysis.full_match("abc"), "optional analysis cap lost exact DFA execution");

    const auto detached = [] {
        auto temporary = parse("[ab]{2}c");
        return RegularFragment(temporary, temporary.root);
    }();
    const RegularFragment retained(ast, ast.root);
    ast.nodes.clear(); ast.capture_names.clear(); ast.capture_nodes.clear();
    for (const auto* fragment : {&detached, &retained}) {
        require(fragment->full_match("abc") && fragment->full_match("aac") &&
                !fragment->full_match("abx"), "compiled fragment borrowed source lifetime");
        require(fragment->regular_program().endpoints("abc!") == std::vector<std::size_t>{3},
                "retained DFA borrowed mutated source");
    }

    // A selected regular subtree can belong to a stateful source. Its source
    // flags and unrelated effect nodes must not change component admission.
    const auto stateful = parse("a{#n}b{=n}");
    NodeId literal = no_id;
    for (NodeId id = 0; id < stateful.nodes.size(); ++id)
        if (stateful.nodes[id].kind == Kind::CharSet && stateful.nodes[id].characters['a']) {
            literal = id; break;
        }
    require(literal != no_id, "stateful fixture has no literal component");
    compare_fragments(stateful, literal);

    const auto called = parse("(?(DEFINE)(?<A>a|b))(?&A)");
    compare_fragments(called, called.root);
    const RegularFragment empty(called, std::vector<NodeId>{});
    require(empty.full_match("") && !empty.full_match("a"), "empty roots changed epsilon language");
    const RegularFragment joined(called, std::vector<NodeId>{called.root, called.root});
    require(joined.full_match("ab") && joined.full_match("ba") &&
            !joined.full_match("a") && !joined.full_match("abc"),
            "multiple roots changed composition or resolved call targets");
}

template<class Exception>
void fragment_failure(const Ast& ast, RegularOptions options, const std::string& expected) {
    const auto selected = failure<Exception>([&] { RegularFragment value(ast, ast.root, options); }, expected);
    const auto roots = failure<Exception>([&] { RegularFragment value(ast, std::vector<NodeId>{ast.root}, options); }, expected);
    require(selected == expected && roots == expected, "fragment diagnostic changed: " + expected);
}

void invalid_graphs_and_caps() {
    auto hidden = parse(R"((a)\1)");
    hidden.has_references = hidden.has_counts = hidden.has_calls = hidden.has_extended =
        hidden.has_occurrence_paths = false;
    fragment_failure<std::invalid_argument>(hidden, {}, "regular fragment contains a nonregular node");
    auto dead_effect = parse(R"((?:(a)\1){0})");
    dead_effect.has_references = false;
    fragment_failure<std::invalid_argument>(dead_effect, {}, "regular fragment contains a nonregular node");

    auto invalid = parse("a");
    invalid.nodes[invalid.root].kind = Kind::Capture;
    invalid.nodes[invalid.root].children = {invalid.nodes.size()};
    fragment_failure<std::invalid_argument>(invalid, {}, "regular fragment has an invalid child index");
    invalid.nodes[invalid.root].children = {invalid.root};
    fragment_failure<std::invalid_argument>(invalid, {}, "regular fragment contains a structural cycle");
    invalid = parse("a");
    invalid.nodes[invalid.root].children = {invalid.root};
    fragment_failure<std::invalid_argument>(invalid, {}, "regular fragment leaf has children");
    invalid = parse("a{2}");
    invalid.nodes[invalid.root].upper = 1;
    fragment_failure<std::invalid_argument>(invalid, {}, "regular fragment has reversed repetition bounds");

    // Unrelated malformed/reference nodes remain irrelevant to a selected
    // component even when the public convenience flags say regular.
    auto unrelated = parse("a");
    Node reference; reference.kind = Kind::Reference; reference.capture_id = no_id;
    unrelated.nodes.push_back(reference);
    Node broken; broken.kind = Kind::Capture; broken.children = {no_id};
    unrelated.nodes.push_back(broken);
    compare_fragments(unrelated, unrelated.root);

    const auto ast = parse("abc");
    RegularOptions cap; cap.max_nfa_states = 0;
    fragment_failure<std::length_error>(ast, cap, "regular NFA state limit exceeded");
    cap = {}; cap.max_dfa_states = 0;
    fragment_failure<std::length_error>(ast, cap, "regular DFA state limit exceeded");
    cap = {}; cap.max_expansion_depth = 0;
    fragment_failure<std::length_error>(ast, cap, "regular NFA expansion depth limit exceeded");
}

void program_dispatch_and_observations() {
    struct Case { const char* pattern; const char* subject; const char* backend; };
    for (const auto& fixture : std::vector<Case>{{"(?<x>[ab]*)", "abba", "regular-scan"},
            {R"((?<x>[ab]*)\k<x>)", "abbaabba", "equality-plan"},
            {"a{#n}b{=n}", "aabb", "count-plan"}}) {
        const Program program(fixture.pattern);
        RunStatistics stats;
        require(program.full_match(fixture.subject, {}, &stats) && stats.backend == fixture.backend,
                "preparation changed specialized dispatch");
        RunOptions states; states.backend = Backend::States; states.optimize = false;
        RunOptions occurrences; occurrences.backend = Backend::Occurrences; occurrences.optimize = false;
        require(program.full_match(fixture.subject, states) && program.full_match(fixture.subject, occurrences),
                "specialization disagrees with explicit general backends");
        const std::string rejected = std::string(fixture.subject) + 'c';
        require(!program.full_match(rejected) && !program.full_match(rejected, states) &&
                !program.full_match(rejected, occurrences), "preparation changed rejection");
        const std::string prefixes = std::string(fixture.subject) + '!';
        require(program.endpoints(prefixes) == program.endpoints(prefixes, 0, states) &&
                program.endpoints(prefixes) == program.endpoints(prefixes, 0, occurrences),
                "specialized preparation changed endpoint contract");
        const auto outcomes = program.outcomes(fixture.subject);
        const auto witness = program.first_match(fixture.subject);
        const auto trace = program.first_trace(fixture.subject);
        require(!outcomes.empty() && witness && trace && witness->end == std::string(fixture.subject).size() &&
                trace->outcome == *witness && !trace->occurrences.empty(),
                "preparation changed capture/count/history observations");
        if (program.ast().has_counts)
            require(witness->counts.size() == 1 && witness->counts[0] == 2, "shared count observation lost");
        else
            require(witness->captures.size() == 2 && witness->captures[1] == Span{0, 4},
                    "capture/reference observation lost");
        states.match.max_steps = 0;
        (void)failure<ResourceLimit>([&] { (void)program.full_match(fixture.subject, states); }, "state work cap");
        occurrences.occurrence.max_steps = 0;
        (void)failure<ResourceLimit>([&] { (void)program.first_trace(fixture.subject, occurrences); }, "history work cap");
    }

    // The Program shortcut must not change the separate public plan builder's
    // treatment of actual reference nodes with stale convenience metadata.
    auto stale = parse(R"((?<x>[ab]*)\k<x>)"); stale.has_references = false;
    const auto equality = make_equality_plan(stale);
    require(equality && equality->match("abbaabba") && !equality->match("abbaabab"),
            "public equality admission trusted stale reference metadata");

    CompileOptions compile; compile.regular.max_nfa_states = 0;
    const Program limited("abc", compile);
    RunOptions plain; plain.optimize = false;
    require(limited.full_match("abc", plain) && !limited.full_match("acb", plain),
            "regular compile cap lost general fallback");
    RunOptions dfa; dfa.backend = Backend::Dfa;
    require(failure<ResourceLimit>([&] { (void)limited.full_match("abc", dfa); }, "forced DFA cap") ==
            "DFA compilation: regular NFA state limit exceeded", "forced DFA cap diagnostic changed");
    plain.occurrence.max_states = 0;
    (void)failure<ResourceLimit>([&] { (void)limited.full_match("abc", plain); }, "fallback state cap");

    const Program recursive("(?<S>a(?&S)b|)");
    const detail::AstMachineProgram native(recursive.ast());
    require(native.full_match("aabb"), "native cancellation fixture warm-up failed");
    std::atomic_bool stopped{true}; OccurrenceStatistics statistics; bool cancelled = false;
    try { (void)native.full_match_node(recursive.ast().root, "aabb", {}, &statistics, &stopped); }
    catch (const detail::AstExecutionCancelled&) { cancelled = true; }
    require(cancelled && !statistics.limit_reached, "pre-cancelled query became rejection or resource limit");
}
}

int main() {
    try {
        regular_components_and_ownership(); invalid_graphs_and_caps(); program_dispatch_and_observations();
        std::cout << "PASS: " << checks << " expression preparation checks\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n'; return 1;
    }
}
