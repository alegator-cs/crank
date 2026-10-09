#include "ast_machine.hpp"
#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>
using namespace crank;
namespace {
std::size_t checks=0, skipped=0, interval_skipped=0;
void require(bool b,const char* what){++checks;if(!b)throw std::runtime_error(what);}
auto semantic(const OccurrenceStatistics&s) {
    return std::tuple{s.memo_entries,s.memo_hits,s.return_links,s.completions,s.replay_steps,
        s.up_steps,s.down_steps,s.coalesced_returns,s.compression_lookups,s.compression_hops,
        s.compressed_completions,s.duplicate_completions,s.aggregate_word_tasks,
        s.aggregate_completion_members,s.suffix_word_tasks,s.suffix_members,
        s.native_agenda_tasks,s.regular_region_requests,s.regular_region_transitions};
}
OccurrenceMatchOptions roomy(){OccurrenceMatchOptions o;o.max_states=20'000'000;o.max_steps=200'000'000;return o;}
void compare(const detail::AstMachineProgram&p,const std::string&subject,bool expected) {
    auto old=roomy();old.aggregate_interval_runs=false;OccurrenceStatistics baseline;
    require(p.full_match(subject,old,&baseline)==expected,"independent language oracle");
    const auto endpoints=p.endpoints(subject,0,old);
    for(const bool prefix:{false,true}) {
        auto current=roomy();current.aggregate_prefix_blocks=prefix;OccurrenceStatistics s;
        require(p.full_match(subject,current,&s)==expected,"run language result");
        require(semantic(s)==semantic(baseline),"run changed logical scheduling");
        require(p.endpoints(subject,0,current)==endpoints,"run changed endpoint export");
        skipped+=s.aggregate_prefix_words_skipped;interval_skipped+=s.aggregate_run_words_skipped;
    }
}
}
int main(){try{
    for(int kind=0;kind<4;++kind){
        const std::array patterns{
            "(?<S>(?&S)(?&S)|a)", "(?<S>(?&S)(?&S)|a|)",
            "(?<S>(?&S)(?&S)(?&S)|a)",
            "(?(DEFINE)(?<S>(?&T)(?&T)|a)(?<T>(?&S)|))(?&S)"};
        const auto ast=parse(patterns[kind]);const detail::AstMachineProgram p(ast);
        for(std::size_t n:{0,1,2,3,15,16,17,63,64,65,127,128,129,191,192,193,255,256,257}) {
            const bool valid=kind==0?n>0:kind==2?n%2==1:true;
            compare(p,std::string(n,'a'),valid);
            if(n>=128){
                auto wrong=std::string(n,'a');wrong[n/2]='b';compare(p,wrong,false);
                wrong=std::string(n,'a');wrong.back()='b';compare(p,wrong,false);
            }
        }
        const std::string input="b"+std::string(256,'a')+"b";
        for(std::size_t start:{0,1,2,64,128,255,256,257,258}) {
            std::vector<std::size_t> expected;
            if(kind==1||kind==3)expected.push_back(start);
            for(auto end=start+1;end<=input.size()&&input[end-1]=='a';++end)
                if(kind!=2||(end-start)%2==1)expected.push_back(end);
            require(p.endpoints(input,start,roomy())==expected,"nonzero endpoint relation");
        }
        std::vector<NodeId> entries{ast.root,ast.capture_nodes.at(1),ast.root};
        auto options=roomy();OccurrenceStatistics stats;
        require(p.full_match_any(entries,std::string(257,'a'),options,&stats),"duplicate accepting seeds");
        detail::AtomicExclusions bits(ast.nodes.size());detail::NativeAssistanceStatistics as;
        require(p.full_match_assisted(std::string(257,'a'),options,bits,&stats,&as),"assisted empty certificates");
        auto old=options;old.aggregate_interval_runs=false;
        for(std::size_t n:{0,1,4,17}) {
            require(p.first_match(std::string(n,'a'),old)==p.first_match(std::string(n,'a'),options,&stats),"witness identity");
            require(stats.aggregate_runs==0&&stats.aggregate_prefix_blocks==0,"witness representation bypass");
        }
        for(const int mode:{0,1,2,3}) {
            auto old=options;old.aggregate_interval_runs=false;
            if(mode==0)old.coalesce_returns=false;
            if(mode==1)old.compress_returns=false;
            if(mode==2)old.collapse_control=false;
            if(mode==3)old.aggregate_suffix_returns=false;
            auto current=old;current.aggregate_interval_runs=true;
            require(p.endpoints(std::string(33,'a'),0,old)==p.endpoints(std::string(33,'a'),0,current),"old switch combination");
        }
    }
    // Repeated callers must keep demanded and open questions distinct. A false
    // union of the X starts on aab would manufacture the final b match.
    for(const auto& pattern:std::vector<std::string>{
        "(?(DEFINE)(?<X>a))a(?&X)c|(?&X)b",
        "(?<S>(?&S)(?&S)|a){2,4}b", "(?<S>(?&S)(?&S)|a|){0,3}b",
        "(?<S>(?&S)(?&S)|a)b|(?<T>(?&T)(?&T)|b)a"}) {
        detail::AstMachineProgram p(parse(pattern));auto old=roomy();old.aggregate_interval_runs=false;
        for(const auto& subject:std::vector<std::string>{"", "aab", std::string(192,'a')+"b",std::string(255,'a')+"b",std::string(192,'b')+"a"})
            compare(p,subject,p.full_match(subject,old));
    }
    // The unchanged corpus envelope is an explicit gate, not a raised cap.
    detail::AstMachineProgram catalan(parse("a(?<R>(?&R)(?&R)|a)b"));OccurrenceMatchOptions original;
    const std::string subject="a"+std::string(1024,'a')+"b";OccurrenceStatistics s;
    require(catalan.full_match(subject,original,&s),"Catalan1024 original1M/4M envelope");
    require(!s.native_acceleration_fallbacks&&s.states<1'000'000&&s.work_steps<4'000'000,"original cap no retry");
    auto no_runs=original;no_runs.aggregate_interval_runs=false;
    bool unresolved=false;try{(void)catalan.full_match(subject,no_runs);}catch(const ResourceLimit&){unresolved=true;}
    require(unresolved,"control still exceeds original allowance");
    require(skipped>0&&interval_skipped>0,"both compression representations activated");
    std::cout<<"dense_run_checks="<<checks<<" prefix_words_skipped="<<skipped
             <<" interval_words_skipped="<<interval_skipped<<" catalan1024_states="<<s.states
             <<" catalan1024_work="<<s.work_steps<<'\n';
}catch(const std::exception&e){std::cerr<<"checks="<<checks<<": "<<e.what()<<'\n';return 1;}}
