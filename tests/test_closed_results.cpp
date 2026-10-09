#include "ast_machine.hpp"
#include <iostream>
#include <stdexcept>

using namespace crank;
using namespace crank::detail;

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

int main() {
    const std::vector<std::string> patterns{
        "(?<S>(?&S)(?&S)|a)", "(?<S>(?&S)(?&S)|a|)",
        "(?<S>(?&S)a|)", "(?<S>a(?&S)b|)", "(?<S>(?&S)|)",
        "(?(DEFINE)(?<X>a|aa))(?&X)a|a(?&X)b",
        "(?(DEFINE)(?<A>(?&B)|a)(?<B>(?&A)|))(?&A)",
        "(?(DEFINE)(?<A>a*))(?&A)b|(?&A)ab",
        "(?<S>aa*(?&S)?b)|a", "((?<S>(?&S)(?&S)|a)){2,4}"
    };
    std::vector<std::string> words{""};
    for (std::size_t i=0; i<words.size(); ++i)
        if (words[i].size()<4) for (const char c : {'a','b'}) words.push_back(words[i]+c);
    std::size_t checks=0, reused=0;
    auto check_stats = [&](const OccurrenceStatistics& old, const OccurrenceStatistics& now) {
        require(now.native_result_slots+now.native_reused_result_slots==now.completions,
                "logical/physical result accounting");
        require(old.native_result_slots==old.completions && old.native_reused_result_slots==0,
                "materialized accounting");
        require(old.states-now.states==now.native_reused_result_slots,
                "state saving must equal physically omitted result pushes");
        require(old.work_steps==now.work_steps && old.completions==now.completions &&
                old.memo_entries==now.memo_entries && old.return_links==now.return_links &&
                old.native_agenda_tasks==now.native_agenda_tasks && old.replay_steps==now.replay_steps,
                "result storage changed semantic execution");
        reused += now.native_reused_result_slots;
    };
    for (const auto& pattern : patterns) {
        const AstMachineProgram program(parse(pattern));
        auto subjects=words;
        subjects.push_back(std::string(32,'a'));
        for (const auto& subject : subjects) for (unsigned mode=0; mode<8; ++mode) {
            OccurrenceMatchOptions off,on;
            // Isolate storage reuse from newer reductions in semantic work.
            off.regional_execution=off.regional_constraints=on.regional_execution=on.regional_constraints=false;
            // Preserve this historical representation under the newer root tiers.
            off.bounded_word_prediction=off.deterministic_programs=false;on.bounded_word_prediction=on.deterministic_programs=false;
            off.predictive_recursion=on.predictive_recursion=false;
            off.endpoint_consumer_guards=on.endpoint_consumer_guards=false;
            off.max_steps=on.max_steps=32000000;
            off.coalesce_returns=on.coalesce_returns=!(mode&1);
            off.compress_returns=on.compress_returns=!(mode&2);
            off.local_regular_regions=on.local_regular_regions=!(mode&4);
            off.compact_closed_results=false;
            on.compact_closed_results=true;
            OccurrenceStatistics a,b;
            require(program.full_match(subject,off,&a)==program.full_match(subject,on,&b),
                    "Boolean result changed");
            check_stats(a,b); ++checks;
            for (std::size_t start=0;start<=subject.size();++start) {
                require(program.endpoints(subject,start,off,&a)==program.endpoints(subject,start,on,&b),
                        "root endpoint export changed");
                check_stats(a,b); ++checks;
            }
            // Reuse must remain off for richer witness execution.
            const auto x=program.first_match(subject,off,&a), y=program.first_match(subject,on,&b);
            require(x.has_value()==y.has_value(),"witness existence changed");
            require(!b.native_reused_result_slots && a.states==b.states && a.work_steps==b.work_steps,
                    "witness incorrectly reused slots");
            if(x) {
                require(x->end==y->end && x->counts==y->counts && x->captures.size()==y->captures.size(),
                        "witness output changed");
                for(std::size_t i=0;i<x->captures.size();++i) {
                    const auto u=x->captures[i],v=y->captures[i];
                    require(u.has_value()==v.has_value() && (!u || (u->begin==v->begin && u->end==v->end)),
                            "witness capture changed");
                }
            }
            ++checks;
        }
    }
    require(reused>0,"new mechanism was not exercised");
    std::cout << "{\"checks\":" << checks << ",\"reused_results\":" << reused
              << ",\"status\":\"pass\"}\n";
}
