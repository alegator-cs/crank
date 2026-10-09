#include "ast_machine.hpp"
#include "crank.hpp"
#include "tests/helpers/relation_reference.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <thread>

// This suite compares live production recognition with a dense denotational
// least fixed point. The oracle does not know regional certificates, consumer
// scopes, boundary propagation, request keys, or publication scheduling.
using namespace crank;
namespace ref = certified_reference;
namespace {
std::string context;
std::size_t checks=0, inputs=0, oracle_queries=0, production_queries=0;
std::size_t attempts=0, successes=0, deferred=0, published=0;
std::size_t quota_queries=0, quota_completed=0, quota_limits=0, regional_retries=0;
std::size_t observation_checks=0, generated_grammars=0;

void require(bool value, const std::string& why) {
    ++checks;
    if (!value) throw std::runtime_error(context + ": " + why);
}

struct Fixture {
    const char* name;
    const char* pattern;
    std::string alphabet;
    unsigned maximum;
    std::vector<std::string> extra;
};

std::vector<Fixture> fixtures() {
    return {
        {"recursive region before live consumer", R"((?(DEFINE)(?<S>a(?&S)b|x))(?&S);|(?&S),)", "abx;,", 3,
            {"aaxbb;", "aaxbb,", "aaxb;", "aaxbbb;", "aaaaxbbbb;"}},
        {"future five and eight coexist with ordinary next byte", R"((?(DEFINE)(?<S>a(?&S)b|x)(?<T>a(?&T)b|xbbb))(?&S)b*b;|(?&T)b*b;|aaxbbbbbb;)", "abx;", 2,
            {"aaxbbbbbb;", "axbbbbb;", "xbbbb;", "aaxbbbbbb", "aaxbbbbb;", "aaybbbbbb;"}},
        {"old result new subscriber and nullable end zero", R"((?(DEFINE)(?<S>a(?&S)b|))(?&S);|(?&S)(?&S);|(?:(?&S));)", "ab;", 4,
            {";", "aabb;", "abab;", "aabbab;", "aab;"}},
        {"epsilon unit chain and delayed caller", R"((?(DEFINE)(?<S>a(?&S)b|)(?<U>(?&S)))(?&S);|(?&U);|(?&S)(?&U);)", "ab;", 4, {}},
        {"productive right recursion unique consumer exit", R"((?(DEFINE)(?<S>a(?&S)|))(?&S);|(?&S),)", "a;,", 4,
            {"aaaaaaaa;", "aaaaaaa,", "aaaaa"}},
        {"broad follow forbids greedy consumer narrowing", R"((?(DEFINE)(?<S>a(?&S)|))(?&S)a;|(?&S);)", "a;", 5,
            {"aaaaaaaaa;", ";", "a;"}},
        {"shared recursive producer incompatible consumers", R"((?(DEFINE)(?<S>a(?&S)|b))(?&S);|(?&S)a;|a(?&S),)", "ab;,", 3,
            {"aaaaba;", "aaaab;", "aaaab,", "aaba;", "aaab"}},
        {"shared regular producer requires earlier endpoint", R"((?(DEFINE)(?<S>[a-c]+))(?&S)c;|(?&S);)", "abc;", 4,
            {"abbc;", "abc;", "ac;", "c;"}},
        {"equivalent sources have different consumer contracts", R"((?(DEFINE)(?<A>\((?&A)\)|a+)(?<B>\((?&B)\)|a+))(?:(?&A)x|(?&B)a))", "a()x", 4,
            {"aa", "aaa", "aax", "(aa)a", "((a))x", "((a))a", "(a)x"}},
        {"left recursive outer fixed point crossing right recursive island", R"((?(DEFINE)(?<I>a(?&I)b|x)(?<L>(?&L);(?&I)|(?&I)))(?&L)!)", "ax;!b", 3,
            {"x!", "x;axb!", "aaxbb;x;axb!", "x;;x!", "x;ab!"}},
        {"left recursion followed by consumer must decline prediction", R"((?(DEFINE)(?<S>(?&S)a|b))(?&S);|(?&S),)", "ab;,", 3,
            {"baaaaa;", "baaaa,", "aaaa;"}},
        {"nullable left recursive component", R"((?(DEFINE)(?<S>(?&S)a|))(?&S);|(?&S)a;)", "a;", 5, {}},
        {"unproductive mutual component", R"((?(DEFINE)(?<S>(?&T))(?<T>(?&S)))(?&S);|x)", "x;", 3, {}},
        {"nullable mutual component with exits", R"((?(DEFINE)(?<S>(?&T)|a)(?<T>(?&S)|))(?&S);|(?&T),)", "a;,", 4, {}},
        {"two recursive children preserve endpoint relation", R"((?(DEFINE)(?<S>a(?&S)b(?&S)|))(?&S);|(?&S),)", "ab;", 4,
            {"aabbab;", "abaabb;", "ababab;", "aabbb;"}},
        {"multiple sequential regional invocations", R"((?(DEFINE)(?<S>a(?&S)b|x))(?&S);(?&S),|(?&S),(?&S);)", "abx;,", 2,
            {"aaxbb;axb,", "x,aaaxbbb;", "x;x,", "x,", "aaxbb;ab,"}},
        {"distinct origins overlap future completions", R"((?(DEFINE)(?<S>a(?&S)b|x))(?&S);|a(?&S)b;|aa(?&S)bb;)", "abx;", 3,
            {"aaaxbbb;", "aaxbb;", "axbb;", "aaaxbb;"}},
        {"empty input nullable export", R"((?(DEFINE)(?<S>a(?&S)b|))(?&S)|(?&S)(?&S))", "ab", 5, {}},
        {"binary consumer bytes", R"((?(DEFINE)(?<S>[\x00\xff](?&S)a|x))(?&S)\xff|(?&S)\x00)", std::string("a\0\xffx",4), 3,
            {std::string("\0\xffxaa\xff",6), std::string("\0\xffxax\xff",6)}},
        {"original root recursion under local entry", R"((?:a(?R)b|x)(?:q|qq))", "abxq", 4,
            {"xq", "axqbq", "axqqbqq", "aaxqbqbqq", "axbq", "aaxbbq"}},
        {"original nullable root recursion", R"((?:a(?R)b|)(?:x|xx))", "abx", 4,
            {"x", "axbxx", "aaxbxbxx", "aaxbbx"}},
        {"finite widths and recursive residue overlap", R"((?(DEFINE)(?<R>x(?&R)qq|))(?&R)(?:q|qq|qqqq))", "xq", 6,
            {"xxqqqqq", "xxqqqqqq", "xxqqqqqqqq", "xqqqqqq", "xxqqxqq"}},
        {"two unknown boundaries independent holdout", R"((?(DEFINE)(?<S>aaaa(?&S)bb|bb))(?&S)(?:bbb|bbbb)(?:b|bbb))", "ab", 6,
            {"aaaabbbbbbbb", "aaaabbbbbbbbb", "aaaabbbbbbb", "aaabbbbbbbbb"}},
        {"holdout inside ordinary root alternative", R"((?(DEFINE)(?<S>aaaa(?&S)bb|bb))(?:(?&S)(?:bbb|bbbb)(?:b|bbb)|aaz))", "abz", 4,
            {"aaaabbbbbbbb", "aaaabbbbbbbbb", "aaz", "aaaabbbbbbbz"}},
        {"native prefix can make earlier byte facts redundant", R"((?(DEFINE)(?<R>u(?&R)vvvv|u(?&R)wwww|))(?:aa){1,2}(?&R)(?:www){1,2})", "auvw", 3,
            {"aaaauwwwwwwwwww", "aaaauvvvvwww", "aauuwwwwvvvvwww", "aaaauwwwvwww"}},
        {"byte and length support jointly necessary inside native", R"((?(DEFINE)(?<R>u(?&R)vvvv|u(?&R)wwww|))(?&R)(?:w{2}|w{3}|w{8}))", "uvw", 4,
            {"uuvvvvwwwwwww", "uuwwwwvvvvwww", "uuvvvvwwwwww", "uuvvvvwwwwwwww", "uuvvvxwwwwwww"}},
        {"nullable finite remaining widths", R"((?(DEFINE)(?<S>a(?&S)a|))(?&S)a{0,2})", "ab", 6, {}},
        {"unbounded remaining widths retain unknown", R"((?(DEFINE)(?<S>a(?&S)b|))(?&S)b*)", "ab", 6, {}},
        {"bounded repeat nullable atom", R"((?(DEFINE)(?<S>a(?&S)b|))(?&S)(?:x?){2})", "abx", 4,
            {"aabbxx", "aaabbbx", "aabbxxx"}},
        {"empty class unreachable arm", R"((?(DEFINE)(?<S>a(?&S)b|))(?&S)(?:[^\x00-\xff]|x|xx))", "abx", 3, {}},
        {"mutual typed recursive island", R"((?(DEFINE)(?<S>a(?&T)b|x)(?<T>c(?&S)d|y))(?&S);|(?&S),)", "abcdxy;", 2,
            {"ayb;", "acxdb,", "acaybdb;", "acxddb;"}},
    };
}

OccurrenceMatchOptions options(unsigned ablation=0) {
    OccurrenceMatchOptions out;
    // Root accelerators would hide the bridge under test. The public Auto
    // regression gate independently verifies their normal enabled behavior.
    out.bounded_word_prediction=out.deterministic_programs=false;
    out.exact_end_spines=false;
    out.predictive_recursion=false;
    out.predictive_exact_rims=false;
    out.max_states=4'000'000;
    out.max_steps=32'000'000;
    if (ablation==1) out.regional_execution=false;
    if (ablation==2) out.regional_consumers=false;
    if (ablation==3) out.regional_constraints=false;
    if (ablation==4) out.regional_length_facts=false;
    if (ablation==5) out.regional_byte_facts=false;
    return out;
}

void observe(const OccurrenceStatistics& stats) {
    attempts+=stats.regional_attempts;
    successes+=stats.regional_successes;
    deferred+=stats.regional_deferred_completions;
    published+=stats.regional_published_completions;
    require(!stats.limit_reached, "completed Boolean retains limit flag");
    require(!stats.proof_nodes, "pure Boolean allocated witness proof history");
}

void compare(const Ast& ast, const std::vector<std::string>& subjects, const std::string& name) {
    const detail::AstMachineProgram program(ast);
    const auto before=attempts;
    for (const auto& subject:subjects) {
        context=name+" input="+ref::escaped(subject);
        ++inputs;
        const auto relation=ref::language(ast,subject);
        ++oracle_queries;
        const bool truth=relation.nodes[ast.root].at(0,subject.size());
        for (unsigned ablation=0;ablation<6;++ablation) {
            OccurrenceStatistics stats;
            require(program.full_match(subject,options(ablation),&stats)==truth,
                "finite-relation disagreement, ablation="+std::to_string(ablation));
            ++production_queries;
            observe(stats);
            if (ablation==1) require(!stats.regional_attempts,"disabled regional execution activated");
        }
        if (subject.size()<=2) {
            detail::AtomicExclusions empty(ast.nodes.size());
            OccurrenceStatistics assisted_stats;
            require(program.full_match_assisted(subject,options(),empty,&assisted_stats)==truth,
                "empty assistance changed admitted relation");
            require(!assisted_stats.regional_attempts,"root regional scope leaked to assisted execution");
            ++observation_checks;
            for (std::size_t begin=0;begin<=subject.size();++begin) {
                std::vector<std::size_t> ends;
                for (std::size_t end=begin;end<=subject.size();++end)
                    if (relation.nodes[ast.root].at(begin,end)) ends.push_back(end);
                OccurrenceStatistics stats;
                require(program.endpoints(subject,begin,options(),&stats)==ends,"endpoint export narrowed by root consumer scope");
                require(!stats.regional_attempts,"Boolean region entered endpoint export");
                ++observation_checks;
            }
            OccurrenceStatistics stats;
            require(program.first_match(subject,options(),&stats)==program.first_match(subject,options(1)),
                "first witness/capture identity changed");
            require(!stats.regional_attempts,"Boolean region entered first witness export");
            ++observation_checks;
            std::vector<NodeId> entries;
            bool any=false;
            for (const auto entry:ast.capture_nodes) if (entry!=no_id) {
                require(program.full_match_node(entry,subject,options(),&stats)==relation.nodes[entry].at(0,subject.size()),
                    "entry-specific demanded relation changed");
                if (entry!=ast.root) require(!stats.regional_attempts,"root consumer scope leaked to nonroot query");
                entries.push_back(entry); any |= relation.nodes[entry].at(0,subject.size());
                ++observation_checks;
            }
            require(program.full_match_any(entries,subject,options(),&stats)==any,"entry union changed");
            require(!stats.regional_attempts,"root consumer scope leaked to entry union");
            ++observation_checks;
            for (unsigned mode=0;mode<7;++mode) {
                auto value=options();
                if (mode==0) value.collapse_control=value.merge_equivalent_nodes=false;
                if (mode==1) value.compress_returns=false;
                if (mode==2) value.local_regular_regions=false;
                if (mode==3) value.aggregate_returns=false;
                if (mode==4) value.frontier_indexes=false;
                if (mode==5) value.implicit_request_agenda=false;
                if (mode==6) value.compact_closed_results=false;
                require(program.full_match(subject,value)==truth,"scheduler/storage ablation changed language");
                ++production_queries;
            }
        }
    }
    std::cout<<"FIXTURE "<<name<<" subjects="<<subjects.size()<<" regional_attempts="<<attempts-before<<'\n';
}

void budgets() {
    const std::vector<std::pair<std::string,std::vector<std::string>>> all{
        {R"((?(DEFINE)(?<S>a(?&S)b|x))(?&S);|(?&S),)",{"x;","aaxbb;","aaxb;","aaxbb,"}},
        {R"((?(DEFINE)(?<S>a(?&S)|))(?&S);|(?&S),)",{";","aaaa;","aaaa,","aaa"}},
        {R"((?(DEFINE)(?<S>aaaa(?&S)bb|bb))(?&S)(?:bbb|bbbb)(?:b|bbb))",{"bbbbbb","aaaabbbbbbbb","aaabbbbbbbb"}},
        {R"((?(DEFINE)(?<I>a(?&I)b|x)(?<L>(?&L);(?&I)|(?&I)))(?&L)!)",{"x!","x;axb!","x;;x!"}},
    };
    for (const auto& [pattern,subjects]:all) {
        const detail::AstMachineProgram program(parse(pattern));
        for (const auto& subject:subjects) {
            context="budget pattern="+pattern+" input="+subject;
            OccurrenceStatistics old_stats;
            auto preceding=options(1);preceding.regional_constraints=false;
            const bool truth=program.full_match(subject,preceding,&old_stats);
            const std::vector<std::size_t> state_caps{0,1,2,3,4,8,16,32,old_stats.states};
            const std::vector<std::size_t> work_caps{0,1,2,4,8,16,32,64,old_stats.work_steps};
            for (auto states:state_caps) for (auto work:work_caps) {
                auto old=preceding;old.max_states=states;old.max_steps=work;
                auto now=old;now.regional_execution=true;now.regional_constraints=true;
                std::optional<bool> previous;
                try { previous=program.full_match(subject,old); } catch(const ResourceLimit&) {}
                OccurrenceStatistics stats;
                ++quota_queries;
                try {
                    require(program.full_match(subject,now,&stats)==truth,"resource bound produced wrong Boolean");
                    ++quota_completed;
                } catch(const ResourceLimit&) {
                    ++quota_limits;
                    require(!previous.has_value(),"optional regional attempt stole successful fallback allowance");
                }
                require(stats.native_regional_fallbacks<=1,"regional retry repeated");
                regional_retries+=stats.native_regional_fallbacks;
            }
        }
    }
}

void publication_protocol() {
    context="two future endpoints coexist with every ordinary byte frontier";
    const detail::AstMachineProgram program(parse(R"((?(DEFINE)(?<S>a(?&S)b|x)(?<T>a(?&T)b|xbbb))(?&S)b*b;|(?&T)b*b;|aaxbbbbbb;)"));
    const std::string subject="aaxbbbbbb;";
    OccurrenceStatistics stats;
    require(program.full_match(subject,options(),&stats),"accepted competing futures rejected");
    // S completes at 5, T at 8. Their b*b; continuations cannot themselves
    // certify predictively because the repeat and its consumer both begin b.
    // The regular third arm keeps ordinary work alive through position 10.
    require(stats.max_regional_pending>=2,"fixture never owned two simultaneous future completions");
    require(stats.regional_published_completions>=2,"both future results were not delivered");
    require(stats.ordered_positions==subject.size()+1 && !stats.regional_frontier_jumps,
        "regional future bypassed an ordinary live frontier");
    std::cout<<"PUBLICATION max_future="<<stats.max_regional_pending
        <<" deferred="<<stats.regional_deferred_completions<<" published="<<stats.regional_published_completions
        <<" positions="<<stats.ordered_positions<<'\n';

    context="immediate nullable regional result and late left-recursive subscriber";
    const detail::AstMachineProgram nullable(parse(R"((?(DEFINE)(?<S>a(?&S)b|)(?<U>(?&S)|(?&U)))(?&S)(?:;|;;)|(?&U)(?:;|;;;))"));
    auto option=options();
    option.inline_nullable_frontiers=false;
    option.collapse_control=option.merge_equivalent_nodes=false;
    require(nullable.full_match(";",option,&stats),"nullable base was lost through a recursive late subscriber");
    require(stats.regional_attempts>0 && stats.regional_published_completions>0,
        "nullable fixture never published an immediate regional result");
    require(!stats.regional_deferred_completions,"nullable endpoint zero was improperly deferred");
    require(stats.replay_steps>0,"nullable fixture never replayed an old result to a later subscriber");
    std::cout<<"NULLABLE_PUBLICATION published="<<stats.regional_published_completions
        <<" replay_steps="<<stats.replay_steps<<'\n';
}

void live_constraint_chain() {
    const std::vector<std::pair<std::string,std::string>> cases{
        {R"((?(DEFINE)(?<R>u(?&R)vvvv|u(?&R)wwww|))(?&R)(?:w{2}|w{3}|w{8}))", "uuvvvvwwwwwww"},
        {R"((?(DEFINE)(?<R>u(?&R)vvvv|u(?&R)wwww|))(?&R)(?:w{3}|w{6}|w{11}))", "uuu"+std::string(12,'v')+std::string(6,'w')},
    };
    for (const auto& [pattern,subject]:cases) {
        context="live heterogeneous inference chain input="+subject;
        const auto ast=parse(pattern);
        const detail::AstMachineProgram program(ast);
        require(ref::language(ast,subject).nodes[ast.root].at(0,subject.size()),"chain fixture is not in independent language");
        ++oracle_queries;
        OccurrenceStatistics all, no_execution;
        require(program.full_match(subject,options(),&all),"live inferred exact demand rejected");
        require(all.regional_constraint_refinements>0,"live chain did not refine boundary facts");
        require(all.regional_spine_attempts>0,"inferred regional span never activated a spine");
        require(all.regional_published_completions>0,"regional answer never returned to ordinary native execution");
        for (unsigned mode:{3U,4U,5U}) {
            OccurrenceStatistics weak;
            require(program.full_match(subject,options(mode),&weak),"causal ablation changed recognition");
            require(!weak.regional_attempts,"claimed necessary fact family was redundant for regional activation, mode="+std::to_string(mode));
        }
        require(program.full_match(subject,options(1),&no_execution),"exact boundary facts require regional execution for correctness");
        require(no_execution.regional_constraint_refinements>0,"execution-off erased logically independent constraint propagation");
        require(!no_execution.regional_attempts,"execution-off activated regional executor");
        require(no_execution.memo_entries>all.memo_entries,"regional chain did not physically remove producer requests");
        std::cout<<"LIVE_CHAIN length="<<subject.size()<<" refinements="<<all.regional_constraint_refinements
            <<" regional_spines="<<all.regional_spine_attempts<<" requests="<<all.memo_entries
            <<" execution_off_requests="<<no_execution.memo_entries<<'\n';
    }
}

void cancellation() {
    context="cancellation inside regional work";
    const auto ast=parse(R"((?(DEFINE)(?<S>a(?&S)b|x))(?&S);|(?&S),)");
    const detail::AstMachineProgram program(ast);
    const std::string subject=std::string(2'000'000,'a')+'x'+std::string(2'000'000,'b')+';';
    auto option=options();option.max_states=64'000'000;option.max_steps=256'000'000;
    std::atomic_bool cancel{false};
    OccurrenceStatistics stats;
    bool cancelled=false;
    // Publication is single-threaded; the sole shared datum here is the
    // documented cooperative cancellation atomic. Delaying avoids merely
    // exercising the already-covered cancellation-before-start path.
    std::thread interrupter([&] { std::this_thread::sleep_for(std::chrono::milliseconds(3));cancel.store(true); });
    try { (void)program.full_match_node(ast.root,subject,option,&stats,&cancel); }
    catch(const detail::AstExecutionCancelled&) {cancelled=true;}
    catch(...) {interrupter.join();throw;}
    interrupter.join();
    require(cancelled,"cancelled execution returned a Boolean");
    require(stats.regional_attempts>0,"timed cancellation never entered a regional attempt");
    require(!stats.limit_reached,"cancellation mislabeled as resource exhaustion");
    std::cout<<"CANCELLATION regional_attempts="<<stats.regional_attempts<<" regional_steps="<<stats.regional_steps<<'\n';
}

void shared_preparation() {
    context="concurrent first query of one immutable source program";
    const auto ast=parse(R"((?(DEFINE)(?<S>a(?&S)b|x)(?<T>a(?&T)b|xbbb))(?&S)b*b;|(?&T)b*b;|aaxbbbbbb;)" );
    const detail::AstMachineProgram program(ast);
    const std::vector<std::string> subjects{"xbbb;","axbb;","aaxbbbbbb;","aaxbbbb;","aaxbbb;","aaaxbb;",";","x"};
    std::vector<bool> expected;
    for (const auto& input:subjects) {
        expected.push_back(ref::language(ast,input).nodes[ast.root].at(0,input.size()));
        ++oracle_queries;
    }
    std::vector<std::exception_ptr> errors(8);
    std::vector<std::thread> workers;
    std::atomic_bool go{false};
    for (std::size_t worker=0;worker<errors.size();++worker) workers.emplace_back([&,worker] {
        while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
        try {
            for (unsigned iteration=0;iteration<64;++iteration) {
                const auto index=(iteration+worker)%subjects.size();
                if (program.full_match(subjects[index],options())!=expected[index])
                    throw std::runtime_error("shared preparation corrupted a query's answer");
            }
        } catch(...) {errors[worker]=std::current_exception();}
    });
    go.store(true,std::memory_order_release);
    for (auto& worker:workers) worker.join();
    for (const auto& error:errors) {if (error) std::rethrow_exception(error);require(!error,"concurrent query failed");}
    production_queries+=64*errors.size();checks+=64*errors.size();
    std::cout<<"SHARED_PREPARATION threads="<<errors.size()<<" queries="<<64*errors.size()<<'\n';
}

void effects() {
    context="effect and observation eligibility";
    for (const auto pattern:{R"((?<S>a(?&S)b|)(x)\2)",R"((?<S>a(?&S)b|)(?<X>x)\k<X>)"})
        require(!detail::AstMachineProgram::eligible(parse(pattern)),"backreference admitted pure regional execution");
    auto ast=parse(R"((?<S>a(?&S)b|))");
    ast.has_counts=ast.has_references=ast.has_calls=false;
    ast.nodes[ast.root].bind_count=0;
    require(!detail::AstMachineProgram::eligible(ast),"stale public flags hid an observable count effect");
}
} // namespace

