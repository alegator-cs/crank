#include "ast_machine.hpp"
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
using namespace crank;
std::size_t checks=0;
void require(bool p,const char*m){++checks;if(!p)throw std::runtime_error(m);}
void same(const OccurrenceStatistics&a,const OccurrenceStatistics&b){
    require(a.work_steps==b.work_steps,"work differs");
    require(a.memo_entries==b.memo_entries,"requests differ");
    require(a.return_links==b.return_links,"returns differ");
    require(a.completions==b.completions,"results differ");
    require(a.down_steps==b.down_steps,"Down work differs");
    require(a.up_steps==b.up_steps,"Up work differs");
    require(a.max_pending==b.max_pending,"pending accounting differs");
    require(a.replay_steps==b.replay_steps,"replay differs");
    require(a.compression_hops==b.compression_hops,"compression differs");
    require(a.coalesced_returns==b.coalesced_returns,"coalescing differs");
    require(a.duplicate_completions==b.duplicate_completions,"dedup differs");
    require(a.regular_region_requests==b.regular_region_requests,"regions differ");
    require(a.native_acceleration_fallbacks==0&&b.native_acceleration_fallbacks==0,"unexpected fallback");
    require(a.states-b.states==a.native_agenda_tasks-b.native_agenda_tasks,"state reduction differs from deleted objects");
    require(a.native_agenda_tasks>=b.native_agenda_tasks,"more agenda allocations");
}
int main(){
    const std::vector<std::string> patterns={
        "", "a", "a*", "a*ab", "(a|b)*", "((a|b)*(b|a)*)*", "(a?){0,5}",
        "(?<S>(?&S)(?&S)|a)", "(?<S>(?&S)(?&S)|a|)", "(?<S>(?&S)a|)",
        "(?<S>a(?&S)|)", "(?<S>a(?&S)b|)", "(?<S>(?&S)|a)",
        "(?(DEFINE)(?<S>(?&S)))(?&S)", "(?(DEFINE)(?<S>(?&T)|a)(?<T>(?&S)|))(?&S)",
        "(?<S>(?:a*b*)?[(](?&S)[)]a*|a*b*)", "(?<S>[(](?&S)[)]|a{0,4}b*)", 
        "(?(DEFINE)(?<A>a|)(?<B>(?&A)b|a(?&A)))(?&B)*", "(?<S>a(?&S)?b)*",
        "(?<S>(?&S)(?&S)|a|b){0,3}", "(a|aa|aaa)*b?", "(?<S>a(?&S)|b(?&S)|a*b*)"
    };
    std::vector<std::string> subjects{"", "()", "(())", "a(a)b", "aaaabbbb"};
    for(unsigned len=1;len<=6;++len) for(unsigned bits=0;bits<(1u<<len);++bits){std::string s(len,'a');for(unsigned i=0;i<len;++i)if(bits&(1u<<i))s[i]='b';subjects.push_back(s);}
    for(const auto&pattern:patterns){
      detail::AstMachineProgram p(parse(pattern));
      for(unsigned mode=0;mode<8;++mode){
        OccurrenceMatchOptions before;before.max_states=8'000'000;before.max_steps=32'000'000;before.implicit_request_agenda=false;
        // Preserve the request populations whose physical Down records differ.
        before.regional_execution=before.regional_constraints=false;
        // Preserve this historical representation under the newer root tiers.
        before.bounded_word_prediction=before.deterministic_programs=false;
        before.predictive_recursion=false;before.endpoint_consumer_guards=false;
        before.local_regular_regions=(mode&1)==0;before.coalesce_returns=(mode&2)==0;before.prune=(mode&4)==0;
        auto after=before;after.implicit_request_agenda=true;
        for(const auto&s:subjects){
          OccurrenceStatistics a,b;
          require(p.full_match(s,before,&a)==p.full_match(s,after,&b),"Boolean mismatch");same(a,b);
          require(a.states-b.states==a.memo_entries,"Boolean must delete exactly one Down record per request");
          auto exact=after;exact.max_states=a.states;exact.max_steps=a.work_steps;
          require(p.full_match(s,exact)==p.full_match(s,before),"ordinary exact budget lost");
          for(size_t start=0;start<=s.size();++start){
            require(p.endpoints(s,start,before,&a)==p.endpoints(s,start,after,&b),"endpoint mismatch");same(a,b);
            require(a.states-b.states==a.memo_entries,"endpoint must delete one Down record per request");
          }
        }
      }
      auto before=OccurrenceMatchOptions{};before.implicit_request_agenda=false;auto after=before;after.implicit_request_agenda=true;
      for(const auto&s:subjects){OccurrenceStatistics a,b;const auto oa=p.first_match(s,before,&a),ob=p.first_match(s,after,&b);require(oa.has_value()==ob.has_value(),"witness presence");if(oa)require(oa->captures==ob->captures,"witness captures");require(a.states==b.states&&a.work_steps==b.work_steps&&b.implicit_down_steps==0,"witness scheduler changed");}
      before.compress_returns=false;after=before;after.implicit_request_agenda=true;
      for(const auto&s:subjects){OccurrenceStatistics a,b;require(p.endpoints(s,0,before,&a)==p.endpoints(s,0,after,&b),"unordered mismatch");same(a,b);require(a.states==b.states&&b.implicit_down_steps==0,"unordered scheduler changed");}
    }
    std::cout<<"implicit request agenda: "<<checks<<" checks passed\n";
}
