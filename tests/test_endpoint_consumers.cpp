#include "ast_machine.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>
using namespace crank;
namespace {
std::size_t checks=0, rejected=0, fallbacks=0;
void require(bool ok,const char*why){++checks;if(!ok)throw std::runtime_error(why);}
std::vector<std::string> words(std::string_view alphabet,unsigned depth){std::vector<std::string> v{""};std::size_t lo=0,hi=1;while(depth--){for(auto i=lo;i<hi;++i)for(char c:alphabet)v.push_back(v[i]+c);lo=hi;hi=v.size();}return v;}
}
int main(){
 const std::vector<std::string> patterns{
  R"((?<S>a(?&S)?b|[ab]+))", // same producer can finish at every interior endpoint
  R"((?(DEFINE)(?<L>[ab]+)(?<S>\((?&S)\)|(?&L)))((?&S)c|(?&S)b))", // distinct consumer FIRST
  R"((?(DEFINE)(?<L>[ab]+)(?<S>\((?&S)\)|(?&L)))(?&S)a?b?)", // nullable suffix and EOF
  R"((?(DEFINE)(?<L>a*b*)(?<S>(?&S)|(?&L)|c(?&S)c))(?&S))", // unproductive cycle and late epsilon
  R"((?(DEFINE)(?<L>a+b?)(?<S>(?&S)(?&S)|(?&L)))(?&S)c?)", // recursive ambiguity and late consumers
  R"((?(DEFINE)(?<S>a(?&S)?b))([ab]*(?&S)?c?){0,3})", // repetition declines
  R"((?(DEFINE)(?<S>a(?&S)?b))(?&S)?a*ab)", // greedy lexical prefix is not safe
  R"((?(DEFINE)(?<S>a(?&S)?b))a[ab]*b(?&S)?ba*)", // compiled concatenation slice
  R"((?<S>(?&S)(?:ab|ba)|[ab]{0,3}))", // left recursion and nullable regular alternation
  R"((?<S>a?(?&S)b?|c[ab]*c|[ab]+))",
  R"((?<S>a(?&S)?b|(?:ab){1,3}|[ab]{2,5}))",
  R"((?(DEFINE)(?<S>a(?&T)?b)(?<T>b(?&S)?a))(?&S)|a*b*)",
 };
 auto subjects=words("abc",4);
 for(const auto& text:patterns){auto ast=parse(text);detail::AstMachineProgram p(ast);
  for(const auto& input:subjects){
   OccurrenceMatchOptions off,on;off.exact_end_spines=on.exact_end_spines=false;off.predictive_exact_rims=on.predictive_exact_rims=false;off.endpoint_consumer_guards=false;
   // This older layer filters a fully executed producer. Keep newer producer
   // substitutions and their independent retry tier out of this comparison.
   off.regional_execution=off.regional_constraints=on.regional_execution=on.regional_constraints=false;
   // Preserve this historical representation under the newer root tiers.
   off.bounded_word_prediction=off.deterministic_programs=off.normalized_exact_programs=off.native_slr=false;on.bounded_word_prediction=on.deterministic_programs=on.normalized_exact_programs=on.native_slr=false;
   OccurrenceStatistics old,got;bool answer=p.full_match(input,off,&old);
   require(p.full_match(input,on,&got)==answer,"Boolean consumer guard");rejected+=got.endpoint_consumer_rejections;
   require(got.regular_region_transitions==old.regular_region_transitions,"guard preserves DFA transitions");
   for(std::size_t start=0;start<=input.size();++start)
    require(p.endpoints(input,start,on)==p.endpoints(input,start,off),"endpoint export and nullable replay");
   if(input.size()<3){
    auto tight=on;tight.max_states=old.states;tight.max_steps=old.work_steps;
    require(p.full_match(input,tight,&got)==answer,"exact prior allowance");fallbacks+=got.native_consumer_fallbacks;
    require(got.native_consumer_fallbacks<=1,"one consumer fallback");
    require(got.native_acceleration_fallbacks==got.native_regional_fallbacks+got.native_consumer_fallbacks+got.native_frontier_fallbacks+got.native_ordinary_fallbacks,"retry accounting");
    for(NodeId id=0;id<ast.nodes.size();++id){
     require(p.full_match_node(id,input,on,nullptr)==p.full_match_node(id,input,off,nullptr),"arbitrary entry");
     const std::vector<NodeId> entries{id,ast.root};
     require(p.full_match_any(entries,input,on,nullptr)==p.full_match_any(entries,input,off,nullptr),"multi entry and shared lexical producer");
    }
    auto a=p.first_match(input,on,&got),b=p.first_match(input,off);
    require(bool(a)==bool(b),"witness decision");if(a)require(a->captures==b->captures,"witness captures");
    require(!got.endpoint_consumer_checks,"witness does not guard");
   }
   if(input.size()<4){for(unsigned mode=0;mode<6;++mode){auto a=on,b=off;
    if(mode==0)a.collapse_control=b.collapse_control=false;
    if(mode==1)a.coalesce_returns=b.coalesce_returns=false;
    if(mode==2)a.compress_returns=b.compress_returns=false;
    if(mode==3)a.merge_equivalent_nodes=b.merge_equivalent_nodes=false;
    if(mode==4)a.regular_cursor_frontiers=b.regular_cursor_frontiers=false;
    if(mode==5)a.endpoint_consumer_admission=b.endpoint_consumer_admission=false;
    require(p.full_match(input,a)==p.full_match(input,b),"scheduler/control ablation");
   }}
  }
 }
 const auto filter=R"((?(DEFINE)(?<E>(?&C)(?:[|][|](?&C))*)(?<C>(?&P)(?:&&(?&P))*)(?<P>!?\((?&E)\)|(?&A)(?:==|!=|<=|>=|<|>)(?&A))(?<A>(?&T)(?:[+-](?&T))*)(?<T>(?&F)(?:[*/](?&F))*)(?<F>[0-9]+|[a-z][a-z0-9_]*|\((?&A)\)))(?&E))";
 detail::AstMachineProgram p(parse(filter));
 for(const std::string input:{"(x+2*3<=limit&&!(count==0||score>7))","(x+2*3=<limit&&!(count==0||score>7))","identifier==longidentifier","identifier+identifier==a","a==a","a==a||a==a"}){
  OccurrenceMatchOptions a,b;b.endpoint_consumer_guards=false;OccurrenceStatistics x,y;
  a.regional_execution=a.regional_constraints=b.regional_execution=b.regional_constraints=false;
  // Preserve this historical representation under the newer root tiers.
  a.bounded_word_prediction=a.deterministic_programs=a.normalized_exact_programs=a.native_slr=false;b.bounded_word_prediction=b.deterministic_programs=b.normalized_exact_programs=b.native_slr=false;
  require(p.full_match(input,a,&x)==p.full_match(input,b,&y),"filter differential");
  require(x.states<=y.states,"Filter objects omitted");require(x.work_steps<=y.work_steps,"Filter total charged work falls");
  rejected+=x.endpoint_consumer_rejections;
 }
 // Admission is only scheduling. Byte distribution changes recover a fresh
 // probe class, while equal-byte changed contexts can forgo useful proofs.
 for(bool same_byte:{false,true}){
  const auto pattern = same_byte
   ? R"((?<L>a+)(?<A>a#|\((?&A)\))(?&L)(?<B>;|\((?&B)\)))"
   : R"((?<L>[ab]+)(?<A>a#|\((?&A)\)))";
  const auto input=std::string(24,'a')+(same_byte?"#":"")+std::string(1024,same_byte?'a':'b')+(same_byte?";":"a#");
  detail::AstMachineProgram q(parse(pattern));OccurrenceMatchOptions a,b;b.endpoint_consumer_admission=false;
  a.regional_execution=a.regional_constraints=b.regional_execution=b.regional_constraints=false;
  // Preserve this historical representation under the newer root tiers.
  a.bounded_word_prediction=a.deterministic_programs=a.normalized_exact_programs=a.native_slr=false;b.bounded_word_prediction=b.deterministic_programs=b.normalized_exact_programs=b.native_slr=false;
  OccurrenceStatistics x,y;require(q.full_match(input,a,&x)&&q.full_match(input,b,&y),"admission shift semantics");
  require(x.endpoint_consumer_declines>0,"bounded admission activates");
  if(same_byte)require(x.endpoint_consumer_rejections<y.endpoint_consumer_rejections,"retained admission falsifier");
  else require(x.endpoint_consumer_rejections==y.endpoint_consumer_rejections,"changed-byte phase keeps useful proofs");
 }
 require(rejected>0,"rejection mechanism activated");require(fallbacks>0,"exact prior retry activated");
 std::cout<<"checks="<<checks<<" rejected="<<rejected<<" consumer_fallbacks="<<fallbacks<<'\n';
}
