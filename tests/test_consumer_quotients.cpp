#include "ast_machine.hpp"
#include <algorithm>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
using namespace crank;
std::size_t checks;
void require(bool b,const std::string& why){++checks;if(!b)throw std::runtime_error(why);}
std::vector<std::string> words(std::string alphabet,unsigned maximum){std::vector<std::string> all{std::string{}},layer=all;while(maximum--){std::vector<std::string> next;for(auto& p:layer)for(char c:alphabet)next.push_back(p+c);all.insert(all.end(),next.begin(),next.end());layer=std::move(next);}return all;}
bool unary(std::string_view s){return s.find_first_not_of('a')==s.npos;}
bool equal(std::string_view s){auto n=s.size()/2;return s.size()%2==0&&s.substr(0,n)==std::string(n,'a')&&s.substr(n)==std::string(n,'b');}
bool dyck(std::string_view s){int n=0;for(char c:s){if(c=='(')++n;else if(c==')'){if(--n<0)return false;}else return false;}return n==0;}
int main(){try{
 struct Fixture{std::string pattern,alphabet;std::function<bool(std::string_view)> accepts;};
 const std::vector<Fixture> fixtures={
  {"a*ab","ab",[](auto s){return s.size()>=2&&s.back()=='b'&&unary(s.substr(0,s.size()-1));}},
  {"(?<S>a(?&S)b|)","ab",equal},
  {"(?<S>(?&S)(?&S)|a)","ab",[](auto s){return !s.empty()&&unary(s);}},
  {"(?<S>[(](?&S)[)](?&S)|)","()",dyck},
  {"(?<E>(?&E)[+](?&E)|a)","a+",[](auto s){if(s.size()%2!=1)return false;for(std::size_t i=0;i<s.size();++i)if(s[i]!=(i%2?'+':'a'))return false;return true;}},
  {"(?(DEFINE)(?<A>(?&B)|a)(?<B>(?&A)|))(?&A)","ab",[](auto s){return s.empty()||s=="a";}},
  {"(?<S>a(?&S)|b(?&S)|)","ab",[](auto){return true;}},
  {"(a?){18446744073709551615}","ab",unary},
  {"(?(DEFINE)(?<A>a(?&A)b|)(?<B>a(?&B)b|))(?&A)c|(?&B)d","abcd",[](auto s){return !s.empty()&&(s.back()=='c'||s.back()=='d')&&equal(s.substr(0,s.size()-1));}}
 };
 for(const auto& f:fixtures){const auto ast=parse(f.pattern);detail::AstMachineProgram p(ast);
  for(const auto& s:words(f.alphabet,4))for(bool prune:{false,true})for(unsigned mode=0;mode<4;++mode){
   OccurrenceMatchOptions opt;opt.prune=prune;opt.inline_terminals=mode&1;opt.merge_equivalent_nodes=mode&2;
   OccurrenceStatistics stats;require(p.full_match(s,opt,&stats)==f.accepts(s),"independent membership: "+f.pattern+" : "+s);
   if(!(mode&1))require(stats.inline_terminal_transitions==0,"terminal ablation records no direct transition");
   for(std::size_t start=0;start<=s.size();++start){std::vector<std::size_t> expected;for(auto end=start;end<=s.size();++end)if(f.accepts(std::string_view(s).substr(start,end-start)))expected.push_back(end);require(p.endpoints(s,start,opt)==expected,"independent endpoints: "+f.pattern+" : "+s);}
  }
 }
 // Branching nullable expressions, wide boundaries, and high counts exercise
 // scheduling/coalescing fallbacks beyond the independent language fixtures.
 std::vector<std::string> patterns={"(?:a?|b?){2,4}","(?:a|aa)*a", "(?:a?|b?)*a?", "(?<S>(?&S)(?&S)|a|)", "(?:a?|b?){63,66}b", "(a*)(a*)", "(?(DEFINE)(?<A>(?&A)|a|))(?&A){2,4}"};
 std::string wide;for(unsigned i=0;i<70;++i)wide+="a?";patterns.push_back(wide+"b");
 for(const auto& pattern:patterns){detail::AstMachineProgram p(parse(pattern));for(const auto& s:words("ab",4)){
  OccurrenceMatchOptions old;old.inline_terminals=false;old.merge_equivalent_nodes=false;
  const auto want=p.full_match(s,old);const auto ends=p.endpoints(s,0,old);const auto witness=p.first_match(s,old);
  for(bool compression:{false,true})for(bool collapse:{false,true})for(bool coalescing:{false,true})for(unsigned mode=0;mode<4;++mode){
   auto opt=old;opt.compress_returns=compression;opt.collapse_control=collapse;opt.coalesce_returns=coalescing;opt.inline_terminals=mode&1;opt.merge_equivalent_nodes=mode&2;
   require(p.full_match(s,opt)==want,"strategy membership");require(p.endpoints(s,0,opt)==ends,"strategy endpoints");
   OccurrenceStatistics stats;const auto got=p.first_match(s,opt,&stats);require(bool(got)==bool(witness),"witness membership");if(got)require(got->captures==witness->captures,"witness capture identity");require(stats.inline_terminal_transitions==0,"witness bypasses direct transitions");
  }
 }}
 // Optional atomic exclusion certificates remain applicable to original and
 // redirected terminal nodes; no direct task can resurrect a globally dead node.
 const auto ast=parse("(?:a|b)*c");detail::AstMachineProgram p(ast);detail::AtomicExclusions bits(ast.nodes.size());
 for(NodeId id=0;id<ast.nodes.size();++id)if(ast.nodes[id].kind==Kind::CharSet&&ast.nodes[id].characters.test('b'))bits.exclude(id);
 require(p.full_match_assisted("aaac",{},bits),"direct terminal accepts beside absent branch");
 detail::AtomicExclusions none(ast.nodes.size());OccurrenceStatistics base,assisted;require(p.full_match("aaac",{},&base),"native atomic control");OccurrenceMatchOptions exact;exact.max_states=base.states;exact.max_steps=base.work_steps;
 require(p.full_match_assisted("aaac",exact,none,&assisted),"empty helper preserves exact quotient budget");require(base.states==assisted.states&&base.work_steps==assisted.work_steps,"quotient helper parity");
 // Multiple accepting entries may canonicalize to one request; it must stay
 // protected from transparent-return compression as an accepting seed.
 const auto entry_ast=parse("(?(DEFINE)(?<A>a(?&A)b|)(?<B>a(?&B)b|))x");
 detail::AstMachineProgram entries(entry_ast);
 const std::vector<NodeId> choices{entry_ast.capture_nodes.at(1),entry_ast.capture_nodes.at(2)};
 for(unsigned mode=0;mode<4;++mode){OccurrenceMatchOptions opt;opt.inline_terminals=mode&1;opt.merge_equivalent_nodes=mode&2;
  require(entries.full_match_any(choices,"aaabbb",opt,nullptr),"equivalent entry aliases accept");
  require(entries.full_match_any(choices,"",opt,nullptr),"equivalent nullable entries accept epsilon");
  require(!entries.full_match_any(choices,"aabbb",opt,nullptr),"equivalent entry aliases reject");
 }
 const auto delayed_ast=parse("[ab]*");detail::AstMachineProgram delayed(delayed_ast);
 detail::AtomicExclusions delayed_bits(delayed_ast.nodes.size());
 struct Context{detail::AtomicExclusions* bits;unsigned calls=0;};Context context{&delayed_bits};
 const detail::DeferredAssistance hook{128,&context,[](void* ptr,std::size_t)noexcept -> const detail::AtomicExclusions* {
  auto& c=*static_cast<Context*>(ptr);++c.calls;c.bits->reject_root();return c.bits;}};
 auto bad=std::string(512,'a');bad[400]='z';OccurrenceStatistics stopped;
 require(!delayed.full_match_deferred(bad,{},hook,&stopped),"deferred root proof stops direct tasks");
 require(context.calls==1&&stopped.work_steps==128,"direct-task deferred hook retains exact polling boundary");
 OccurrenceMatchOptions tiny;tiny.max_states=1;bool limited=false;
 try{(void)delayed.full_match("aaaa",tiny);}catch(const ResourceLimit&){limited=true;}
 require(limited,"direct transitions retain native unresolved budget status");
 std::cout<<"consumer quotient checks: "<<checks<<'\n';
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
