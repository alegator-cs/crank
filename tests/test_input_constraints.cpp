#include "input_constraints.hpp"
#include "ast_machine.hpp"
#include "occurrence_matcher.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using crank::detail::InputConstraints;
std::size_t checks = 0;
void require(bool ok, const std::string& context) {
    ++checks;
    if (!ok) throw std::runtime_error(context);
}

void root_culling() {
    struct Case { const char* pattern; const char* input; bool possible; };
    for (const Case item : {
        Case{"abc(def)*z", "abcz", true},
        Case{"abc(def)+z", "abcz", false},
        Case{"abc(def)*z", "abcdefz", true},
        Case{"(abc)*(def)*", "defdefdefabcabc", false},
        Case{"(abc)*(def)*", "abcabcdefdef", true},
        Case{"(abc)*(def)*a", "abcadef", false},
        Case{"(abc)*(def)*a", "abcabca", true},
        Case{"(?(DEFINE)(?<unused>z))ab", "z", false},
        Case{"(?(DEFINE)(?<unused>z))ab", "ab", true},
        Case{"(?(DEFINE)(?<E>))a(?&E)b", "ab", true},
        Case{"(?(DEFINE)(?<E>(?&E)|))a(?&E)b", "ab", true},
        Case{"(?<S>(?&S))", "", false},
        Case{"(?<S>(?&S)|)", "", true},
        Case{"(?<S>(?&S)){0}", "", true},
        Case{"(?<S>(?&S)){1}", "", false},
        Case{"(?(DEFINE)(?<A>(?&B)|a)(?<B>(?&A)|))(?&A)", "a", true},
        Case{"(?(DEFINE)(?<A>(?&B)|a)(?<B>(?&A)|))(?&A)", "", true},
        Case{"(?:ab|bc)*", "abcbc", true},
        Case{"(?:ab|bc)*", "abc", true}, // surviving filters need not imply membership
        Case{"", "", true}, Case{"", "a", false},
    }) {
        const auto ast = crank::parse(item.pattern);
        const crank::OccurrenceAnalysis analysis(ast);
        const InputConstraints compiled(ast, analysis);
        const auto index = compiled.build(item.input);
        const auto context = std::string(item.pattern) + " on '" + item.input + "'";
        require(!index.stats().declined, context + ": small index completed");
        require(index.root_possible() == item.possible, context + ": input-relative root productivity");
        require(index.stats().bytes_scanned <= std::string_view(item.input).size(),
                context + ": byte scan stays within the subject");
        if (!item.possible) require(!crank::OccurrenceProgram(ast).full_match(item.input),
                                    context + ": each rejection is independently confirmed");
    }
    const auto ast = crank::parse("abc(def)*z");
    const crank::OccurrenceAnalysis analysis(ast);
    const auto index = InputConstraints(ast, analysis).build("abcz");
    require(index.stats().inactive_nodes > 0, "absent optional body is marked inactive without killing star");
}

void overlapping_literals_and_runs() {
    {
        const auto ast = crank::parse("aaa");
        const crank::OccurrenceAnalysis analysis(ast);
        const auto index = InputConstraints(ast, analysis).build("aaaaa");
        const auto* starts = index.starts(ast.root);
        require(starts && *starts == std::vector<std::size_t>{0, 1, 2},
                "fixed literal index retains every overlapping occurrence in order");
        require(index.literal_size(ast.root) == 3, "literal width describes the complete fragment");
        for (std::size_t begin = 0; begin <= 5; ++begin)
            for (auto end = begin; end <= 5; ++end) {
                const bool expected = end - begin == 3;
                require(index.exact(ast.root, begin, end) == expected,
                        "exact literal answers agree with direct substring comparison");
                if (expected) require(index.possible(ast.root, begin, end),
                                      "an actual literal occurrence is never culled");
            }
    }
    for (const auto& pattern : {std::string("(?:aa){2,3}"), std::string("(?:aaa)*")}) {
        const auto ast = crank::parse(pattern);
        const crank::OccurrenceAnalysis analysis(ast);
        const std::string subject(8, 'a');
        const auto index = InputConstraints(ast, analysis).build(subject);
        const auto width = pattern == "(?:aaa)*" ? std::size_t(3) : std::size_t(2);
        for (std::size_t begin = 0; begin <= subject.size(); ++begin)
            for (auto end = begin; end <= subject.size(); ++end) {
                const auto length = end - begin;
                const bool expected = length % width == 0 &&
                    (width == 3 || (length / width >= 2 && length / width <= 3));
                require(index.exact(ast.root, begin, end) == expected,
                        "exact repeated fixed fragment enforces alignment and count bounds");
            }
    }
    {
        const auto ast = crank::parse("(?:(?:ab)*|b*)");
        const crank::OccurrenceAnalysis analysis(ast);
        const auto index = InputConstraints(ast, analysis).build("abbab");
        require(!index.stats().declined, "nonconsecutive runs are completely indexed");
        crank::NodeId repeat = crank::no_id;
        for (crank::NodeId id = 0; id < ast.nodes.size(); ++id) {
            const auto& node = ast.nodes[id];
            if (node.kind == crank::Kind::Repeat && index.literal_size(node.children.at(0)) == 2)
                repeat = id;
        }
        require(repeat != crank::no_id, "test locates the fixed-ab repetition node");
        require(index.exact(repeat, 0, 2) == true && index.exact(repeat, 3, 5) == true,
                "runs retain both independent exact occurrences");
        require(index.exact(repeat, 0, 5) == false && index.exact(repeat, 0, 4) == false,
                "a gap between occurrences cannot become a repeated span");
    }
}

