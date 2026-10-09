#include "engine/ast_machine.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
using namespace crank;
std::size_t checks = 0;
void require(bool b,const char* what){++checks;if(!b)throw std::runtime_error(what);}
int main(){try{
    std::size_t dense = 0, suffix = 0;
    for(const auto& pattern:std::vector<std::string>{"(?<S>(?&S)(?&S)|a)","(?<S>(?&S)(?&S)|a|)","(?<S>(?&S)(?&S)(?&S)|a)","a(?<S>(?&S)(?&S)|a)b", "(?<S>(?&S)(?&S)|a)b|(?<T>(?&T)(?&T)|b)a", "(?<S>(?&S)(?&S)|a){2,4}","(?<S>(?&S)(?&S)|a){0,3}b"}) {
        detail::AstMachineProgram p(parse(pattern));
        for(std::size_t n: {0,1,2,3,4,15,16,17,24,32,48,64}) {
            std::vector<std::string> inputs{std::string(n,'a'),std::string(n,'a')+'b',std::string(n,'b')+'a',std::string(n/2,'a')+'b'+std::string(n/2,'a')};
            for(const auto& subject:inputs) {
                OccurrenceMatchOptions old; old.max_states=10'000'000;old.max_steps=40'000'000;
                old.aggregate_returns=old.aggregate_suffix_returns=false;
                const auto want=p.full_match(subject,old);
                const auto ends=p.endpoints(subject,0,old);
                for(const bool masks:{false,true})for(const bool suffixes:{false,true}) {
                    auto o=old;o.aggregate_returns=masks;o.aggregate_suffix_returns=suffixes;
                    OccurrenceStatistics s;
                    require(p.full_match(subject,o,&s)==want,"full match ablation");
                    dense+=s.aggregate_word_tasks;suffix+=s.suffix_word_tasks;
                    require(p.endpoints(subject,0,o)==ends,"endpoint ablation");
                    if(pattern=="(?<S>(?&S)(?&S)|a)")require(want==(subject.size()>0&&subject.find_first_not_of('a')==std::string::npos),"independent nonempty unary oracle");
                    if(pattern=="(?<S>(?&S)(?&S)|a|)")require(want==(subject.find_first_not_of('a')==std::string::npos),"independent nullable unary oracle");
                    if(pattern=="(?<S>(?&S)(?&S)(?&S)|a)")require(want==(subject.size()%2==1&&subject.find_first_not_of('a')==std::string::npos),"independent odd unary oracle");
                }
                if(n==24){OccurrenceStatistics s;const auto before=p.first_match(subject,old);const auto after=p.first_match(subject,{},&s);require(bool(before)==bool(after),"witness existence");if(before)require(before->captures==after->captures,"witness captures");require(s.aggregate_word_tasks==0&&s.suffix_word_tasks==0,"witness bypass");}
            }
        }
    }
    require(dense>0&&suffix>0,"dense mechanisms exercised");
    // Independent nonzero-start endpoint oracle, including nullable replay.
    for(const bool nullable:{false,true}) {
        const auto a=parse(nullable?"(?<S>(?&S)(?&S)|a|)":"(?<S>(?&S)(?&S)|a)");detail::AstMachineProgram p(a);
        const std::string subject="b"+std::string(40,'a')+"b";
        for(std::size_t start=0;start<=subject.size();++start){std::vector<std::size_t> expected;if(nullable)expected.push_back(start);for(auto end=start+1;end<=subject.size()&&subject[end-1]=='a';++end)expected.push_back(end);require(p.endpoints(subject,start)==expected,"all-start unary relation");}
        detail::AtomicExclusions empty(a.nodes.size());OccurrenceStatistics s;require(p.full_match_assisted(std::string(48,'a'),{},empty,&s),"empty assistance agreement");require(s.aggregate_word_tasks>0,"assisted masks exercised");
        std::vector<NodeId> entry{a.root,a.capture_nodes.at(1)};require(p.full_match_any(entry,std::string(48,'a'),{},&s),"multiple accepting requests");
    }
    std::cout<<"aggregate checks="<<checks<<" dense_word_tasks="<<dense<<" suffix_word_tasks="<<suffix<<'\n';
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