int main() {
    try {
        const auto all=fixtures();
        for (const auto& f:all) {
            auto subjects=ref::words(f.alphabet,f.maximum);
            subjects.insert(subjects.end(),f.extra.begin(),f.extra.end());
            compare(parse(f.pattern),subjects,f.name);
        }
        ref::Random random{UINT64_C(0xe3ac90f1ccaa7019)};
        for (unsigned n=0;n<36;++n) {
            auto f=ref::random_fixture(random,n,4);
            ++generated_grammars;
            compare(f.ast,f.inputs,f.name);
        }
        require(attempts>0,"regional execution never activated during oracle coverage");
        require(deferred>0 && published>0,"future regional publication bridge was never exercised");
        publication_protocol();live_constraint_chain();budgets();cancellation();shared_preparation();effects();
        std::cout<<"{\"status\":\"passed\",\"fixtures\":"<<all.size()
            <<",\"generated_grammars\":"<<generated_grammars<<",\"inputs\":"<<inputs
            <<",\"checks\":"<<checks<<",\"finite_relation_queries\":"<<oracle_queries
            <<",\"production_queries\":"<<production_queries<<",\"observation_checks\":"<<observation_checks
            <<",\"regional_attempts\":"<<attempts<<",\"regional_successes\":"<<successes
            <<",\"deferred_completions\":"<<deferred<<",\"published_completions\":"<<published
            <<",\"quota_queries\":"<<quota_queries<<",\"quota_completed\":"<<quota_completed
            <<",\"quota_limits\":"<<quota_limits<<",\"regional_retries\":"<<regional_retries<<"}\n";
    } catch(const std::exception& error) {
        std::cerr<<"REGIONAL FAILURE: "<<error.what()<<'\n';return 1;
    }
}