void safe_declines() {
    const auto ast = crank::parse("(?:aaa)*");
    const crank::OccurrenceAnalysis analysis(ast);
    const InputConstraints compiled(ast, analysis);
    const std::string input(9, 'a');
    const auto child = ast.nodes[ast.root].children.at(0);
    for (unsigned mode = 0; mode < 3; ++mode) {
        InputConstraints::Limits limits;
        if (mode == 0) limits.max_steps = 0;
        if (mode == 1) limits.max_occurrences = 0;
        if (mode == 2) limits.max_input_bytes = input.size() - 1;
        const auto index = compiled.build(input, limits);
        require(index.stats().declined, "optional index declines at its configured resource cap");
        require(index.root_possible(), "a declined optional scan does not invent a rejection");
        require(index.starts(child) == nullptr, "incomplete occurrence lists are not exposed as complete");
        require(index.possible(ast.root, 0, input.size()), "a valid root survives optional index decline");
    }
    // Find a cut after scanning but before all subsequent analyses complete.
    // This tests the public completion boundary, without fixing private work
    // accounting to one particular implementation's number of charges.
    bool observed_partial = false;
    for (std::size_t budget = 1; budget < 512; ++budget) {
        InputConstraints::Limits limits;
        limits.max_steps = budget;
        const auto index = compiled.build(input, limits);
        if (index.stats().declined && index.starts(child)) {
            observed_partial = true;
            require(*index.starts(child) == std::vector<std::size_t>{0, 1, 2, 3, 4, 5, 6},
                    "a completed occurrence scan remains exact when a later optional pass declines");
            require(index.root_possible(), "unfinished later propagation preserves a valid root");
            require(index.exact(child, 1, 4) == true,
                    "exact fragment lookup is usable after a later optional decline");
            break;
        }
    }
    require(observed_partial, "tested the scan-complete but analysis-incomplete boundary");

    crank::Ast large;
    large.nodes.resize(65'537); // exceeds the documented optional compiler node cap
    const crank::OccurrenceAnalysis large_analysis(large);
    const auto declined = InputConstraints(large, large_analysis).build("");
    require(declined.stats().declined, "oversized optional compiled index declines");
    require(declined.root_possible(), "compile cap leaves empty-word membership unknown");
}

void byte_classes_and_sound_span_answers() {
    const std::vector<std::string> patterns{
        "[\\x00-\\xff]*", "(?:\\x00a|\\xffb)*", "(?:[^a]a?)*",
        "(?:a?|b?){1,3}", "(?<S>a(?&S)b|)", "(?<S>(?&S)a|)",
        "(?(DEFINE)(?<S>a|))(?&S)(?&S)b", "a*ab", "(?:aa|a)*b?"
    };
    const std::vector<std::string> subjects{
        "", "a", "b", "aa", "ab", "aba", "aabb", "aaaab",
        std::string("\0a\xff" "b", 4), std::string("\0\xff", 2)
    };
    for (const auto& pattern : patterns) {
        const auto ast = crank::parse(pattern);
        const crank::OccurrenceAnalysis analysis(ast);
        const InputConstraints compiled(ast, analysis);
        for (const auto& subject : subjects) {
            const auto index = compiled.build(subject);
            // Each node is recognized in its original AST so whole-pattern
            // calls retain their original target even when another entry is
            // queried. Native full_match_node supplies a retained independent
            // request/return implementation, not the new bidirectional engine.
            const crank::detail::AstMachineProgram native(ast);
            if (!index.root_possible()) require(!native.full_match(subject),
                                                "global scan never eliminates a native root match");
            for (crank::NodeId id = 0; id < ast.nodes.size(); ++id)
                for (std::size_t begin = 0; begin <= subject.size(); ++begin)
                    for (auto end = begin; end <= subject.size(); ++end) {
                        const bool expected = native.full_match_node(id,
                            std::string_view(subject).substr(begin, end - begin), {}, nullptr);
                        if (expected) require(index.possible(id, begin, end),
                                              "necessary conditions retain each supported node span");
                        if (const auto exact = index.exact(id, begin, end))
                            require(*exact == expected, "every exposed exact answer agrees with retained core");
                    }
        }
    }
}
} // namespace

int main() {
    try {
        root_culling();
        overlapping_literals_and_runs();
        safe_declines();
        byte_classes_and_sound_span_answers();
        std::cout << "input constraints regression: " << checks << " checks\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
