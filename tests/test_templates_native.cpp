#include "ast_machine.hpp"
#include "crank.hpp"
#include "native_slr_program.hpp"
#include "tests/helpers/normalized_exact_program.hpp"
#include "compiled_exact_programs.hpp"
#include "tests/helpers/relation_reference.hpp"
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <thread>
#if defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#endif

using namespace crank;
namespace ref = certified_reference;
namespace {
std::size_t checks=0,subjects=0,oracle_queries=0,paired_queries=0,quota_pairs=0;
std::size_t observer_queries=0,concurrent_queries=0,guard_queries=0,cancellations=0;
std::size_t normalized_admissions=0,native_calls=0,generated_grammars=0;
std::string context;
void require(bool condition,const std::string& why) {
    ++checks;if(!condition)throw std::runtime_error(context+": "+why);
}
std::string fingerprint(const Ast& ast) {
    std::ostringstream out;
    out<<ast.root<<'/'<<ast.nodes.size()<<'/'<<ast.has_calls<<ast.has_counts
       <<ast.has_references<<ast.has_extended<<ast.has_occurrence_paths;
    for(const auto& n:ast.nodes) {
        out<<'|'<<static_cast<int>(n.kind)<<n.characters<<':'<<n.lower<<':'
           <<(n.upper?std::to_string(*n.upper):"*")<<':'<<n.capture_id<<':'
           <<n.call_target<<':'<<n.bind_count<<':'<<n.use_count<<':'<<n.count_path
           <<':'<<n.forward<<':'<<n.nullable<<':'<<n.minimum_length<<':'<<n.stateful;
        for(auto child:n.children)out<<','<<child;
    }
    for(auto id:ast.capture_nodes)out<<'#'<<id;
    for(const auto& name:ast.capture_names)out<<'@'<<name;
    return out.str();
}
const std::string templ=R"((?(DEFINE)(?<T>(?:[a-z ]+|\$\{[a-z]+\}|(?&I))*)(?<I>%\{if [a-z]+\}(?&T)(?:%\{else\}(?&T))?%\{end\}))(?&T))";
const std::string simple=R"((?(DEFINE)(?<T>(?:a+|(?&I))*)(?<I>ab(?&T)ac))(?&T))";
const std::string filter=R"((?(DEFINE)(?<A>\((?&A)\)|x)(?<E>\((?&E)\)|(?&A)=(?&A)))(?&E))";
const std::string assign=R"((?(DEFINE)(?<S>(?&L)=(?&R)|(?&R))(?<L>\*(?&R)|x)(?<R>(?&L)))(?&S))";

OccurrenceMatchOptions options(bool native=true,bool normalized=true) {
    OccurrenceMatchOptions o;
    o.max_steps=64'000'000;o.max_states=8'000'000;
    o.regional_execution=o.regional_constraints=false;
    o.predictive_recursion=o.predictive_exact_rims=o.bounded_word_prediction=o.exact_end_spines=false;
    o.native_slr=native;o.normalized_exact_programs=normalized;
    return o;
}
struct Result { std::optional<bool> value;OccurrenceStatistics stats; };
Result run(const detail::AstMachineProgram& program,const std::string& input,OccurrenceMatchOptions o) {
    Result result;
    try{result.value=program.full_match(input,o,&result.stats);}catch(const ResourceLimit&){}
    return result;
}
void same(const OccurrenceStatistics& a,const OccurrenceStatistics& b) {
#define EQ(field) require(a.field==b.field,"same-certificate semantic counter differs: " #field)
    EQ(states);EQ(work_steps);EQ(down_steps);EQ(up_steps);EQ(pruned);
    EQ(memo_entries);EQ(memo_hits);EQ(return_links);EQ(completions);EQ(native_result_slots);
    EQ(exact_program_attempts);EQ(exact_program_actions);EQ(exact_program_shifts);
    EQ(exact_program_reductions);EQ(exact_program_max_stack);EQ(native_exact_program_fallbacks);
    EQ(limit_reached);EQ(native_acceleration_fallbacks);EQ(native_consumer_fallbacks);
    EQ(native_frontier_fallbacks);EQ(native_ordinary_fallbacks);
    EQ(native_acceleration_discarded_states);EQ(native_acceleration_discarded_steps);
    EQ(normalized_exact_attempts);EQ(native_normalized_program_fallbacks);
#undef EQ
}
void absent_new(const OccurrenceStatistics& stats) {
    require(!stats.native_slr_attempts&&!stats.normalized_exact_attempts,
        "native/normalized program escaped its observation contract");
}
void pair(const detail::AstMachineProgram& p,const std::string& input,bool expected,
          OccurrenceMatchOptions o=options(),bool limited=false) {
    o.native_slr=false;const auto a=run(p,input,o);
    o.native_slr=true;const auto b=run(p,input,o);++paired_queries;
    require(a.value==b.value,"native and portable result/resource status differ");
    if(a.value)require(*a.value==expected,"independent denotation mismatch");
    else require(limited,"unrestricted query failed to decide");
    same(a.stats,b.stats);
}
struct Fixture {std::string name,pattern,alphabet;unsigned maximum;std::vector<std::string> extras;};
std::vector<Fixture> fixtures() {
    return {
        {"pure template",templ,"a%{}$",2,{"","a","abc","${x}","%{if x}%{end}","%{if x}a%{else}b%{end}","%{if x}%{if y}a%{end}%{end}","%","%{","%{i","%{e","%{en","%{else}","%{if x}%{else}%{else}%{end}","%{if x}%{end}a"}},
        {"shared prefix",simple,"abc",5,{"abac","ababacac","aaabacaa","abaaaac","abaac"}},
        {"unobservable partitions",R"((?(DEFINE)(?<B>a+)(?<T>(?:(?&B)|b(?&T)c)*))(?&T))","abc",4,{"baac","abbcc"}},
        {"alias has outside use",R"((?(DEFINE)(?<B>a+))(?:(?&B)|b)*(?&B))","ab",5,{}},
        {"nullable partition",R"((?:(?:a?)+|b)*)","ab",5,{}},
        {"nullable nested stars",R"((?:(?:a*)*|b)*)","ab",5,{}},
        {"finite bounds inside star",R"((?:a{2,3}|b)*)","ab",5,{}},
        {"finite outer bound",R"((?:a+|b){2,3})","ab",5,{}},
        {"finite positive lower",R"((?:a{2,}|b)*)","ab",5,{}},
        {"duplicate pure captures",R"((?:(?<A>a+)|(?<B>a+)|b)*)","ab",5,{}},
        {"overlapping text and directive",R"((?(DEFINE)(?<T>(?:[abc]+|ab(?&T)ac)*))(?&T))","abc",4,{}},
        {"empty recursion",R"((?(DEFINE)(?<A>(?&B)|a)(?<B>(?&A)|))(?&A))","ab",4,{}},
        {"mutual wrapper aliases",R"((?(DEFINE)(?<A>(?&B))(?<B>a(?&A)b|x))(?&A))","abx",4,{"aaxbb"}},
        {"unproductive aliases",R"((?(DEFINE)(?<A>(?&B))(?<B>(?&A)))(?&A))","ab",4,{}},
        {"left recursion",R"((?(DEFINE)(?<S>(?&S)a|b))(?&S))","ab",5,{}},
        {"irreducible ambiguity",R"((?(DEFINE)(?<S>(?&S)(?&S)|a))(?&S))","ab",4,{}},
        {"delayed SLR",filter,"()x=",3,{"x=x","(x)=x","((x=x))","((x))","x=x=x"}},
        {"assignment",assign,"x*=",4,{"x=x","*x=x","x=*x","**x=**x"}},
        {"binary grammar",R"((?(DEFINE)(?<S>(?:\x00+|\xff(?&S)\xfe)*))(?&S))",std::string("\0\xff\xfe",3),4,{}}
    };
}
void normalized_languages() {
    for(const auto& f:fixtures()) {
        const auto ast=parse(f.pattern);const auto original=fingerprint(ast);
        const detail::AstMachineProgram p(ast);
        std::vector<std::unique_ptr<detail::NormalizedExactProgram>> normalized;
        for(bool repetition:{false,true})for(bool wrappers:{false,true}) {
            detail::NormalizedExactProgram::Options o;
            o.repetition_quotient=repetition;o.transparent_calls=wrappers;
            normalized.push_back(std::make_unique<detail::NormalizedExactProgram>(ast,o));
            normalized_admissions+=normalized.back()->ready();
            if(normalized.back()->statistics().nullable_complete) {
                const auto& changed=normalized.back()->normalized_ast();
                const auto empty=ref::language(changed,"");++oracle_queries;
                for(NodeId id=0;id<changed.nodes.size();++id)
                    require(changed.nodes[id].nullable==empty.nodes[id].at(0,0),"nullable facade differs from independent empty relation");
            }
        }
        require(fingerprint(ast)==original,"normalization modified original source");
        auto inputs=ref::words(f.alphabet,f.maximum);inputs.insert(inputs.end(),f.extras.begin(),f.extras.end());
        for(const auto& input:inputs) {
            context=f.name+" / "+ref::escaped(input);++subjects;++oracle_queries;
            const bool expected=ref::language(ast,input).nodes[ast.root].at(0,input.size());
            pair(p,input,expected);
            for(const auto& n:normalized) {
                const auto& converted=n->normalized_ast();++oracle_queries;
                require(ref::language(converted,input).nodes[converted.root].at(0,input.size())==expected,
                        "normalization changed least-fixed-point language");
                if(const auto* cert=n->certificate()) {
                    detail::DeterministicProgram::Runtime runtime;
                    require(cert->execute(input,0,input.size(),[]{},[]{},runtime)==expected,"normalized certificate mismatch");
                }
            }
        }
    }
}
void generated_normalizers() {
    ref::Random random{UINT64_C(0xc091ad21eb87f023)};
    for(unsigned i=0;i<48;++i) {
        context="generated normalization "+std::to_string(i);
        auto f=ref::random_fixture(random,i,3);const auto before=fingerprint(f.ast);
        const detail::NormalizedExactProgram normalized(f.ast);
        require(fingerprint(f.ast)==before,"generated normalization mutated source");
        const auto& changed=normalized.normalized_ast();
        for(const auto& input:f.inputs) {
            ++subjects;oracle_queries+=2;
            const bool expected=ref::language(f.ast,input).nodes[f.ast.root].at(0,input.size());
            require(ref::language(changed,input).nodes[changed.root].at(0,input.size())==expected,
                "generated normalizer changed language");
            if(const auto* cert=normalized.certificate()) {
                detail::DeterministicProgram::Runtime runtime;
                require(cert->execute(input,0,input.size(),[]{},[]{},runtime)==expected,"generated normalized SLR changed language");
            }
        }
        ++generated_grammars;
    }
}
void quota_prefixes() {
    for(const auto& [pattern,inputs]:std::vector<std::pair<std::string,std::vector<std::string>>>{
        {filter,{"x=x","(x)=x","((x=x))","((x))"}},
        {templ,{"","a","${x}","%{if x}a%{else}b%{end}","%{if x}%{else}%{end}"}},
        {assign,{"x=x","*x=x","x=*x","x==x"}}}) {
        const auto ast=parse(pattern);const detail::AstMachineProgram p(ast);
        for(const auto& input:inputs) {
            context="native quota prefix "+ref::escaped(input);
            const bool truth=ref::language(ast,input).nodes[ast.root].at(0,input.size());++oracle_queries;
            const auto baseline=run(p,input,options(false));require(baseline.value.has_value(),"quota baseline incomplete");
            for(std::size_t cap=0;cap<=baseline.stats.work_steps+2;++cap) {
                auto o=options();o.max_steps=cap;pair(p,input,truth,o,true);++quota_pairs;
            }
            for(std::size_t cap=0;cap<=baseline.stats.states+2;++cap) {
                auto o=options();o.max_states=cap;pair(p,input,truth,o,true);++quota_pairs;
            }
        }
    }
}
void observer_contracts() {
    for(const auto& pattern:{templ,filter,assign}) {
        const auto ast=parse(pattern);const detail::AstMachineProgram p(ast);
        for(const auto& input:std::vector<std::string>{"","a","x=x","${x}","%{if x}%{end}"}) {
            context="observer exclusion / "+input;
            const auto relation=ref::language(ast,input);++oracle_queries;
            auto enabled=options(),disabled=options(false,false);OccurrenceStatistics stats;
            detail::AtomicExclusions exclusions(ast.nodes.size());
            require(p.full_match_assisted(input,enabled,exclusions,&stats)==relation.nodes[ast.root].at(0,input.size()),"assisted result changed");
            absent_new(stats);
            ++observer_queries;
            require(p.first_match(input,enabled,&stats)==p.first_match(input,disabled),"first witness changed");++observer_queries;
            absent_new(stats);
            for(std::size_t begin=0;begin<=input.size();++begin) {
                std::vector<std::size_t> ends;
                for(auto end=begin;end<=input.size();++end)if(relation.nodes[ast.root].at(begin,end))ends.push_back(end);
                require(p.endpoints(input,begin,enabled,&stats)==ends,"endpoint relation changed");++observer_queries;
                absent_new(stats);
            }
            std::vector<NodeId> entries;bool expected=false;
            for(auto entry:ast.capture_nodes)if(entry!=no_id&&entry!=ast.root) {
                require(p.full_match_node(entry,input,enabled,&stats)==relation.nodes[entry].at(0,input.size()),"nonroot entry changed");
                absent_new(stats);
                entries.push_back(entry);expected|=relation.nodes[entry].at(0,input.size());++observer_queries;
            }
            require(p.full_match_any(entries,input,enabled,&stats)==expected,"entry-union changed");++observer_queries;
            absent_new(stats);
        }
    }
    for(unsigned field=0;field<5;++field) {
        auto ast=parse(templ);auto& n=ast.nodes[ast.root];
        switch(field){case 0:n.bind_count=0;break;case 1:n.use_count=0;break;case 2:n.count_path=0;break;
        case 3:n.forward=true;break;default:n.kind=Kind::Reference;break;}
        ast.has_references=ast.has_counts=ast.has_extended=ast.has_occurrence_paths=false;
        context="effect field defeats stale flags "+std::to_string(field);
        require(!detail::NormalizedExactProgram(ast).ready(),"normalizer admitted observable effect");
    }
}

void cache_failures_and_limits() {
    context="negative native compile cache and source caps";
    const auto ast=parse(filter);const OccurrenceAnalysis analysis(ast);
    const detail::DeterministicProgram cert(ast,analysis);
    require(cert.certified(),"native negative-cache source not SLR");
    for(unsigned reason=0;reason<3;++reason) {
        detail::NativeSlrProgram::Options limited;
        if(reason==0)limited.executable_memory=false;
        if(reason==1)limited.max_code_bytes=0;
        if(reason==2)limited.max_analysis_steps=1;
        detail::ExactLoweringCache<detail::NativeSlrProgram> cache;
        require(!cache.get(cert,limited),"compiler limit/policy unexpectedly succeeded");
        const auto first=cache.statistics();
        require(first.attempted&&!first.ready,"failed native lowering not cached");
        require(!cache.get(cert),"negative result retried after caller changed options");
        const auto next=cache.statistics();
        require(first.compilation_steps==next.compilation_steps&&first.retained_bytes==next.retained_bytes,"negative cache changed");
        detail::DeterministicProgram::Runtime r;
        require(cert.execute("x=x",0,3,[]{},[]{},r),"native decline lost portable certificate");
    }
    // Exercise cache allocation-failure publication with the same production
    // generic once-only owner. It is a controlled throwing constructor, not an
    // assertion that the host operating system ran out of memory.
    struct Failing {
        explicit Failing(std::atomic_uint* attempts){++*attempts;throw std::bad_alloc{};}
        bool ready()const{return false;}std::size_t compilation_steps()const{return 0;}
        std::size_t retained_bytes()const{return 0;}std::size_t code_bytes()const{return 0;}
    };
    std::atomic_uint attempts{0};detail::ExactLoweringCache<Failing> failure;
    std::vector<std::thread> workers;
    for(unsigned i=0;i<8;++i)workers.emplace_back([&]{(void)failure.get(&attempts);});
    for(auto& t:workers)t.join();
    require(attempts==1&&failure.statistics().attempted&&!failure.statistics().ready,"allocation failure was not shared once");
    auto large=ast;large.nodes.resize(513);
    require(!detail::NormalizedExactProgram(large).ready(),"source-node normalizer cap ignored");
    auto wide=ast;wide.nodes[wide.root].children.resize(8193,0);
    require(!detail::NormalizedExactProgram(wide).ready(),"source-edge normalizer cap ignored");
    auto metadata=ast;metadata.capture_names.push_back(std::string(65537,'x'));
    require(!detail::NormalizedExactProgram(metadata).ready(),"source metadata cap ignored");
}

void concurrent_lifetime() {
    for(const auto& pattern:{templ,filter,assign}) {
        context="shared native/normalized immutable lifetime";
        auto ast=parse(pattern);const auto saved=ast;
        auto original=std::make_unique<detail::AstMachineProgram>(ast);
        auto survivor=*original;original.reset();ast.nodes.clear();ast.capture_nodes.clear();
        const std::vector<std::string> inputs{"","a","x=x","*x=x","((x=x))","${x}","%{if x}a%{else}b%{end}","%{if x}%{else}","a@"};
        std::vector<bool> truth;
        for(const auto& s:inputs){truth.push_back(ref::language(saved,s).nodes[saved.root].at(0,s.size()));++oracle_queries;}
        std::atomic_bool go{false};std::vector<std::thread> threads;std::vector<std::exception_ptr> errors(8);
        for(std::size_t t=0;t<errors.size();++t)threads.emplace_back([&,t] {
            while(!go.load(std::memory_order_acquire))std::this_thread::yield();
            try {
                auto copy=survivor;
                for(unsigned n=0;n<64;++n) {
                    const auto i=(n+t)%inputs.size();const auto& handle=n%2?copy:survivor;
                    if(handle.full_match(inputs[i],options(n%3!=0))!=truth[i])throw std::runtime_error("concurrent native answer changed");
                }
            }catch(...){errors[t]=std::current_exception();}
        });
        go.store(true,std::memory_order_release);for(auto& t:threads)t.join();
        for(const auto& error:errors)if(error)std::rethrow_exception(error);
        concurrent_queries+=errors.size()*64;
        for(std::size_t i=0;i<inputs.size();++i)pair(survivor,inputs[i],truth[i]);
    }
}
void preparation_and_cancellation() {
    context="normalization causal cache and native preparation";
    const auto ast=parse(templ);const detail::AstMachineProgram p(ast);
    auto stats=p.normalized_program_preparation_statistics();
    for(const auto& s:stats)require(!s.attempted,"normalization compiled eagerly");
    require(p.full_match("%{if x}a%{end}",options(false,false)),"old native fixture failed");
    for(const auto& s:p.normalized_program_preparation_statistics())require(!s.attempted,"disabled normalization compiled");
    for(unsigned mask=0;mask<4;++mask) {
        auto o=options(false);o.normalize_repetition_partitions=mask&1;o.normalize_transparent_calls=mask&2;
        require(p.full_match("%{if x}a%{end}",o),"normalization ablation changed language");
    }
    stats=p.normalized_program_preparation_statistics();
    require(stats[3].attempted&&stats[3].certified,"combined normalizations did not admit frozen template");
    require(!stats[1].certified&&!stats[2].certified,"single normalization unexpectedly admitted template; inspect causal claim");
    for(unsigned i=0;i<4;++i)require(p.full_match("%{if x}a%{end}",options()),"native cached normalized fixture failed");
    const auto compiled=p.compiled_program_preparation_statistics();
#if defined(__x86_64__) && defined(__linux__) && !defined(CRANK_DISABLE_NATIVE_SLR)
    require(compiled.normalized_native_slr[3].attempted&&compiled.normalized_native_slr[3].ready,"normalized native lowering not used");
#endif
    for(unsigned mask=0;mask<4;++mask) {
        const auto now=p.normalized_program_preparation_statistics()[mask];
        require(now.normalization_steps==stats[mask].normalization_steps&&now.slr.analysis_steps==stats[mask].slr.analysis_steps,
            "normalization cached result rebuilt");
    }
    for(const auto& [pattern,small]:std::vector<std::pair<std::string,std::string>>{{filter,"x=x"},{templ,"%{if x}a%{end}"}}) {
        context="cooperative cancellation / "+small;
        const auto source=parse(pattern);const detail::AstMachineProgram program(source);
        require(program.full_match(small,options()),"cancellation preparation failed");
        for(bool native:{false,true}) {
            std::atomic_bool stop{true};OccurrenceStatistics counters;bool caught=false;
            try{(void)program.full_match_node(source.root,small,options(native),&counters,&stop);}
            catch(const detail::AstExecutionCancelled&){caught=true;}
            require(caught&&!counters.limit_reached,"pre-cancelled query returned an answer/resource limit");++cancellations;
        }
        std::string input;
        if(pattern==filter)input=std::string(1'000'000,'(')+"x=x"+std::string(1'000'000,')');
        else for(unsigned i=0;i<120000;++i)input+="%{if x}a%{else}b%{end}";
        std::atomic_bool stop{false};OccurrenceStatistics counters;bool caught=false;
        std::thread interrupter([&]{std::this_thread::sleep_for(std::chrono::milliseconds(2));stop.store(true,std::memory_order_relaxed);});
        try{(void)program.full_match_node(source.root,input,options(),&counters,&stop);}
        catch(const detail::AstExecutionCancelled&){caught=true;}
        catch(...){interrupter.join();throw;}
        interrupter.join();
        require(caught&&!counters.limit_reached,"native running query did not cancel");
#if defined(__x86_64__) && defined(__linux__) && !defined(CRANK_DISABLE_NATIVE_SLR)
        require(counters.native_slr_attempts>0,"cancellation missed native code");++cancellations;
#else
        ++cancellations;
#endif
    }
}

// Native instructions are outside ASan instrumentation. These additional
// checks make the input boundaries inaccessible and verify immutable RX code.
void guard_pages() {
#if defined(__linux__) && defined(__x86_64__) && !defined(CRANK_DISABLE_NATIVE_SLR)
    const auto ast=parse(R"((?(DEFINE)(?<S>a(?&S)b|x))(?&S))");
    const OccurrenceAnalysis analysis(ast);
    const detail::DeterministicProgram source(ast,analysis);
    const detail::NativeSlrProgram native(source);context="native guard pages";
    require(native.ready(),"host native lowering unavailable in guard test");
    for(const auto [begin,end]:std::vector<std::pair<std::size_t,std::size_t>>{{1,0},{0,2},{SIZE_MAX,SIZE_MAX}}) {
        detail::DeterministicProgram::Runtime runtime;bool rejected=false;
        try{(void)native.execute("x",begin,end,[]{},[]{},runtime);}
        catch(const std::invalid_argument&){rejected=true;}
        require(rejected,"invalid exact span reached native code");
    }
    {
        detail::DeterministicProgram::Runtime runtime;
        require(!native.execute(std::string_view{},0,0,[]{},[]{},runtime),"null empty input changed language");
    }
    const auto code=native.code();const std::vector<unsigned char> original(code.begin(),code.end());
    bool found=false;std::ifstream maps("/proc/self/maps");std::string line;
    const auto address=reinterpret_cast<std::uintptr_t>(code.data());
    while(std::getline(maps,line)) {
        std::uintptr_t begin,end;char dash;std::string permissions;std::istringstream row(line);
        row>>std::hex>>begin>>dash>>end>>permissions;
        if(address>=begin&&address<end){require(permissions.size()>=3&&permissions[0]=='r'&&permissions[1]=='-'&&permissions[2]=='x',"published code violates W^X");found=true;break;}
    }
    require(found,"native mapping not found in /proc/self/maps");
    const auto page=static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
    void* region=::mmap(nullptr,page*3,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    require(region!=MAP_FAILED,"guard mapping failed");
    auto* middle=static_cast<char*>(region)+page;
    require(::mprotect(middle,page,PROT_READ|PROT_WRITE)==0,"guard writable middle failed");
    for(unsigned depth:{0U,1U,2U,31U,32U,63U,64U,65U,128U,511U}) {
        const std::string valid=std::string(depth,'a')+'x'+std::string(depth,'b');
        for(unsigned variant=0;variant<5;++variant) {
            std::string input=valid;
            if(variant==1)input.back()='a';
            if(variant==2)input.pop_back();
            if(variant==3)input[depth]='\0';
            if(variant==4)input.clear();
            for(bool right:{false,true}) {
                std::memset(middle,0xa5,page);auto* data=right?middle+page-input.size():middle;
                std::memcpy(data,input.data(),input.size());const std::string_view view(data,input.size());
                std::vector<unsigned char> bytes(reinterpret_cast<unsigned char*>(middle),reinterpret_cast<unsigned char*>(middle)+page);
                require(::mprotect(middle,page,PROT_READ)==0,"subject readonly protection failed");
                detail::DeterministicProgram::Runtime a,b;
                const bool expected=source.execute(view,0,view.size(),[]{},[]{},a);
                const bool actual=native.execute(view,0,view.size(),[]{},[]{},b);++native_calls;++guard_queries;
                require(actual==expected&&a.actions==b.actions&&a.shifts==b.shifts&&a.reductions==b.reductions&&a.peak_stack==b.peak_stack,"guard subject semantic mismatch");
                require(!std::memcmp(middle,bytes.data(),page),"subject or canary mutated");
                require(::mprotect(middle,page,PROT_READ|PROT_WRITE)==0,"subject reset protection failed");
            }
        }
    }
    require(!std::memcmp(code.data(),original.data(),original.size()),"shared native code changed across subjects");
    require(::munmap(region,page*3)==0,"guard unmap failed");
#endif
}
}
int main() {
    try {
        normalized_languages();generated_normalizers();quota_prefixes();observer_contracts();
        cache_failures_and_limits();concurrent_lifetime();guard_pages();preparation_and_cancellation();
        std::cout<<"{\"status\":\"passed\",\"templates_native_checks\":"<<checks
            <<",\"subjects\":"<<subjects<<",\"finite_relation_queries\":"<<oracle_queries
            <<",\"paired_queries\":"<<paired_queries<<",\"quota_pairs\":"<<quota_pairs
            <<",\"observer_queries\":"<<observer_queries<<",\"concurrent_queries\":"<<concurrent_queries
            <<",\"guard_page_queries\":"<<guard_queries<<",\"cancellations\":"<<cancellations
            <<",\"normalized_admissions\":"<<normalized_admissions<<",\"direct_native_calls\":"<<native_calls
            <<",\"generated_grammars\":"<<generated_grammars<<"}\n";
    }catch(const std::exception& error){std::cerr<<"TEMPLATES NATIVE FAILURE: "<<error.what()<<'\n';return 1;}
}
