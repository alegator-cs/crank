#include "ast_machine.hpp"
#include "crank.hpp"
#include "tests/helpers/relation_reference.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <thread>
#include <tuple>

// The denotational oracle computes least fixed points of finite span relations.
// No parser table, certificate, lowerer, analysis facts or optimized engine is
// used to decide its expected language. Interpreter equality separately tests
// the stronger operational promise: exact historical metering and fallback.
using namespace crank;
namespace ref = certified_reference;
namespace {
std::string context;
std::size_t checks=0, subjects=0, oracle_queries=0, production_queries=0;
std::size_t generated=0, observation_checks=0, quota_queries=0, quota_limits=0;
std::size_t concurrent_queries=0, cancelled_queries=0;
std::size_t slr_uses=0, word_uses=0, predictive_uses=0, spine_uses=0;
void require(bool value,const std::string& why) {
    ++checks;
    if (!value) throw std::runtime_error(context+": "+why);
}
enum class Route { Default, SLR, Word, Predictive, Spine, Rim };
OccurrenceMatchOptions options(Route route=Route::Default,bool compiled=true) {
    OccurrenceMatchOptions o;
    o.max_states=8'000'000;o.max_steps=64'000'000;
    o.compiled_exact_programs=compiled;
    if (route==Route::Default) return o;
    o.regional_execution=o.regional_constraints=false;
    o.exact_end_spines=route==Route::Spine;
    o.predictive_recursion=route==Route::Predictive;
    o.predictive_exact_rims=route==Route::Rim;
    o.bounded_word_prediction=route==Route::Word;
    o.deterministic_programs=route==Route::SLR;
    return o;
}
void absent(const OccurrenceStatistics& s) {
    require(!s.lowered_slr_attempts && !s.lowered_predictive_attempts &&
        !s.lowered_bounded_word_attempts && !s.lowered_spine_attempts,
        "lowering escaped its whole-root unassisted Boolean contract");
}
void count(const OccurrenceStatistics& s) {
    slr_uses+=s.lowered_slr_attempts;word_uses+=s.lowered_bounded_word_attempts;
    predictive_uses+=s.lowered_predictive_attempts;spine_uses+=s.lowered_spine_attempts;
}
// Do not compare new physical-operation counters with semantic counters.
// Every old observable execution/accounting quantity in the exact route is
// compared individually, including discarded retry charges and peak storage.
void same_semantics(const OccurrenceStatistics& a,const OccurrenceStatistics& b) {
#define SAME(name) require(a.name==b.name,"interpreted/lowered counter " #name " differs: "+std::to_string(a.name)+"/"+std::to_string(b.name))
    SAME(states);SAME(work_steps);SAME(down_steps);SAME(up_steps);SAME(pruned);
    SAME(arithmetic_pruned);SAME(memo_entries);SAME(memo_hits);SAME(return_links);
    SAME(completions);SAME(proof_nodes);SAME(native_result_slots);SAME(limit_reached);
    SAME(spine_attempts);SAME(spine_steps);SAME(spine_transitions);SAME(spine_boundary_bytes);
    SAME(max_spine_state_slots);SAME(native_spine_fallbacks);
    SAME(predictive_rim_attempts);SAME(predictive_rim_bytes);SAME(native_predictive_rim_fallbacks);
    SAME(bounded_word_attempts);SAME(bounded_word_steps);SAME(bounded_word_frames);
    SAME(bounded_word_max_frames);SAME(native_bounded_word_fallbacks);
    SAME(exact_program_attempts);SAME(exact_program_actions);SAME(exact_program_shifts);
    SAME(exact_program_reductions);SAME(exact_program_max_stack);SAME(native_exact_program_fallbacks);
    SAME(predictive_attempts);SAME(predictive_steps);SAME(predictive_frames);
    SAME(max_predictive_frames);SAME(native_predictive_fallbacks);SAME(predictive_frame_runs);
    SAME(max_predictive_frame_runs);SAME(predictive_terminal_run_bytes);
    SAME(native_acceleration_fallbacks);SAME(native_consumer_fallbacks);
    SAME(native_frontier_fallbacks);SAME(native_ordinary_fallbacks);
    SAME(native_acceleration_discarded_states);SAME(native_acceleration_discarded_steps);
#undef SAME
}
struct Outcome { std::optional<bool> value; OccurrenceStatistics statistics; };
Outcome execute(const detail::AstMachineProgram& program,std::string_view input,OccurrenceMatchOptions o) {
    Outcome result;++production_queries;
    try {result.value=program.full_match(input,o,&result.statistics);}
    catch(const ResourceLimit&) {}
    return result;
}
void comparison(const detail::AstMachineProgram& program,std::string_view input,Route route,
        bool truth, bool quota=false, std::size_t states=8'000'000,std::size_t steps=64'000'000) {
    auto o=options(route,false);o.max_states=states;o.max_steps=steps;
    const auto interpreted=execute(program,input,o);
    o.compiled_exact_programs=true;const auto lowered=execute(program,input,o);
    require(interpreted.value==lowered.value,"interpreted/lowered result or ResourceLimit differs");
    if (lowered.value) require(*lowered.value==truth,"independent finite-relation disagreement");
    else {require(quota,"unrestricted oracle query exhausted");++quota_limits;}
    same_semantics(interpreted.statistics,lowered.statistics);
    absent(interpreted.statistics);count(lowered.statistics);
    if (quota) ++quota_queries;
}
struct Fixture { std::string name,pattern,alphabet; unsigned maximum; Route route; std::vector<std::string> extra; };
const std::string tags=R"((?(DEFINE)(?<S><a>(?&S)</a>|<b>(?&S)</b>|))(?&S))";
const std::string delayed=R"((?(DEFINE)(?<A>\((?&A)\)|x)(?<E>\((?&E)\)|(?&A)=(?&A)))(?&E))";
const std::string tree=R"((?(DEFINE)(?<S>a(?&S)b(?&S)|x))(?&S))";
const std::string rim=tree+"(?:z(?&S))*z";
const std::string pair=R"((?<S>a(?&S)b|x))";
std::vector<Fixture> fixtures() {
    return {
        {"late shared prefix",delayed,"()x=",3,Route::SLR,{"(x)=x","((x=x))","((x))=x","((x))","(x=x)=x"}},
        {"left arithmetic",R"((?(DEFINE)(?<E>(?&E)\+(?&T)|(?&T))(?<T>x|\((?&E)\)))(?&E))","x+()",3,Route::SLR,{"x+x+x","(x+x)+x","x+(x+x)","x++x"}},
        {"nullable left recursion",R"((?(DEFINE)(?<S>(?&S)a|))(?&S)b)","ab",5,Route::SLR,{}},
        {"nullable reduction chain",R"((?(DEFINE)(?<A>)(?<B>(?&A))(?<C>(?&B))(?<S>(?&C)a(?&S)|b))(?&S))","ab",5,Route::SLR,{}},
        {"unit reduction chain",R"((?(DEFINE)(?<A>a)(?<B>(?&A))(?<C>(?&B))(?<S>(?&S)(?&C)|(?&C)))(?&S))","ab",5,Route::SLR,{}},
        {"overlapping masks",R"((?(DEFINE)(?<S>[ab](?&S)c|[bc](?&S)d|x))(?&S))","abcdx",3,Route::SLR,{"axc","bxd","bxc","cx d"}},
        {"bounded tags",tags,"<>ab/",2,Route::Word,{"<a></a>","<b></b>","<a><b></b></a>","<a><b></a></b>","<a></b>"}},
        {"FOLLOW second byte",R"((?(DEFINE)(?<S>ab(?&S)|))(?&S)ac)","abc",4,Route::Word,{"ababac","abababc"}},
        {"finite repeated second byte",R"((?(DEFINE)(?<S>ab(?&S)ba|x))(?&S)(?:ac){1,3}ab)","abcx",3,Route::Word,{"xacab","xacacab","xab","abxbaacab"}},
        {"predictive two recursive children",tree,"abx",4,Route::Predictive,{"axbx","aaxbx bx","aaxbxbx","axbaxb x"}},
        {"predictive continuation runs",pair,"abx",4,Route::Predictive,{"aaaxbbb",std::string(12,'a')+'x'+std::string(12,'b')}},
        {"inward pair",pair,"abx",4,Route::Spine,{"aaaxbbb",std::string(12,'a')+'x'+std::string(12,'b')}},
        {"inward right dispatch",R"((?(DEFINE)(?<S>a(?&S)b|a(?&S)c|x))(?&S))","abcx",3,Route::Spine,{"aaxbc","aaxcb","aaxbb","aaxcc"}},
        {"inward mutual state",R"((?(DEFINE)(?<A>a(?&B)b|x)(?<B>c(?&A)d|y))(?&A))","abcdxy",2,Route::Spine,{"ayb","acxdb","acaybdb","acxdbb"}},
        {"inward layout decline",R"((?<S>aa(?&S)bb|xx))","abx",3,Route::Spine,{"aaxxbb","aaaaxxbbbb","aaxbb"}},
        {"exact suffix around predictor",rim,"abxz",3,Route::Rim,{"xz","axbxz","xzxz","axbxxz","axbxzx"}},
        {"ambiguous fallback",R"((?(DEFINE)(?<S>(?&S)(?&S)|a))(?&S))","ab",4,Route::SLR,{}},
        {"unproductive nullable cycle",R"((?(DEFINE)(?<S>(?&T)|a)(?<T>(?&S)|))(?&S)b)","ab",4,Route::Default,{}},
        {"binary bytes",R"((?(DEFINE)(?<S>\x00a(?&S)\xff|\x00b(?&S)\xfe|))(?&S))",std::string("\0a\xff",3),3,Route::Word,{std::string("\0a\xff",3),std::string("\0b\xfe",3),std::string("\0a\0b\xfe\xff",6),std::string("\0a\0b\xff\xfe",6)}}
    };
}
void compare_fixture(const Ast& ast,const std::vector<std::string>& inputs,const std::string& name,Route route) {
    const detail::AstMachineProgram program(ast);
    for (const auto& input:inputs) {
        context=name+" input="+ref::escaped(input);++subjects;++oracle_queries;
        const auto relation=ref::language(ast,input);
        comparison(program,input,route,relation.nodes[ast.root].at(0,input.size()));
    }
}
void all_bytes() {
    for (const auto route:{Route::SLR,Route::Word,Route::Predictive,Route::Spine}) {
        const auto pattern=route==Route::Word
            ? R"((?(DEFINE)(?<S>\x00[\x00-\x7f](?&S)a|\x00[\x80-\xff](?&S)b|x))(?&S))"
            : R"((?<S>[\x00-\x7f](?&S)\xff|[\x80-\xff](?&S)\x00|x))";
        const auto ast=parse(pattern);std::vector<std::string> inputs;
        for (unsigned byte=0;byte<256;++byte) {
            std::string s;if(route==Route::Word)s+='\0';s+=char(byte);s+='x';
            const char good=route==Route::Word?(byte<128?'a':'b'):(byte<128?'\xff':'\0');
            inputs.push_back(s+good);inputs.push_back(s+char(static_cast<unsigned char>(good)^1));
        }
        compare_fixture(ast,inputs,"all byte values including NUL and EOF distinction",route);
    }
}
void generated_grammars() {
    ref::Random random{UINT64_C(0xad3054c027a861e7)};
    for (unsigned n=0;n<48;++n) {
        const char a='a'+random.choose(3),b='d'+random.choose(3),x='x';
        const std::string aa(1,a),bb(1,b);std::string pattern;Route route;
        switch(n%4) {
        case 0: pattern="(?(DEFINE)(?<S>(?&S)"+aa+"|"+bb+"))(?&S)";route=Route::SLR;break;
        case 1: pattern="(?(DEFINE)(?<S>"+aa+"a(?&S)"+bb+"|"+aa+"b(?&S)"+bb+"|x))(?&S)";route=Route::Word;break;
        case 2: pattern="(?(DEFINE)(?<S>"+aa+"(?&S)"+bb+"(?&S)|x))(?&S)";route=Route::Predictive;break;
        default: pattern="(?(DEFINE)(?<S>"+aa+"(?&S)"+bb+"|x))(?&S)";route=Route::Spine;break;
        }
        auto inputs=ref::words(aa+bb+x,3);
        if(route==Route::Word)inputs.push_back(aa+"a"+aa+"b"+"x"+bb+bb);
        else if(route==Route::SLR)inputs.push_back(bb+aa+aa+aa);
        else if(route==Route::Predictive)inputs.push_back(aa+aa+"x"+bb+"x"+bb+"x");
        else inputs.push_back(aa+aa+aa+"x"+bb+bb+bb);
        compare_fixture(parse(pattern),inputs,"generated deterministic "+std::to_string(n),route);++generated;
    }
    for (unsigned n=0;n<32;++n) {
        auto f=ref::random_fixture(random,n,3);++generated;
        compare_fixture(f.ast,f.inputs,"generated arbitrary "+f.name,Route::Default);
    }
}
void quotas() {
    const std::vector<std::tuple<std::string,Route,std::vector<std::string>>> tests{
        {delayed,Route::SLR,{"x=x","(x)=x","((x=x))","((x))"}},
        {tags,Route::Word,{"","<a></a>","<a><b></b></a>","<a></b>"}},
        {pair,Route::Predictive,{"x","aaaxbbb","aaaxbba"}},
        {pair,Route::Spine,{"x","aaaxbbb","aaaxbba"}},
        {rim,Route::Rim,{"xz","axbxz","axbxzx"}},
    };
    for (const auto& [pattern,route,inputs]:tests) {
        const auto ast=parse(pattern);const detail::AstMachineProgram program(ast);
        for (const auto& input:inputs) {
            context="every short quota prefix "+pattern+" / "+ref::escaped(input);
            const bool truth=ref::language(ast,input).nodes[ast.root].at(0,input.size());++oracle_queries;
            const auto base=execute(program,input,options(route,false));require(base.value.has_value(),"quota reference incomplete");
            // Cut separately through every state and work charge, then cut both
            // at deterministic mixed prefixes. These hit interior fused actions.
            const auto states=std::min<std::size_t>(base.statistics.states+2,100);
            const auto steps=std::min<std::size_t>(base.statistics.work_steps+2,200);
            for(std::size_t i=0;i<=states;++i)comparison(program,input,route,truth,true,i,64'000'000);
            for(std::size_t i=0;i<=steps;++i)comparison(program,input,route,truth,true,8'000'000,i);
            for(std::size_t i=0;i<=std::min(states,steps);++i)comparison(program,input,route,truth,true,i,i);
        }
    }
    require(quota_limits>0,"quota sweep had no unresolved prefix");
}
void long_sequences() {
    const detail::AstMachineProgram program(parse(pair));
    for(const auto route:{Route::Predictive,Route::Spine}) {
        context="long known nesting crosses terminal batches and cancellation polling boundaries";
        const auto input=std::string(192,'a')+'x'+std::string(192,'b');
        comparison(program,input,route,true);
        for(const auto at:{193U,255U,256U,257U,383U,384U}) {
            auto bad=input;bad[at]='a';comparison(program,bad,route,false);
        }
        const auto original=execute(program,input,options(route,false));
        for(std::size_t difference=0;difference<=130;++difference)
            comparison(program,input,route,true,true,8'000'000,original.statistics.work_steps-difference);
        auto old=options(route,false);old.predictive_continuation_runs=false;
        auto now=old;now.compiled_exact_programs=true;
        const auto a=execute(program,input,old),b=execute(program,input,now);
        require(a.value==b.value && a.value==true,"continuation-run disabled lowering changed answer");same_semantics(a.statistics,b.statistics);
    }
}
void observers() {
    for (const auto& [pattern,route]:std::vector<std::pair<std::string,Route>>{{tags,Route::Word},{delayed,Route::SLR},{tree,Route::Predictive},{pair,Route::Spine}}) {
        const auto ast=parse(pattern);const detail::AstMachineProgram p(ast);
        for(const auto& input:std::vector<std::string>{"","x","x=x","<a></a>","axb"}) {
            context="observer scope "+pattern+" / "+input;
            const auto relation=ref::language(ast,input);++oracle_queries;OccurrenceStatistics stats;
            detail::AtomicExclusions empty(ast.nodes.size());
            require(p.full_match_assisted(input,options(route),empty,&stats)==relation.nodes[ast.root].at(0,input.size()),"assisted Boolean changed");absent(stats);++observation_checks;
            detail::DeferredAssistance deferred;
            require(p.full_match_deferred(input,options(route),deferred,&stats)==relation.nodes[ast.root].at(0,input.size()),"deferred Boolean changed");absent(stats);++observation_checks;
            require(p.first_match(input,options(route),&stats)==p.first_match(input,options(route,false)),"first witness changed");absent(stats);++observation_checks;
            for(std::size_t begin=0;begin<=input.size();++begin) {
                std::vector<std::size_t> expected;for(auto end=begin;end<=input.size();++end)if(relation.nodes[ast.root].at(begin,end))expected.push_back(end);
                require(p.endpoints(input,begin,options(route),&stats)==expected,"endpoint enumeration changed");absent(stats);++observation_checks;
            }
            std::vector<NodeId> entries;bool any=false;
            for(const auto entry:ast.capture_nodes)if(entry!=no_id && entry!=ast.root) {
                require(p.full_match_node(entry,input,options(route),&stats)==relation.nodes[entry].at(0,input.size()),"nonroot entry changed");absent(stats);++observation_checks;
                entries.push_back(entry);any|=relation.nodes[entry].at(0,input.size());
            }
            require(p.full_match_any(entries,input,options(route),&stats)==any,"entry union changed");absent(stats);++observation_checks;
        }
    }
}
void ownership_and_concurrency() {
    for(const auto& [pattern,route]:std::vector<std::pair<std::string,Route>>{{tags,Route::Word},{delayed,Route::SLR},{tree,Route::Predictive},{pair,Route::Spine},{R"((?<S>aa(?&S)bb|xx))",Route::Spine}}) {
        context="immutable source snapshot and concurrent reuse";
        auto ast=parse(pattern);const auto original=ast;const detail::AstMachineProgram p(ast);
        // Destroy all information in the caller's source. The program owns its
        // compilation provenance and must never borrow this mutable graph.
        ast.nodes.clear();ast.capture_nodes.clear();ast.root=no_id;
        auto copy=p;std::vector<std::string> inputs{"","x","axb","x=x","(x)=x","<a></a>","<a></b>","axbx","aaxxbb"};
        std::vector<bool> truth;for(const auto& s:inputs){truth.push_back(ref::language(original,s).nodes[original.root].at(0,s.size()));++oracle_queries;}
        std::atomic_bool go{false};std::vector<std::exception_ptr> errors(6);std::vector<std::thread> workers;
        for(std::size_t t=0;t<errors.size();++t)workers.emplace_back([&,t]{
            while(!go.load(std::memory_order_acquire))std::this_thread::yield();
            try{for(unsigned n=0;n<48;++n){const auto i=(n+t)%inputs.size();OccurrenceStatistics stats;
                const auto& handle=n%2?p:copy;
                if(handle.full_match(inputs[i],options(route,n%3!=0),&stats)!=truth[i])throw std::runtime_error("shared lowering corrupted a query");
            }}catch(...){errors[t]=std::current_exception();}
        });
        go.store(true,std::memory_order_release);for(auto& worker:workers)worker.join();
        for(const auto& error:errors){if(error)std::rethrow_exception(error);require(!error,"concurrent worker error");}
        concurrent_queries+=errors.size()*48;
        for(std::size_t i=0;i<inputs.size();++i)comparison(copy,inputs[i],route,truth[i]);
    }
}
const detail::LoweringPreparationStatistics& selected(const detail::CompiledProgramPreparationStatistics& s,Route route) {
    if(route==Route::SLR)return s.slr;
    if(route==Route::Word)return s.bounded_word;
    if(route==Route::Predictive)return s.predictive;
    if(route==Route::Rim)return s.predictive_rim;
    return s.spine;
}
void preparation() {
    const std::vector<std::tuple<std::string,Route,std::string,bool>> fixtures{
        {delayed,Route::SLR,"(x)=x",true},{tags,Route::Word,"<a></a>",true},
        {tree,Route::Predictive,"axbx",true},{pair,Route::Spine,"axb",true},
        {rim,Route::Rim,"axbxz",true},
        {R"((?<S>aa(?&S)bb|xx))",Route::Spine,"aaxxbb",false}
    };
    for(const auto& [pattern,route,input,ready]:fixtures) {
        context="lazy positive/negative provenance and master/per-family opt-outs "+pattern;
        const detail::AstMachineProgram program(parse(pattern));
        require(!selected(program.compiled_program_preparation_statistics(),route).attempted,"eager lowering before query");
        auto o=options(route,false);require(program.full_match(input,o),"interpreted cache fixture failed");
        require(!selected(program.compiled_program_preparation_statistics(),route).attempted,"disabled lowering still compiled");
        o=options(route);o.lowered_slr=o.lowered_predictive=o.lowered_spines=false;
        OccurrenceStatistics disabled;require(program.full_match(input,o,&disabled),"family disabled route changed answer");absent(disabled);
        require(!selected(program.compiled_program_preparation_statistics(),route).attempted,"family-disabled lowering compiled");
        o=options(route);require(program.full_match(input,o),"enabled lowering changed answer");
        const auto first=selected(program.compiled_program_preparation_statistics(),route);
        require(first.attempted && first.ready==ready,"expected positive/negative lowering was not cached");
        const auto version = route==Route::Word || route==Route::Predictive || route==Route::Rim ? 2U : 1U;
        require(first.compiler_version==version,"missing lowering compiler version");
        if(ready)require(first.code_bytes>0 && first.retained_bytes>=first.code_bytes,"invalid retained program accounting");
        for(unsigned i=0;i<4;++i)require(program.full_match(input,o),"reuse changed answer");
        const auto second=selected(program.compiled_program_preparation_statistics(),route);
        require(first.attempted==second.attempted && first.ready==second.ready && first.compiler_version==second.compiler_version &&
            first.compilation_steps==second.compilation_steps && first.retained_bytes==second.retained_bytes && first.code_bytes==second.code_bytes,
            "immutable cached lowering repeated work or changed payload");
    }
}
void cancellation() {
    for(const auto& [pattern,route]:std::vector<std::pair<std::string,Route>>{{tags,Route::Word},{delayed,Route::SLR},{pair,Route::Predictive},{pair,Route::Spine}}) {
        context="cancellation distinct from rejection and resource limit";
        const auto ast=parse(pattern);const detail::AstMachineProgram program(ast);
        const std::string small=route==Route::Word?"<a></a>":route==Route::SLR?"x=x":"axb";
        require(program.full_match(small,options(route)),"cancellation warm-up failed");
        for(const bool lowered:{false,true}) {
            std::atomic_bool stopped{true};OccurrenceStatistics stats;bool caught=false;
            try{(void)program.full_match_node(ast.root,small,options(route,lowered),&stats,&stopped);}
            catch(const detail::AstExecutionCancelled&){caught=true;}
            require(caught && !stats.limit_reached,"pre-cancelled query returned a Boolean or ResourceLimit");++cancelled_queries;
        }
        std::string input;
        if(route==Route::Word){for(unsigned i=0;i<350'000;++i)input+="<a>";for(unsigned i=0;i<350'000;++i)input+="</a>";}
        else if(route==Route::SLR)input=std::string(1'000'000,'(')+"x=x"+std::string(1'000'000,')');
        else input=std::string(2'000'000,'a')+'x'+std::string(2'000'000,'b');
        std::atomic_bool stop{false};OccurrenceStatistics stats;bool caught=false;
        auto o=options(route);o.max_states=256'000'000;o.max_steps=512'000'000;
        std::thread interrupter([&]{std::this_thread::sleep_for(std::chrono::milliseconds(2));stop.store(true,std::memory_order_relaxed);});
        try{(void)program.full_match_node(ast.root,input,o,&stats,&stop);}
        catch(const detail::AstExecutionCancelled&){caught=true;}
        catch(...){interrupter.join();throw;}
        interrupter.join();require(caught && !stats.limit_reached,"running lowered query failed cooperative cancellation");
        require(stats.lowered_slr_attempts+stats.lowered_bounded_word_attempts+stats.lowered_predictive_attempts+stats.lowered_spine_attempts>0,"cancellation never entered lowered executor");
        ++cancelled_queries;
    }
}
void effects() {
    context="effect fields defeat stale summary flags";
    for(const auto& pattern:{tags,delayed,tree,pair}) {
        auto ast=parse(pattern);ast.has_calls=ast.has_counts=ast.has_references=false;
        ast.nodes[ast.root].bind_count=0;
        require(!detail::AstMachineProgram::eligible(ast),"stale flags admitted observable count effect");
    }
    context="public API parity";
    for(const auto& pattern:{tags,delayed,tree,pair}) {
        const Program p(pattern);
        for(const auto& s:std::vector<std::string>{"","x","axb","x=x","<a></a>","<a></b>"}) {
            const auto truth=ref::language(p.ast(),s).nodes[p.ast().root].at(0,s.size());++oracle_queries;
            require(p.full_match(s)==truth,"public Auto differs from finite relation");++production_queries;
        }
    }
}
}
int main() {
    try {
        const auto cases=fixtures();
        for(const auto& fixture:cases){auto inputs=ref::words(fixture.alphabet,fixture.maximum);inputs.insert(inputs.end(),fixture.extra.begin(),fixture.extra.end());
            compare_fixture(parse(fixture.pattern),inputs,fixture.name,fixture.route);}
        all_bytes();generated_grammars();quotas();long_sequences();observers();ownership_and_concurrency();preparation();cancellation();effects();
        require(slr_uses && word_uses && predictive_uses && spine_uses,"not all lowering routes exercised");
        std::cout<<"{\"status\":\"passed\",\"compiled_exact_checks\":"<<checks<<",\"targeted_fixtures\":"<<cases.size()
            <<",\"generated_grammars\":"<<generated<<",\"subjects\":"<<subjects<<",\"finite_relation_queries\":"<<oracle_queries
            <<",\"production_queries\":"<<production_queries<<",\"observation_checks\":"<<observation_checks
            <<",\"quota_pairs\":"<<quota_queries<<",\"quota_limit_pairs\":"<<quota_limits
            <<",\"concurrent_queries\":"<<concurrent_queries<<",\"cancelled_queries\":"<<cancelled_queries
            <<",\"lowered_slr_attempts\":"<<slr_uses<<",\"lowered_word_attempts\":"<<word_uses
            <<",\"lowered_predictive_attempts\":"<<predictive_uses<<",\"lowered_spine_attempts\":"<<spine_uses<<"}\n";
    }catch(const std::exception& error){std::cerr<<"COMPILED EXACT FAILURE: "<<error.what()<<'\n';return 1;}
}
