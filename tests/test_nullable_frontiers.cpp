#include "ast_machine.hpp"
#include "helpers/test_progress.hpp"
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
using namespace crank;
std::size_t checks=0, activations=0, fallbacks=0;
void require(bool p,const char*m){++checks;if(!p)throw std::runtime_error(m);}

std::string query_detail(const crank_test::Progress& progress, const std::string& pattern,
                         const std::string& subject, unsigned mode, std::size_t start,
                         const OccurrenceMatchOptions& options) {
 if(!progress.enabled()) return {};
 std::ostringstream out;
 out << "expression=\"" << pattern << "\" input=\"" << subject << "\" bytes=" << subject.size()
     << " mode=" << mode << " start=" << start
     << " max_states=" << options.max_states << " max_steps=" << options.max_steps
     << " max_occurrences=" << options.max_occurrences << " max_repeat=" << options.max_repeat
     << " max_call_depth=" << options.max_call_depth
     << " nullable=" << options.inline_nullable_frontiers
     << " prune=" << options.prune << " collapse_control=" << options.collapse_control
     << " local_regular_regions=" << options.local_regular_regions
     << " compress_returns=" << options.compress_returns;
 return out.str();
}

detail::AstMachineProgram prepare_program(crank_test::Progress& progress,
                                         const std::string& pattern) {
 const auto setup_detail=progress.enabled()?"expression=\""+pattern+"\"":std::string{};
 const auto ast=progress.call("parse",setup_detail,[&]{return parse(pattern);});
 return progress.call("program.setup",setup_detail,[&]{return detail::AstMachineProgram(ast);},true);
}

int main(){
 crank_test::Progress progress("test_nullable_frontiers");
 const std::vector<std::string> patterns={
   "", "a*", "a?b", "a*b*", "a*ab", "(a|)*b", "(a?|b?)*a", "(a?){4,8}b?", "(a*){3}b",
   "(?<S>(?&S)(?&S)|a|)", "(?<S>(?&S)|)", "(?<S>(?&S)a|)", "(?<S>a(?&S)|)",
   "(?<S>a(?&S)b|)", "(?<S>(?&S)b|a?)", "(?<S>(?&S)(?&S)|a|b)*",
   "(?(DEFINE)(?<S>(?&T)|)(?<T>(?&S)))(?&S)b?", "(?(DEFINE)(?<S>(?&T)|a)(?<T>(?&S)|))(?&S)",
   "(?(DEFINE)(?<A>a*)(?<B>b*))(?&A)(?&B)", "(?<S>[(](?&S)[)]|a*)b*",
   "(?<S>a*(?&S)b|a*)", "(?<S>(?&S)a*|b*)", "(a?){64}b*", "(a?){0,0}b",
   "(?(DEFINE)(?<X>a|))a(?&X)c|(?&X)b", "(?<S>(?:a*b*)?[(](?&S)[)]a*|a*b*)"
 };
 std::vector<std::string> subjects{"", "()", "(())", "a(a)b", "abc", "aab", "aaaaabbbbb"};
 for(unsigned len=1;len<=7;++len) for(unsigned bits=0;bits<(1u<<len);++bits){std::string s(len,'a');for(unsigned i=0;i<len;++i)if(bits&(1u<<i))s[i]='b';subjects.push_back(s);}
 for(const auto&pattern:patterns){
   auto p=prepare_program(progress,pattern);
   for(unsigned mode=0;mode<8;++mode){
     OccurrenceMatchOptions before;before.inline_nullable_frontiers=false;before.max_states=8'000'000;before.max_steps=32'000'000;
     // Keep the epsilon-frontier ablation on the original request/return path.
     before.regional_execution=before.regional_constraints=false;
     // Preserve this historical representation under the newer root tiers.
     before.bounded_word_prediction=before.deterministic_programs=false;
     before.predictive_recursion=false;before.endpoint_consumer_guards=false;
     before.local_regular_regions=(mode&1)==0;before.prune=(mode&2)==0;before.collapse_control=(mode&4)==0;
     auto after=before;after.inline_nullable_frontiers=true;
     for(const auto&s:subjects){
       auto test_case=progress.scope("case",query_detail(progress,pattern,s,mode,0,before));
       OccurrenceStatistics a,b;
       const auto expected=progress.call("boolean.ablation",query_detail(progress,pattern,s,mode,0,before),[&]{return p.full_match(s,before,&a);});
       require(expected==progress.call("boolean.nullable",query_detail(progress,pattern,s,mode,0,after),[&]{return p.full_match(s,after,&b);}),"Boolean mismatch");activations+=b.inline_nullable_transitions;
       auto exact=after;exact.max_states=a.states;exact.max_steps=a.work_steps;
       require(expected==progress.call("boolean.exact_budget",query_detail(progress,pattern,s,mode,0,exact),[&]{return p.full_match(s,exact,&b);}),"ordinary successful Boolean cap lost");fallbacks+=b.native_acceleration_fallbacks;
       for(size_t start=0;start<=s.size();++start){
         const auto endpoints=progress.call("endpoints.ablation",query_detail(progress,pattern,s,mode,start,before),[&]{return p.endpoints(s,start,before,&a);});
         require(endpoints==progress.call("endpoints.nullable",query_detail(progress,pattern,s,mode,start,after),[&]{return p.endpoints(s,start,after,&b);}),"endpoint mismatch");activations+=b.inline_nullable_transitions;
         exact.max_states=a.states;exact.max_steps=a.work_steps;
         require(endpoints==progress.call("endpoints.exact_budget",query_detail(progress,pattern,s,mode,start,exact),[&]{return p.endpoints(s,start,exact,&b);}),"ordinary successful endpoint cap lost");fallbacks+=b.native_acceleration_fallbacks;
       }
       test_case.finish();
     }
   }
   auto before=OccurrenceMatchOptions{};before.inline_nullable_frontiers=false;auto after=before;after.inline_nullable_frontiers=true;
   for(const auto&s:subjects){
     auto test_case=progress.scope("case",query_detail(progress,pattern,s,8,0,before));
     OccurrenceStatistics a,b;
     const auto oa=progress.call("witness.ablation",query_detail(progress,pattern,s,8,0,before),[&]{return p.first_match(s,before,&a);});
     const auto ob=progress.call("witness.nullable",query_detail(progress,pattern,s,8,0,after),[&]{return p.first_match(s,after,&b);});
     require(oa.has_value()==ob.has_value(),"witness presence");if(oa)require(oa->captures==ob->captures,"witness captures");require(a.states==b.states&&a.work_steps==b.work_steps&&b.inline_nullable_transitions==0,"witness path changed");
     test_case.finish();
   }
   before.compress_returns=false;after=before;after.inline_nullable_frontiers=true;
   for(const auto&s:subjects){
     auto test_case=progress.scope("case",query_detail(progress,pattern,s,9,0,before));
     OccurrenceStatistics a,b;
     const auto ea=progress.call("endpoints.uncompressed.ablation",query_detail(progress,pattern,s,9,0,before),[&]{return p.endpoints(s,0,before,&a);});
     const auto eb=progress.call("endpoints.uncompressed.nullable",query_detail(progress,pattern,s,9,0,after),[&]{return p.endpoints(s,0,after,&b);});
     require(ea==eb,"unordered mismatch");require(a.states==b.states&&a.work_steps==b.work_steps&&b.inline_nullable_transitions==0,"unordered path changed");
     test_case.finish();
   }
 }
 require(activations>10000,"feature never activated");
 std::cout<<"nullable frontier: "<<checks<<" checks passed, "<<activations<<" activations, "<<fallbacks<<" exact-budget fallbacks\n";
}
