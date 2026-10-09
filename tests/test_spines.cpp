#include "ast_machine.hpp"
#include "crank.hpp"
#include "tests/helpers/relation_reference.hpp"
#ifdef CRANK_SPINE_WITH_MARPA
#include "adapters/marpa_adapter.hpp"
#endif

#include <atomic>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>

using namespace crank;
namespace ref = certified_reference;
namespace {
std::size_t checks = 0, inputs = 0, relation_queries = 0, native_queries = 0;
std::size_t activations = 0, quota_completions = 0, spine_fallbacks = 0;
std::size_t marpa_queries = 0, marpa_compile_declines = 0;
std::string context;
void require(bool yes, const char* message) {
    ++checks;
    if (!yes) throw std::runtime_error(context + ": " + message);
}

struct Fixture {
    const char* name;
    const char* pattern;
    std::string alphabet;
    unsigned maximum;
    std::vector<std::string> extra;
};

std::vector<Fixture> fixtures() {
    return {
        {"empty base", R"((?<S>a(?&S)b|))", "ab", 6, {"aaaaabbbbb"}},
        {"nonempty base", R"((?<S>a(?&S)b|x))", "abx", 4, {"aaaxbbb"}},
        {"optional recursion", R"((?<S>a(?&S)?b))", "ab", 6, {}},
        {"alternating arms", R"((?<S>a(?&S)c|b(?&S)d|e))", "abcde", 3,
            {"abedc", "ababedcdc", "aabbeddcc", "ababecdcd"}},
        {"same FIRST different LAST", R"((?<S>a(?&S)b|a(?&S)c|))", "abc", 5,
            {"aaabcb", "aaaccc", "aaaccb"}},
        {"same LAST different FIRST", R"((?<S>a(?&S)c|b(?&S)c|))", "abc", 5, {}},
        {"correlated shell alternatives", R"((?<S>ab(?&S)cd|ba(?&S)dc|x))", "abcdx", 3,
            {"abxcd", "baxdc", "abxdc", "baxcd", "abbaxdccd", "abbaxcddc"}},
        {"independent shell alternatives", R"((?<S>(?:ab|ba)(?&S)(?:cd|dc)|x))", "abcdx", 3,
            {"abxcd", "baxdc", "abxdc", "baxcd", "abbaxdccd", "abbaxcddc"}},
        {"multi-byte fixed shells", R"((?<S>ab(?&S)cde|x))", "abcdex", 2,
            {"abxcde", "ababxcdecde", "abxcd", "abxcdee", "aabxcde"}},
        {"character set shells", R"((?<S>[ab](?&S)[cd]|x))", "abcdx", 3,
            {"aaxdd", "bbxcc", "abxcd", "abxdc"}},
        {"same-byte shells", R"((?<S>a(?&S)a|))", "ab", 6, {}},
        {"palindrome odd and even bases", R"((?<S>a(?&S)a|b(?&S)b|a|b|))", "ab", 6, {}},
        {"nullable finite base", R"((?<S>a(?&S)b|x?))", "abx", 4, {"aaxbb"}},
        {"finite base widths", R"((?<S>a(?&S)b|x{0,3}))", "abx", 4,
            {"aaxxxbb", "aaxxxxbb", "axxb"}},
        {"exact-repeat shell", R"((?<S>a{2}(?&S)b{3}|))", "ab", 6,
            {"aaaabbbbbb", "aaaabbbbb", "aaabbbbbb"}},
        {"bounded-repeat shell", R"((?<S>a{1,2}(?&S)b|))", "ab", 6, {}},
        {"optional shell", R"((?<S>a?(?&S)b|x))", "abx", 4, {"xbbb", "aaxbbb"}},
        {"nullable repeat zero", R"((?<S>(?:a?){0}a(?&S)b|))", "ab", 5, {}},
        {"shared FIRST base conflict", R"((?<S>a(?&S)b|aa|))", "ab", 6,
            {"aaaabb", "aaaaabbb"}},
        {"base shell exact overlap", R"((?<S>a(?&S)b|ab|x))", "abx", 4,
            {"aabb", "aaxbb", "axxb"}},
        {"longer same-prefix base", R"((?<S>a(?&S)b|aab|))", "ab", 6, {}},
        {"mutual productive recursion", R"((?(DEFINE)(?<S>a(?&T)b|)(?<T>c(?&S)d|))(?&S))", "abcd", 4,
            {"acdb", "acacdbdb", "acacddbb", "acbdb"}},
        {"mutual one-sided progress", R"((?(DEFINE)(?<S>(?&T)|x)(?<T>a(?&S)b))(?&S))", "abx", 4,
            {"aaxbb", "aaxb"}},
        {"unproductive self cycle", R"((?<S>(?&S)|))", "ab", 3, {}},
        {"unproductive mutual cycle", R"((?(DEFINE)(?<S>(?&T)|x)(?<T>(?&S)|))(?&S))", "abx", 3, {}},
        {"one-sided right", R"((?<S>a(?&S)|x))", "abx", 4, {}},
        {"one-sided left", R"((?<S>(?&S)b|x))", "abx", 4, {}},
        {"two recursive children", R"((?<S>a(?&S)b(?&S)|))", "ab", 6, {}},
        {"recursive repetition", R"((?<S>a(?&S){0,2}b|))", "ab", 5, {}},
        {"unbounded regular shell", R"((?<S>a*(?&S)b|x))", "abx", 4, {}},
        {"unbounded regular base", R"((?<S>a(?&S)b|x*))", "abx", 4,
            {"aaxxxxxbb"}},
        {"root wrapper", R"(xy(?<S>a(?&S)b|)zw)", "abxyzw", 2,
            {"xyzw", "xyabzw", "xyaabbzw", "xyabz", "xyazwb"}},
        {"whole-pattern recursion", R"(a(?R)b|)", "ab", 5, {"aaaabbbb"}},
        {"unused cycle definition", R"((?<S>a(?&S)b|)(?(DEFINE)(?<T>(?&T)|)))", "ab", 4, {}},
        {"two root demands", R"((?<S>a(?&S)b|)(?&S))", "ab", 5,
            {"aabbab", "abaabb", "aaabbb"}},
        {"same producer constrained and open", R"((?(DEFINE)(?<S>a(?&S)b|))(?&S)|(?&S)a|a(?&S))", "ab", 5, {}},
        {"binary bytes", R"((?<S>[\x00\xff](?&S)a|))", std::string("a\0\xff", 3), 4,
            {std::string("\0\xff" "aa", 4)}},
    };
}

OccurrenceMatchOptions unlimited() {
    OccurrenceMatchOptions options;
    // These comparisons measure the original whole-root spine tier alone.
    options.regional_execution = options.regional_constraints = false;
    // Preserve this historical representation under the newer root tiers.
    options.bounded_word_prediction=options.deterministic_programs=false;
    options.predictive_exact_rims = false; // Independent layer, covered in test_exact_rims.cpp.
    options.max_states = 4'000'000;
    options.max_steps = 32'000'000;
    return options;
}

void compare_fixture(const Fixture& fixture) {
    const auto ast = parse(fixture.pattern);
    detail::AstMachineProgram program(ast);
    auto on = unlimited(), old = on, native = on, spine_only = on;
    old.exact_end_spines = false;
    native.exact_end_spines = native.predictive_recursion = false;
    spine_only.predictive_recursion = false;
    auto subjects = ref::words(fixture.alphabet, fixture.maximum);
    subjects.insert(subjects.end(), fixture.extra.begin(), fixture.extra.end());
#ifdef CRANK_SPINE_WITH_MARPA
    std::unique_ptr<comparison::MarpaProgram> marpa;
    try { marpa = std::make_unique<comparison::MarpaProgram>(ast); }
    catch (const comparison::CompileRejected&) { ++marpa_compile_declines; }
#endif
    for (const auto& subject : subjects) {
        ++inputs;
        context = std::string(fixture.name) + " input=" + ref::escaped(subject);
        const auto relation = ref::language(ast, subject);
        ++relation_queries;
        const bool expected = relation.nodes[ast.root].at(0, subject.size());
        OccurrenceStatistics stats;
        require(program.full_match(subject, native) == expected, "original native / finite-relation disagreement");
        ++native_queries;
        require(program.full_match(subject, old) == expected, "previous predictor / finite-relation disagreement");
        require(program.full_match(subject, on, &stats) == expected, "spine / finite-relation disagreement");
        activations += stats.spine_attempts;
        require(!stats.limit_reached, "completed answer retained limit flag");
        require(!stats.proof_nodes, "Boolean answer retained proof history");
        require(program.full_match(subject, spine_only) == expected, "spine depended on predictor");
#ifdef CRANK_SPINE_WITH_MARPA
        if (marpa) {
            require(marpa->run(subject).matched == expected, "Libmarpa / finite-relation disagreement");
            ++marpa_queries;
        }
#endif
        if (subject.size() <= 3) {
            for (std::size_t start = 0; start <= subject.size(); ++start) {
                std::vector<std::size_t> ends;
                for (auto end = start; end <= subject.size(); ++end)
                    if (relation.nodes[ast.root].at(start, end)) ends.push_back(end);
                require(program.endpoints(subject, start, on, &stats) == ends, "endpoint relation changed");
                require(!stats.spine_attempts, "Boolean spine entered endpoint export");
            }
            require(program.first_match(subject, on, &stats) == program.first_match(subject, old),
                    "first witness/capture identity changed");
            require(!stats.spine_attempts, "Boolean spine entered witnessed query");
            for (NodeId entry = 0; entry < ast.nodes.size(); ++entry) {
                require(program.full_match_node(entry, subject, on, &stats) ==
                        relation.nodes[entry].at(0, subject.size()), "nonroot exact-demand relation changed");
                if (entry != ast.root) require(!stats.spine_attempts, "whole-root certificate applied to another entry");
            }
            std::vector<NodeId> entries;
            bool any = false;
            for (std::size_t capture = 0; capture < ast.capture_nodes.size(); ++capture)
                if (ast.capture_nodes[capture] != no_id) {
                    const auto entry = ast.capture_nodes[capture];
                    entries.push_back(entry); any |= relation.nodes[entry].at(0, subject.size());
                }
            require(program.full_match_any(entries, subject, on, &stats) == any, "entry union changed");
            require(!stats.spine_attempts, "whole-root certificate applied to entry union");
            for (unsigned mode = 0; mode < 5; ++mode) {
                auto option = on;
                if (mode == 0) option.prune = false;
                if (mode == 1) option.collapse_control = option.merge_equivalent_nodes = false;
                if (mode == 2) option.compress_returns = false;
                if (mode == 3) option.local_regular_regions = false;
                if (mode == 4) option.predictive_continuation_runs = false;
                require(program.full_match(subject, option) == expected, "independent option ablation changed language");
            }
        }
    }
}

void depth_and_correlations() {
    context = "deep heterogeneous spine";
    const detail::AstMachineProgram program(parse(R"((?<S>a(?&S)c|b(?&S)d|e))"));
    auto on = unlimited(), off = on;
    off.exact_end_spines = false;
    std::optional<std::size_t> states, slots, memo;
    for (std::size_t depth : {1U, 2U, 64U, 1024U, 16384U, 65536U}) {
        std::string left, right;
        for (std::size_t i = 0; i < depth; ++i) {
            left += i % 2 ? 'b' : 'a';
            right += i % 2 ? 'd' : 'c';
        }
        std::reverse(right.begin(), right.end());
        const auto input = left + 'e' + right;
        OccurrenceStatistics current, previous;
        require(program.full_match(input, on, &current), "valid alternating spine rejected");
        require(program.full_match(input, off, &previous), "previous predictor rejected deep input");
        require(current.spine_attempts == 1, "deep fixture failed to activate spine");
        require(current.max_spine_state_slots <= 8, "spine retained a depth-sized structural state");
        require(!current.predictive_frames && !current.return_links && !current.proof_nodes,
                "spine retained continuations or proofs");
        if (states) {
            require(*states == current.states, "allocated state grows with depth");
            require(*slots == current.max_spine_state_slots, "live structural slots grow with depth");
            require(*memo == current.memo_entries, "memo entries grow with depth");
        } else {
            states = current.states; slots = current.max_spine_state_slots; memo = current.memo_entries;
        }
        for (std::size_t offset : {depth, depth + 1, input.size() - 1}) {
            auto invalid = input;
            invalid[offset] = invalid[offset] == 'c' ? 'd' : 'c';
            require(!program.full_match(invalid, on), "cross-arm boundary mismatch accepted");
        }
        std::cout << "depth=" << depth << " states=" << current.states
                  << " slots=" << current.max_spine_state_slots
                  << " steps=" << current.work_steps
                  << " prior_states=" << previous.states << '\n';
    }
}

void quotas_and_cancellation() {
    const std::vector<std::pair<std::string, std::vector<std::string>>> fixtures{
        {R"((?<S>a(?&S)b|))", {"", "ab", "aabb", "abab", "aaabbb", "aaabba"}},
        {R"((?<S>a(?&S)c|b(?&S)d|e))", {"e", "aec", "abedc", "abecd", "abc"}},
        {R"(x{100}(?<S>a(?&S)b|))", {std::string(100, 'x') + "aabb", std::string(100, 'x') + "abab"}},
    };
    for (const auto& [pattern, subjects] : fixtures) {
        const detail::AstMachineProgram program(parse(pattern));
        for (const auto& subject : subjects) {
            context = "quota pattern=" + pattern + " input=" + ref::escaped(subject);
            for (bool predictor : {false, true}) {
                auto previous = unlimited(); previous.exact_end_spines = false;
                previous.predictive_recursion = predictor;
                OccurrenceStatistics baseline;
                const bool truth = program.full_match(subject, previous, &baseline);
                const std::vector<std::size_t> state_budgets{0, 1, 2, 3, 4, 8, 16, 32, baseline.states};
                const std::vector<std::size_t> step_budgets{0, 1, 2, 3, 4, 8, 16, 32, 64, 128, baseline.work_steps};
                for (const auto states : state_budgets) for (const auto steps : step_budgets) {
                    auto old = previous; old.max_states = states; old.max_steps = steps;
                    auto on = old; on.exact_end_spines = true;
                    std::optional<bool> reference;
                    try { reference = program.full_match(subject, old); } catch (const ResourceLimit&) {}
                    OccurrenceStatistics stats;
                    try {
                        require(program.full_match(subject, on, &stats) == truth, "quota returned an incorrect Boolean");
                        ++quota_completions;
                    } catch (const ResourceLimit&) {
                        require(!reference.has_value(), "new optional acceleration lost a prior successful allowance");
                    }
                    require(stats.native_spine_fallbacks <= 1, "spine fallback repeated");
                    require(stats.native_acceleration_fallbacks == stats.native_regional_fallbacks + stats.native_spine_fallbacks +
                            stats.native_predictive_rim_fallbacks +
                            stats.native_predictive_fallbacks + stats.native_consumer_fallbacks +
                            stats.native_frontier_fallbacks + stats.native_ordinary_fallbacks,
                            "fallback tier counters do not sum");
                    spine_fallbacks += stats.native_spine_fallbacks;
                }
            }
        }
    }
    context = "cooperative cancellation";
    auto ast = parse(R"((?<S>a(?&S)b|))");
    const detail::AstMachineProgram program(ast);
    std::atomic_bool cancel{true};
    bool cancelled = false;
    try { (void)program.full_match_node(ast.root, "aabb", unlimited(), nullptr, &cancel); }
    catch (const detail::AstExecutionCancelled&) { cancelled = true; }
    require(cancelled, "cancelled spine returned a Boolean");
}

void stateful_exclusions() {
    context = "stateful eligibility exclusions";
    for (const auto pattern : {R"((?<S>a(?&S)b|)(x)\2)", R"((?<S>a(?&S)b|)(?<X>x)\k<X>)"}) {
        const auto ast = parse(pattern);
        require(!detail::AstMachineProgram::eligible(ast), "observed backreference admitted to pure spine");
    }
    auto ast = parse(R"((?<S>a(?&S)b|))");
    // Public summary flags cannot hide a real observable effect.
    ast.has_counts = ast.has_references = ast.has_calls = false;
    ast.nodes[ast.root].bind_count = 0;
    require(!detail::AstMachineProgram::eligible(ast), "hidden count effect admitted to pure spine");
    bool rejected = false;
    try { detail::AstMachineProgram forbidden(ast); } catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "effectful AST constructor did not reject");
}
} // namespace

int main() {
    try {
        const auto all = fixtures();
        for (const auto& fixture : all) compare_fixture(fixture);
        require(activations > 0, "spine never activated during differential coverage");
        depth_and_correlations();
        quotas_and_cancellation();
        stateful_exclusions();
        std::cout << "{\"status\":\"passed\",\"fixtures\":" << all.size()
                  << ",\"inputs\":" << inputs << ",\"checks\":" << checks
                  << ",\"finite_relation_queries\":" << relation_queries
                  << ",\"original_native_queries\":" << native_queries
                  << ",\"spine_activations\":" << activations
                  << ",\"quota_completions\":" << quota_completions
                  << ",\"spine_fallbacks\":" << spine_fallbacks
                  << ",\"marpa_queries\":" << marpa_queries
                  << ",\"marpa_compile_declines\":" << marpa_compile_declines << "}\n";
    } catch (const std::exception& e) {
        std::cerr << "SPINE FAILURE: " << e.what() << '\n'; return 1;
    }
}
