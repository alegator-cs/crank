#include "ast_machine.hpp"
#include <iostream>
#include <stdexcept>

bool same(const crank::OccurrenceStatistics& a, const crank::OccurrenceStatistics& b) {
    #define EQ(field) if (a.field != b.field) return false;
    EQ(states) EQ(work_steps) EQ(down_steps) EQ(up_steps)
    EQ(pruned) EQ(arithmetic_pruned) EQ(path_checks) EQ(forward_obligations)
    EQ(max_call_depth) EQ(occurrences) EQ(memo_entries) EQ(memo_hits)
    EQ(return_links) EQ(completions) EQ(replay_steps) EQ(proof_nodes)
    EQ(max_pending) EQ(compression_lookups) EQ(compression_hops) EQ(compressed_completions)
    EQ(compression_cycles) EQ(control_redirects) EQ(ordered_positions) EQ(duplicate_completions)
    EQ(coalesced_returns) EQ(ordered_endpoint_checks) EQ(fanout_word_checks) EQ(fanout_links_skipped)
    EQ(scalar_repetition_clamps) EQ(scalar_iteration_bound_reduction) EQ(inline_terminal_transitions) EQ(aggregate_return_links)
    EQ(aggregate_return_words) EQ(aggregate_word_tasks) EQ(aggregate_completion_members) EQ(aggregate_word_checks)
    EQ(suffix_word_tasks) EQ(suffix_members) EQ(regular_region_requests) EQ(regular_region_transitions)
    EQ(regular_region_cursors) EQ(max_regular_cursors) EQ(regular_region_endpoints) EQ(native_acceleration_fallbacks)
    EQ(native_acceleration_discarded_states) EQ(native_acceleration_discarded_steps) EQ(limit_reached)
    EQ(native_agenda_tasks) EQ(suffix_groups_created)
    #undef EQ
    return true;
}
int main() {
    using namespace crank;
    using namespace crank::detail;
    const std::vector<std::string> patterns{
        "", "a", "a|", "(a|ab)*", "(a?){0,5}", "(a*)a?b?",
        "(?<S>a(?&S)b|)", "(?<S>(?&S)a|)", "a(?R)b|",
        "(?<S>(?&S)(?&S)|a)", "(?<S>(?&S)(?&S)|a|)",
        "(?<S>(?&S)|)",
        "(?(DEFINE)(?<A>(?&B)|a)(?<B>(?&A)|))(?&A)",
        "(?(DEFINE)(?<A>(?&B))(?<B>(?&A)|b))(?&A)",
        "(?(DEFINE)(?<A>a*))(?&A)b|(?&A)ab",
        "(?(DEFINE)(?<A>a|))(?&A)(?&A)(?&A)"
    };
    std::vector<std::string> words{""};
    for (std::size_t i=0; i<words.size(); ++i)
        if (words[i].size()<5) for (char c : {'a','b'}) words.push_back(words[i]+c);
    std::size_t checks=0;
    for (const auto& pattern : patterns) {
        const AstMachineProgram program(parse(pattern));
        for (const auto& word : words) for (bool prune : {false,true})
            for (bool coalesce : {false,true}) for (bool inline_atoms : {false,true}) {
                OccurrenceMatchOptions options;
                // Exercise frontier indexing rather than bypassing its graph.
                options.predictive_recursion=false;
                options.endpoint_consumer_guards=false;
                options.prune=prune; options.coalesce_returns=coalesce;
                options.inline_terminals=inline_atoms;
                OccurrenceStatistics old_stats, new_stats;
                options.frontier_indexes=false;
                const auto old_match=program.full_match(word,options,&old_stats);
                options.frontier_indexes=true;
                const auto new_match=program.full_match(word,options,&new_stats);
                if (old_match!=new_match || !same(old_stats,new_stats))
                    throw std::runtime_error("Boolean differential: "+pattern+" / "+word);
                ++checks;
                for (std::size_t start=0; start<=word.size(); ++start) {
                    options.frontier_indexes=false;
                    const auto old_ends=program.endpoints(word,start,options,&old_stats);
                    options.frontier_indexes=true;
                    const auto new_ends=program.endpoints(word,start,options,&new_stats);
                    if (old_ends!=new_ends || !same(old_stats,new_stats))
                        throw std::runtime_error("endpoint differential: "+pattern+" / "+word);
                    ++checks;
                }
            }
        // Witnesses and explicitly unordered native execution never enter
        // the frontier-advance block. Their counters and results coincide.
        for (const auto& word : words) {
            OccurrenceMatchOptions options;
            options.predictive_recursion=false;
            options.endpoint_consumer_guards=false;
            options.compress_returns=false;
            OccurrenceStatistics old_stats,new_stats;
            options.frontier_indexes=false;
            const auto old_match=program.full_match(word,options,&old_stats);
            options.frontier_indexes=true;
            const auto new_match=program.full_match(word,options,&new_stats);
            if (old_match!=new_match || !same(old_stats,new_stats))
                throw std::runtime_error("unordered differential");
            ++checks;
            options.frontier_indexes=false;
            options.compress_returns=true;
            const auto old_witness=program.first_match(word,options,&old_stats);
            options.frontier_indexes=true;
            const auto new_witness=program.first_match(word,options,&new_stats);
            if (old_witness.has_value()!=new_witness.has_value() || !same(old_stats,new_stats))
                throw std::runtime_error("witness differential");
            if (old_witness) {
                if (old_witness->end!=new_witness->end ||
                    old_witness->captures.size()!=new_witness->captures.size() ||
                    old_witness->counts!=new_witness->counts)
                    throw std::runtime_error("witness output differential");
                for (std::size_t i=0;i<old_witness->captures.size();++i) {
                    const auto a=old_witness->captures[i],b=new_witness->captures[i];
                    if (a.has_value()!=b.has_value() ||
                        (a && (a->begin!=b->begin || a->end!=b->end)))
                        throw std::runtime_error("witness capture differential");
                }
            }
            ++checks;
        }
    }
    std::cout << "{\"checks\":" << checks << ",\"status\":\"pass\"}\n";
    return 0;
}
