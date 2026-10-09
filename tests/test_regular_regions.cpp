#include "ast_machine.hpp"
#include "regular_regions.hpp"
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace crank;
namespace { std::size_t checks = 0, fallbacks = 0;
void require(bool v,const char* why){++checks;if(!v)throw std::runtime_error(why);}
std::vector<std::string> words(std::string alphabet,unsigned n){std::vector<std::string> out{""};std::size_t first=0,last=1;for(unsigned i=0;i<n;++i){for(auto k=first;k<last;++k)for(char b:alphabet)out.push_back(out[k]+b);first=last;last=out.size();}return out;}
}
int main(){
 const std::vector<std::string> patterns{
  R"((?<S>a(?&S)?b|ab|aab))",
  R"((?<S>a?(?&S)b?|c[ab]*c|[ab]+))",
  R"((?<S>(?&S)(?:ab|ba)|[ab]{0,3}))",
  R"((?<S>(?&S)(?&S)|a(?:b|a)?))",
  R"((?<S>(?&S)|a*b*|(?:ab)*))",
  R"((?(DEFINE)(?<R>a(?&R)?b)(?<T>a?|ab))(?&T)(?&R)?(?&T))",
  R"((?(DEFINE)(?<R>a(?&R)?b))a?b?(?&R)?a?b?)",
  R"((?(DEFINE)(?<S>a(?&T)?b)(?<T>b(?&S)?a))(?&S)|a*b*)",
  R"((?<S>a(?&S)?b|[ab]{2,5}|(?:ab){1,3}))",
  R"((?<S>(?:a?b?){2,3}(?&S)?a?b?))",
  R"((?(DEFINE)(?<R>a(?&R)?b))a[ab]*b(?&R)?ba*)",
 };
 const auto subjects=words("abc",4);
 for(const auto& pattern:patterns){
  const auto ast=parse(pattern);detail::AstMachineProgram p(ast);
  OccurrenceMatchOptions off,on;off.local_regular_regions=false;
  off.regional_execution=off.regional_constraints=on.regional_execution=on.regional_constraints=false;
  // Preserve this historical representation under the newer root tiers.
  off.bounded_word_prediction=off.deterministic_programs=false;on.bounded_word_prediction=on.deterministic_programs=false;
  // Isolate this older local-region layer: nullable-frontier and frozen-word
  // accelerations have their own combined exact-budget gate. A mixed newer
  // control is not the ordinary fallback whose exact charges are asserted here.
  off.inline_nullable_frontiers=on.inline_nullable_frontiers=false;
  off.aggregate_interval_runs=on.aggregate_interval_runs=false;
  off.aggregate_prefix_blocks=on.aggregate_prefix_blocks=false;
  // These controls measure local cursors, not newer demand representations.
  off.predictive_recursion=on.predictive_recursion=false;
  off.endpoint_consumer_guards=on.endpoint_consumer_guards=false;
  off.aggregate_returns=off.aggregate_suffix_returns=false;
  on.aggregate_returns=on.aggregate_suffix_returns=false;
  require(p.regular_region_statistics().programs>0,"region admission");
  for(const auto& input:subjects){
   OccurrenceStatistics reference,got;
   const auto expected=p.full_match(input,off,&reference);
   require(p.full_match(input,on,&got)==expected,"Boolean differential");
   for(std::size_t start=0;start<=input.size();++start)
    require(p.endpoints(input,start,on)==p.endpoints(input,start,off),"endpoint differential");
   // Every ordinary resolvable query remains resolvable at exactly its budgets.
   auto tight=on;tight.max_states=reference.states;tight.max_steps=reference.work_steps;
   require(p.full_match(input,tight,&got)==expected,"budget preservation");
   fallbacks+=got.native_acceleration_fallbacks;
   if(got.native_acceleration_fallbacks){
    require(got.states==reference.states&&got.work_steps==reference.work_steps,"fallback charges preserve ordinary run");
    require(got.native_acceleration_discarded_states>0&&got.native_acceleration_discarded_steps>0,"abandoned charges exposed");
   }
   if(input.size()<=2){
    const auto ends=p.endpoints(input,0,off,&reference);
    auto tight_ends=on;tight_ends.max_states=reference.states;tight_ends.max_steps=reference.work_steps;
    require(p.endpoints(input,0,tight_ends)==ends,"endpoint export budget preservation");
   }
   for(const auto mode:{0,1,2}){auto o=on;
    if(mode==0) o.compress_returns=false;
    if(mode==1) o.coalesce_returns=false;
    if(mode==2) o.collapse_control=false;
    require(p.full_match(input,o,&got)==expected,"scheduler/control ablation");
    if(mode<2)require(!got.regular_region_requests,"ordered/coalesced gate");
   }
   auto task_mode=on;task_mode.regular_cursor_frontiers=false;
   require(p.full_match(input,task_mode)==expected,"cursor representation ablation");
   const auto a=p.first_match(input,on,&got),b=p.first_match(input,off);
   require(bool(a)==bool(b),"witness Boolean");
   if(a)require(a->captures==b->captures,"witness capture identity");
   require(!got.regular_region_requests,"witness gate");
  }
 }
 // Explicit entry is independent of the AST root and (?R) keeps its target.
 const auto ast=parse(R"((?<S>a(?&S)?b|[ab]*))");detail::AstMachineProgram p(ast);
 OccurrenceMatchOptions off,on;off.local_regular_regions=false;
  off.regional_execution=off.regional_constraints=on.regional_execution=on.regional_constraints=false;
  // Preserve this historical representation under the newer root tiers.
  off.bounded_word_prediction=off.deterministic_programs=false;on.bounded_word_prediction=on.deterministic_programs=false;
  // Isolate this older local-region layer: nullable-frontier and frozen-word
  // accelerations have their own combined exact-budget gate. A mixed newer
  // control is not the ordinary fallback whose exact charges are asserted here.
  off.inline_nullable_frontiers=on.inline_nullable_frontiers=false;
  off.aggregate_interval_runs=on.aggregate_interval_runs=false;
  off.aggregate_prefix_blocks=on.aggregate_prefix_blocks=false;
  off.predictive_recursion=on.predictive_recursion=false;
  off.endpoint_consumer_guards=on.endpoint_consumer_guards=false;
  off.aggregate_returns=off.aggregate_suffix_returns=false;
  on.aggregate_returns=on.aggregate_suffix_returns=false;
 for(const auto& subject:words("ab",3))for(NodeId node=0;node<ast.nodes.size();++node){
  require(p.full_match_node(node,subject,on,nullptr)==p.full_match_node(node,subject,off,nullptr),"entry differential");
  std::vector<NodeId> entries{node,ast.root};
  require(p.full_match_any(entries,subject,on,nullptr)==p.full_match_any(entries,subject,off,nullptr),"multi-entry differential");
 }
 // Native public Ast values ignore leaf children; the stricter regular
 // compiler must decline these candidates without rejecting native preparation.
 auto unusual=parse(R"((?<S>a(?&S)?b|ab))");
 const auto ignored=unusual.nodes.size();unusual.nodes.emplace_back();
 for(auto i=ignored;i-->0;){auto& node=unusual.nodes[i];
  if(node.kind==Kind::CharSet&&node.characters.test('a')){node.children.push_back(ignored);break;}
 }
 require(detail::AstMachineProgram::eligible(unusual),"legacy native leaf shape");
 detail::AstMachineProgram unusual_program(unusual);
 for(const auto& input:words("ab",3))
  require(unusual_program.full_match(input,on)==unusual_program.full_match(input,off),"stricter local compiler decline");
 // Finite-repeat expansion hits an independent compilation cap and declines
 // to the native nullable repetition invariant instead of consuming query work.
 detail::AstMachineProgram capped(parse(R"((?<S>a(?&S)?b|(?:a?){1000000}))"));
 require(capped.regular_region_statistics().limit_reached,"bounded compilation decline");
 require(capped.full_match("aaaa",on)==capped.full_match("aaaa",off),"declined compilation answer");
 // The source-node cap must not allocate per-node optional lookup tables.
 auto large=parse("a");large.nodes.resize(16385);
 std::vector<NodeId> identities(large.nodes.size());
 for(NodeId i=0;i<identities.size();++i)identities[i]=i;
 detail::RegularRegions declined(large,identities,identities);
 require(declined.statistics.limit_reached&&declined.programs.empty()&&declined.concatenations.empty(),"bounded empty compile storage");
 // Acyclic-only compilation remains the existing top-level specialization.
 detail::AstMachineProgram regular(parse(R"((ab|ba)*)"));
 require(regular.regular_region_statistics().programs==0,"whole regular bypass");
 std::cout<<"regular-region checks "<<checks<<", budget fallbacks "<<fallbacks<<"\n";
}
