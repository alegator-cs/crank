#include "ast_machine.hpp"
#include "crank.hpp"
#include "deterministic_program.hpp"
#include "bounded_word_regions.hpp"
#include "tests/helpers/relation_reference.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <thread>

// Independent semantic oracle: dense Boolean relations and their least fixed
// point. It does not use FIRST/FOLLOW, LR items, Crank analysis or metadata,
// optimized request identities, certificates, or native execution as truth.
using namespace crank;
namespace ref = certified_reference;
namespace {
std::string context;
std::size_t checks=0, subjects=0, oracle_queries=0, queries=0, observations=0;
std::size_t generated=0, word_attempts=0, exact_program_attempts=0;
std::size_t quota_queries=0, quota_completed=0, quota_limits=0, quota_retries=0;
void require(bool value,const std::string& why) {
    ++checks;
    if (!value) throw std::runtime_error(context+": "+why);
}

OccurrenceMatchOptions options(unsigned mode=0) {
    OccurrenceMatchOptions out;
    out.max_states=4'000'000;out.max_steps=32'000'000;
    // Keep the new root representations observable. Separate canonical gates
    // exercise the normal combination with every preceding representation.
    out.exact_end_spines=out.predictive_recursion=out.predictive_exact_rims=false;
    out.regional_execution=out.regional_constraints=false;
    out.bounded_word_prediction=mode!=2 && mode!=3;
    out.deterministic_programs=mode!=1 && mode!=3;
    return out;
}
void absent(const OccurrenceStatistics& s) {
    require(!s.bounded_word_attempts && !s.exact_program_attempts,
        "full-root Boolean representation escaped its observation scope");
}
void collect(const OccurrenceStatistics& s) {
    require(!s.limit_reached,"completed query retained inconclusive flag");
    require(!s.proof_nodes,"Boolean execution allocated witness history");
    word_attempts+=s.bounded_word_attempts;
    exact_program_attempts+=s.exact_program_attempts;
}
struct Fixture {
    std::string name, pattern, alphabet;
    unsigned maximum;
    std::vector<std::string> extra;
};
std::vector<Fixture> fixtures() {
    return {
        {"two-byte tag discriminators",R"((?(DEFINE)(?<S><a>(?&S)</a>|<b>(?&S)</b>|))(?&S))","<>ab/",3,
            {"<a></a>","<b></b>","<a><b></b></a>","<a><b></a></b>","<a></b>","<a></a>x"}},
        {"unbounded prefix before predicate discriminator",R"((?(DEFINE)(?<A>\((?&A)\)|x)(?<E>\((?&E)\)|(?&A)=(?&A)))(?&E))","()x=",4,
            {"x=x","(x)=(x)","((x=x))","((x))=x","((x))","(x=x)=x","((x)=x)","(x)=(x"}},
        {"left associative expression",R"((?(DEFINE)(?<E>(?&E)\+(?&T)|(?&T))(?<T>x|\((?&E)\)))(?&E))","x+()",4,
            {"x+x+x","(x+x)+x","((x))+x","x+(x+x)","x++x","(x+x"}},
        {"nullable left recursion",R"((?(DEFINE)(?<S>(?&S)a|))(?&S)b)","ab",5,{}},
        {"genuinely ambiguous split",R"((?(DEFINE)(?<S>(?&S)(?&S)|a))(?&S))","ab",5,{}},
        {"nullable repeat of recursive body",R"((?(DEFINE)(?<S>a(?&S)b|))(?:(?&S)?){1,3})","ab",5,{"aabbab","abaabb","aabbb"}},
        {"nullable unbounded body",R"((?(DEFINE)(?<S>a(?&S)b|))(?&S)*)","ab",5,{"aabbab","abaabb"}},
        {"nullable mutual closure",R"((?(DEFINE)(?<S>(?&T)|a)(?<T>(?&S)|))(?&S)b|(?&T)a)","ab",4,{}},
        {"unproductive mutual closure",R"((?(DEFINE)(?<S>(?&T))(?<T>(?&S)))(?&S)|x)","x",4,{}},
        {"mixed nullable and one-byte continuations",R"((?(DEFINE)(?<S>aa(?&S)|))(?&S)a?b)","ab",5,{}},
        {"repeat stop requires second byte",R"((?(DEFINE)(?<S>ab(?&S)|))(?&S)ac)","abc",4,{"ababac","abababac","abababc"}},
        {"finite repeat lower upper boundaries",R"((?(DEFINE)(?<S>ab(?&S)ba|x))(?&S)(?:ac){1,3}ab)","abcx",3,
            {"xacab","xacacab","xacacacab","xab","xacacacacab","abxbaacab"}},
        {"follow from multiple origins",R"((?(DEFINE)(?<S>aa(?&S)|))(?&S)ab|a(?&S)b)","ab",5,{}},
        {"same rule wrong exact end",R"((?(DEFINE)(?<S>a(?&S)b|x))(?&S)c|a(?&S)bc)","abcx",3,
            {"axbc","aaxbbc","aaxbcc","aaxbc","xc","x"}},
        {"structurally equal languages different context",R"((?(DEFINE)(?<A>aa(?&A)|x)(?<B>aa(?&B)|x))(?&A)ab|(?&B)ac)","abcx",3,
            {"xaaab","xaaac","aaxab","aaxac","aaxaa","x"}},
        {"two recursive children",R"((?(DEFINE)(?<S>a(?&S)b(?&S)|))(?&S)c)","abc",4,
            {"aabbabc","abaabbc","abababc","aabbbc"}},
        {"root recursion keeps original meaning",R"((?:aa(?R)b|x)(?:c|cc))","abcx",3,
            {"xc","aaxcbc","aaaaxcbcbbc","aaxbcc","aaxccbc"}},
        {"empty byte class and unreachable recursion",R"((?(DEFINE)(?<S>[^\x00-\xff](?&S)|ab))(?&S)c)","abc",4,{}},
        {"overlapping byte classes",R"((?(DEFINE)(?<S>[ab](?&S)c|[bc](?&S)d|x))(?&S))","abcdx",3,
            {"axc","bxd","bxc","cx d","abxd c"}},
        {"byte zero high byte and EOF",R"((?(DEFINE)(?<S>\x00a(?&S)\xff|\x00b(?&S)\xfe|))(?&S))",std::string("\0a\xff",3),3,
            {std::string("\0a\xff",3),std::string("\0b\xfe",3),std::string("\0a\0b\xfe\xff",6),std::string("\0a\0b\xff\xfe",6)}},
    };
}

void compare(const Ast& ast,const std::vector<std::string>& inputs,const std::string& name) {
    const detail::AstMachineProgram program(ast);
    const auto old_word=word_attempts,old_deterministic=exact_program_attempts;
    for (const auto& input:inputs) {
        context=name+" input="+ref::escaped(input);
        ++subjects;++oracle_queries;
        const auto relation=ref::language(ast,input);
        const bool expected=relation.nodes[ast.root].at(0,input.size());
        for (unsigned mode=0;mode<4;++mode) {
            OccurrenceStatistics stats;
            require(program.full_match(input,options(mode),&stats)==expected,
                "finite relation disagreement mode="+std::to_string(mode));
            ++queries;collect(stats);
            if (mode==1) require(!stats.exact_program_attempts,"disabled deterministic program entered");
            if (mode==2) require(!stats.bounded_word_attempts,"disabled bounded word program entered");
            if (mode==3) absent(stats);
        }
        if (input.size()>2) continue;
        detail::AtomicExclusions empty(ast.nodes.size());
        OccurrenceStatistics stats;
        require(program.full_match_assisted(input,options(),empty,&stats)==expected,"empty assistance changed language");
        absent(stats);++observations;
        for (std::size_t begin=0;begin<=input.size();++begin) {
            std::vector<std::size_t> ends;
            for (std::size_t end=begin;end<=input.size();++end)
                if (relation.nodes[ast.root].at(begin,end)) ends.push_back(end);
            require(program.endpoints(input,begin,options(),&stats)==ends,"endpoint relation narrowed by root demand");
            absent(stats);++observations;
        }
        require(program.first_match(input,options(),&stats)==program.first_match(input,options(3)),"first witness identity changed");
        absent(stats);++observations;
        std::vector<NodeId> entries;bool any=false;
        for (const auto entry:ast.capture_nodes) if (entry!=no_id && entry!=ast.root) {
            require(program.full_match_node(entry,input,options(),&stats)==relation.nodes[entry].at(0,input.size()),
                "named entry confused with root or consumer context");
            absent(stats);++observations;
            entries.push_back(entry);any|=relation.nodes[entry].at(0,input.size());
        }
        require(program.full_match_any(entries,input,options(),&stats)==any,"entry union lost origin correlation");
        absent(stats);++observations;
    }
    std::cout<<"FIXTURE "<<name<<" subjects="<<inputs.size()<<" bounded_word_attempts="<<word_attempts-old_word
        <<" exact_program_attempts="<<exact_program_attempts-old_deterministic<<'\n';
}

const std::string delayed=R"((?(DEFINE)(?<A>\((?&A)\)|x)(?<E>\((?&E)\)|(?&A)=(?&A)))(?&E))";
const std::string tags=R"((?(DEFINE)(?<S><a>(?&S)</a>|<b>(?&S)</b>|))(?&S))";

void populations() {
    context="late discriminator after unbounded shared prefix";
    const detail::AstMachineProgram program(parse(delayed));
    auto exact=options(2);exact.prune=false;
    for (std::size_t depth:{0U,1U,8U,64U,1024U}) {
        const auto left=std::string(depth,'(')+'x'+std::string(depth,')')+"=x";
        const auto outer=std::string(depth,'(')+"x=x"+std::string(depth,')');
        for (const auto& good:{left,outer}) {
            OccurrenceStatistics stats;
            require(program.full_match(good,exact,&stats),"late discriminator accepted word rejected");
            require(stats.exact_program_attempts>0,"delayed decision fixture did not enter deterministic program");
            require(stats.memo_entries==1 && !stats.return_links && stats.native_result_slots<=1,
                "exact program retained native interior request/return/results population");
            require(!program.full_match(good+"xx",exact,&stats),"late malformed suffix accepted");
            require(stats.exact_program_attempts>0,"late failure bypassed deterministic program");
            queries+=2;
        }
    }
    context="two byte tag program removes native population";
    const detail::AstMachineProgram tag_program(parse(tags));
    OccurrenceStatistics on,off;
    require(tag_program.full_match("<a><b></b></a>",options(1),&on),"tag program rejected balanced tag");
    require(tag_program.full_match("<a><b></b></a>",options(3),&off),"preceding tag recognizer rejected balanced tag");
    require(on.bounded_word_attempts>0,"tag fixture did not enter word prediction");
    require(on.memo_entries==1 && !on.return_links && on.native_result_slots<=1 && off.memo_entries>1,
        "word certificate did not physically remove generalized population");
    context="ambiguous split must decline deterministic certificate";
    const detail::AstMachineProgram ambiguous(parse(R"((?(DEFINE)(?<S>(?&S)(?&S)|a))(?&S))"));
    require(ambiguous.full_match("aaa",options(),&on),"ambiguous language lost accepted word");
    require(!on.bounded_word_attempts && !on.exact_program_attempts,"ambiguous split admitted a deterministic certificate");
    require(on.memo_entries>0,"ambiguous split did not retain generalized recognition");
    queries+=3;
}

void budgets() {
    for (const auto& [pattern,inputs]:std::vector<std::pair<std::string,std::vector<std::string>>>{
            {tags,{"","<a></a>","<a></b>","<a><b></b></a>"}},
            {delayed,{"x=x","(x)=x","((x=x))","((x))"}}}) {
        const detail::AstMachineProgram program(parse(pattern));
        for (const auto& input:inputs) {
            context="full fallback allowance pattern="+pattern+" input="+input;
            OccurrenceStatistics prior;
            const bool truth=program.full_match(input,options(3),&prior);
            for (const auto states:std::vector<std::size_t>{0,1,2,4,8,16,32,prior.states})
                for (const auto work:std::vector<std::size_t>{0,1,2,8,32,64,128,prior.work_steps}) {
                    auto previous=options(3);previous.max_states=states;previous.max_steps=work;
                    std::optional<bool> preceding;
                    try {preceding=program.full_match(input,previous);}catch(const ResourceLimit&) {}
                    auto now=previous;now.bounded_word_prediction=now.deterministic_programs=true;
                    OccurrenceStatistics stats;
                    ++quota_queries;
                    try {
                        require(program.full_match(input,now,&stats)==truth,"quota exhaustion became wrong Boolean");
                        ++quota_completed;
                    }catch(const ResourceLimit&) {
                        ++quota_limits;
                        require(!preceding.has_value(),"optional representation stole successful preceding allowance");
                    }
                    require(stats.native_bounded_word_fallbacks<=1 && stats.native_exact_program_fallbacks<=1,
                        "exact program retry repeated");
                    quota_retries+=stats.native_bounded_word_fallbacks+stats.native_exact_program_fallbacks;
                }
        }
    }
    require(quota_completed && quota_limits && quota_retries,"quota matrix failed to exercise success, limits and retry");
}

void cancellation() {
    for (unsigned mode:{1U,2U}) {
        context=mode==1?"cancellation inside bounded word executor":"cancellation inside deterministic executor";
        const auto ast=parse(mode==1?tags:delayed);
        const detail::AstMachineProgram program(ast);
        const auto small=mode==1?"<a></a>":"x=x";
        require(program.full_match(small,options(mode)),"warm cancellation fixture failed");
        std::string input;
        if (mode==1) {
            for (unsigned i=0;i<250'000;++i) input+="<a>";
            for (unsigned i=0;i<250'000;++i) input+="</a>";
        }else input=std::string(1'000'000,'(')+"x=x"+std::string(1'000'000,')');
        auto option=options(mode);option.max_states=64'000'000;option.max_steps=256'000'000;
        std::atomic_bool cancel{false};OccurrenceStatistics stats;bool cancelled=false;
        std::thread interrupter([&] {std::this_thread::sleep_for(std::chrono::milliseconds(3));cancel.store(true);});
        try {(void)program.full_match_node(ast.root,input,option,&stats,&cancel);}
        catch(const detail::AstExecutionCancelled&) {cancelled=true;}
        catch(...) {interrupter.join();throw;}
        interrupter.join();
        require(cancelled,"cancelled exact execution returned a Boolean or completed without polling");
        require(mode==1?stats.bounded_word_attempts>0:stats.exact_program_attempts>0,
            "timed cancellation never entered tested executor");
        require(!stats.limit_reached,"cancellation mislabeled as resource exhaustion");
        std::cout<<"CANCELLATION mode="<<mode<<" bounded_word_attempts="<<stats.bounded_word_attempts
            <<" exact_program_attempts="<<stats.exact_program_attempts<<'\n';
    }
}

void shared_preparation() {
    for (const auto& pattern:{tags,delayed,std::string(R"((?(DEFINE)(?<S>(?&S)(?&S)|a))(?&S))")}) {
        context="concurrent positive and negative certificate preparation";
        const auto ast=parse(pattern);const detail::AstMachineProgram program(ast);
        const std::vector<std::string> inputs{"","a","aaa","<a></a>","<a></b>","x=x","((x))=x","((x=x))","(x=x)=x"};
        std::vector<bool> truth;
        for (const auto& input:inputs) {
            truth.push_back(ref::language(ast,input).nodes[ast.root].at(0,input.size()));++oracle_queries;
        }
        std::atomic_bool go{false};std::vector<std::exception_ptr> errors(8);std::vector<std::thread> workers;
        for (std::size_t worker=0;worker<errors.size();++worker) workers.emplace_back([&,worker] {
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            try {for (unsigned iteration=0;iteration<64;++iteration) {
                const auto index=(iteration+worker)%inputs.size();
                if (program.full_match(inputs[index],options())!=truth[index])
                    throw std::runtime_error("shared certificate or decline corrupted another query");
            }}catch(...) {errors[worker]=std::current_exception();}
        });
        go.store(true,std::memory_order_release);for (auto& worker:workers) worker.join();
        for (const auto& error:errors) {if (error) std::rethrow_exception(error);require(!error,"worker failed");}
        queries+=64*errors.size();checks+=64*errors.size();
    }
    std::cout<<"SHARED_PREPARATION programs=3 threads_per_program=8 queries=1536\n";
}

void preparation_limits() {
    context="source edge cap precedes unbounded source traversal";
    Ast oversized;
    const auto empty=ref::add(oversized,Kind::Empty);
    oversized.root=ref::add(oversized,Kind::Concat,std::vector<NodeId>(20'000,empty));
    const OccurrenceAnalysis analysis(oversized);
    const detail::DeterministicProgram large_exact(oversized,analysis);
    const detail::BoundedWordRegion large_word(oversized,analysis,false,true);
    require(!large_exact.certified() && large_exact.statistics().limited,"many duplicate child edges escaped SLR cap");
    require(!large_word.certified && large_word.limit_reached,"many duplicate child edges escaped word cap");
    require(large_exact.statistics().analysis_steps<100 && large_word.analysis_steps<100,
        "source edge cap scanned the oversized child vector before declining");
    context="bounded failed deterministic compilation preserves native language";
    const auto ast=parse(R"((?(DEFINE)(?<S>a(?&S)b|x))(?&S)c{0,33})");
    const detail::AstMachineProgram program(ast);
    const auto unused=program.exact_program_preparation_statistics();
    require(!unused.attempted,"optional certificate prepared before first query");
    for (const auto& input:std::vector<std::string>{"x","xc","axbcc","aaxbbccc","axbcx"}) {
        const bool truth=ref::language(ast,input).nodes[ast.root].at(0,input.size());++oracle_queries;
        OccurrenceStatistics stats;
        require(program.full_match(input,options(2),&stats)==truth,"compile cap became rejection");++queries;
        require(!stats.exact_program_attempts,"capped deterministic certificate executed");
    }
    const auto first=program.exact_program_preparation_statistics();
    require(first.attempted && first.limited && !first.certified,"finite-repeat cap fixture did not hit bounded preparation");
    require(first.analysis_steps>0 && first.analysis_steps<=1'048'576,"compiler work not bounded");
    require(program.full_match("x",options(2)),"cached negative certificate changed language");++queries;
    const auto second=program.exact_program_preparation_statistics();
    require(second.analysis_steps==first.analysis_steps && second.retained_bytes==first.retained_bytes,
        "negative certificate prepared repeatedly or accumulated retained storage");
    context="cheap one byte prediction does not pay new compilation";
    const detail::AstMachineProgram cheap(parse(R"((?(DEFINE)(?<S>a(?&S)b|x))(?&S))"));
    require(cheap.full_match("aaxbb"),"cheap one byte fixture rejected");++queries;
    require(!cheap.exact_program_preparation_statistics().attempted,"cheap preceding route paid deterministic compilation");
}

void effects_and_public_api() {
    context="observable effects defeat cached flags";
    require(!detail::AstMachineProgram::eligible(parse(R"((?<S>aa(?&S)b|x)\k<S>)")),"backreference admitted pure program");
    auto ast=parse(tags);ast.has_counts=ast.has_references=ast.has_calls=false;
    ast.nodes[ast.root].bind_count=0;
    require(!detail::AstMachineProgram::eligible(ast),"stale flags hid count effect");
    for (const auto& pattern:{tags,delayed}) {
        const Program program(pattern);context="public Auto agrees with independent relation";
        for (const auto& input:std::vector<std::string>{"","x=x","((x=x))","<a></a>","<a></b>","(x=x)=x"}) {
            const bool truth=ref::language(program.ast(),input).nodes[program.ast().root].at(0,input.size());++oracle_queries;
            require(program.full_match(input)==truth,"public Auto answer differs");++queries;
        }
    }
}
} // namespace

int main() {
    try {
        const auto all=fixtures();
        for (const auto& fixture:all) {
            auto inputs=ref::words(fixture.alphabet,fixture.maximum);
            inputs.insert(inputs.end(),fixture.extra.begin(),fixture.extra.end());
            compare(parse(fixture.pattern),inputs,fixture.name);
        }
        ref::Random random{UINT64_C(0x793fb211b0c58217)};
        for (unsigned n=0;n<24;++n) {
            auto fixture=ref::random_fixture(random,n,4);++generated;
            compare(fixture.ast,fixture.inputs,fixture.name);
        }
        populations();budgets();cancellation();shared_preparation();preparation_limits();effects_and_public_api();
        require(word_attempts && exact_program_attempts,"oracle sweep never exercised both exact programs");
        std::cout<<"{\"status\":\"passed\",\"fixtures\":"<<all.size()<<",\"generated_grammars\":"<<generated
            <<",\"subjects\":"<<subjects<<",\"checks\":"<<checks<<",\"finite_relation_queries\":"<<oracle_queries
            <<",\"production_sweep_queries\":"<<subjects*4<<",\"concurrent_queries\":1536,\"observation_checks\":"<<observations
            <<",\"bounded_word_attempts\":"<<word_attempts<<",\"exact_program_attempts\":"<<exact_program_attempts
            <<",\"quota_queries\":"<<quota_queries<<",\"quota_completed\":"<<quota_completed
            <<",\"quota_limits\":"<<quota_limits<<",\"quota_retries\":"<<quota_retries<<"}\n";
    }catch(const std::exception& error) {std::cerr<<"EXACT PROGRAM FAILURE: "<<error.what()<<'\n';return 1;}
}
