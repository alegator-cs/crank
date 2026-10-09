#include "ast_machine.hpp"
#include "occurrence_analysis.hpp"
#ifndef CRANK_BASELINE
#include "assisted_matcher.hpp"
#include "input_constraints.hpp"
#include "private_culler.hpp"
#include "crank.hpp"
#include "../marpa_comparison/marpa_adapter.hpp"
#endif
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <sys/resource.h>
#include <time.h>

namespace {
using Clock=std::chrono::steady_clock;
double us(Clock::time_point t){return std::chrono::duration<double,std::micro>(Clock::now()-t).count();}
double cpu(){timespec t{};if(clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&t))throw std::runtime_error("CPU clock failed");return t.tv_sec*1e6+t.tv_nsec/1e3;}
std::string read(const char* p){std::ifstream f(p,std::ios::binary);if(!f)throw std::runtime_error("input file missing");return {std::istreambuf_iterator<char>(f),{}};}
std::string quote(std::string_view s){std::ostringstream o;o<<'"';for(unsigned char c:s){if(c=='"'||c=='\\')o<<'\\'<<c;else if(c<32||c>126)o<<"\\u"<<std::hex<<std::setw(4)<<std::setfill('0')<<unsigned(c)<<std::dec;else o<<c;}o<<'"';return o.str();}
class Json{std::ostringstream o;bool first=true;void key(const char* k){if(!first)o<<',';first=false;o<<quote(k)<<':';}public:Json(){o<<std::setprecision(12)<<'{';}template<class T>void add(const char*k,T x){key(k);o<<x;}void add(const char*k,const std::string&x){key(k);o<<quote(x);}void add(const char*k,bool x){key(k);o<<(x?"true":"false");}void raw(const char*k,const std::string&x){key(k);o<<x;}void array(const char*k,const std::vector<double>&v){key(k);o<<'[';for(size_t i=0;i<v.size();++i){if(i)o<<',';o<<v[i];}o<<']';}std::string str(){o<<'}';return o.str();}};
double median(std::vector<double> v){std::sort(v.begin(),v.end());return v.at(v.size()/2);}
std::string stats_json(const crank::OccurrenceStatistics&s){Json j;
#define F(n) j.add(#n,s.n)
F(states);F(work_steps);F(down_steps);F(up_steps);F(pruned);F(arithmetic_pruned);F(memo_entries);F(memo_hits);F(return_links);F(completions);F(replay_steps);F(max_pending);F(compression_hops);F(compressed_completions);F(duplicate_completions);F(limit_reached);
#ifndef CRANK_BASELINE
#ifdef CRANK_COALESCE
F(coalesced_returns);F(ordered_endpoint_checks);F(fanout_word_checks);F(fanout_links_skipped);
#endif
#endif
#undef F
return j.str();}
#ifndef CRANK_BASELINE
std::string decision(crank::AssistanceDecision d){using D=crank::AssistanceDecision;switch(d){case D::Disabled:return"disabled";case D::SmallInput:return"small_input";case D::LargeInput:return"large_input";case D::NoBudget:return"no_budget";case D::Unavailable:return"unavailable";case D::RootRejected:return"root_rejected";case D::Busy:return"busy";case D::AllocationDeclined:return"allocation_declined";case D::Admitted:return"admitted";case D::NoUsefulProof:return"no_useful_proof";case D::PolicyBackoff:return"policy_backoff";case D::BelowWorkThreshold:return"below_work_threshold";}return"unknown";}
std::string assist_json(const crank::AssistedStatistics&s){Json j;j.add("decision",decision(s.decision));j.add("helpers_admitted",s.helpers_admitted);j.add("copied_input_bytes",s.copied_input_bytes);j.add("native_limit_rescued",s.native_limit_rescued);j.add("native_steps_before_admission",s.native_steps_before_admission);j.add("checks",s.consumption.checks);j.add("culled_requests",s.consumption.culled_requests);j.add("culled_tasks",s.consumption.culled_tasks);j.add("root_rejections",s.consumption.root_rejections);return j.str();}
std::string pool_json(const crank::AssistancePoolStatistics&s){Json j;
#define F(n) j.add(#n,s.n)
F(queries_admitted);F(queries_busy);F(tasks_submitted);F(tasks_finished);F(tasks_cancelled);F(tasks_declined);F(helper_steps);F(helper_bytes);F(nodes_excluded);F(root_rejections);F(active_helpers);F(max_stop_observation_ns);F(max_cleanup_ns);F(queries_policy_skipped);F(queries_no_useful_proof);
#undef F
return j.str();}
#endif
}

int main(int argc,char**argv){
    if(argc<9){std::cerr<<"bench ID MODE PATTERN_FILE SUBJECT_FILE EXPECTED MAX_STATES MAX_STEPS PRUNE [--check|--sequence OTHER_SUBJECT ORDER]\n";return 2;}
    const std::string id=argv[1],mode=argv[2],pattern=read(argv[3]),subject=read(argv[4]);
    const bool expected=std::string_view(argv[5])=="1",check=argc==10&&std::string_view(argv[9])=="--check";
    const bool sequence=argc==12&&std::string_view(argv[9])=="--sequence";
    const std::string other=sequence?read(argv[10]):std::string{},order=sequence?argv[11]:"";
    crank::OccurrenceMatchOptions options;options.max_states=std::stoull(argv[6]);options.max_steps=std::stoull(argv[7]);options.prune=std::string_view(argv[8])=="1";
    std::string status="error",detail,backend=mode;bool matched=false;
    double parse_us=0,prepare_us=0,first_us=0,first_cpu=0,first_tail=0,first_tail_cpu=0,warm_wall=0,warm_cpu=0,warm_tail=0,warm_tail_cpu=0,diagnostic_us=0,diagnostic_tail=0;
    std::vector<double> samples,cpus;std::size_t batch=0,queries=0,ast_nodes=0;
    crank::OccurrenceStatistics stats;
    std::string assist="{}",pools="{}",index="{}",marpa_stats="{}",presence_stats="{}",sequence_rows="[]";
    try{
        auto t=Clock::now();const auto ast=crank::parse(pattern);parse_us=us(t);ast_nodes=ast.nodes.size();t=Clock::now();
        std::unique_ptr<crank::detail::AstMachineProgram> native;
#ifndef CRANK_BASELINE
        std::unique_ptr<crank::Program> automatic;
        std::unique_ptr<crank::AssistedProgram> assisted;
        std::unique_ptr<crank::detail::InputConstraints> indexed;
        std::unique_ptr<crank::detail::PrivateCuller> presence;
        std::shared_ptr<const crank::OccurrenceAnalysis> analysis;
        std::unique_ptr<comparison::MarpaProgram> marpa;
        crank::AssistedStatistics a;crank::detail::InputConstraints::Stats i;comparison::Result m;crank::RunStatistics d;
        const bool is_assisted=mode.starts_with("selective")||mode.starts_with("always")||mode.starts_with("progress");
        if(is_assisted){crank::AssistanceOptions h;h.workers=mode.ends_with('4')?4:2;h.policy=mode.starts_with("always")?crank::AssistancePolicy::Always:mode.starts_with("progress")?crank::AssistancePolicy::Progress:crank::AssistancePolicy::Selective;h.max_helper_steps=4'000'000;assisted=std::make_unique<crank::AssistedProgram>(ast,h);}
        else if(mode=="auto")automatic=std::make_unique<crank::Program>(pattern);
        else if(mode=="marpa")marpa=std::make_unique<comparison::MarpaProgram>(ast);
        else {native=std::make_unique<crank::detail::AstMachineProgram>(ast);if(mode=="indexed"){analysis=native->shared_analysis();indexed=std::make_unique<crank::detail::InputConstraints>(ast,*analysis);}else if(mode=="sync_presence"){analysis=native->shared_analysis();presence=std::make_unique<crank::detail::PrivateCuller>(ast,*analysis);}else if(mode!="native"&&mode!="no_coalesce")throw std::invalid_argument("unknown mode");}
#ifdef CRANK_COALESCE
        if(mode=="no_coalesce")options.coalesce_returns=false;
#endif
#else
        if(mode!="baseline")throw std::invalid_argument("baseline executable only supports baseline");
        native=std::make_unique<crank::detail::AstMachineProgram>(ast);
#endif
        prepare_us=us(t);
        const auto drain=[&](double&wall,double&cost){const double c=cpu();auto t=Clock::now();
#ifndef CRANK_BASELINE
            if(assisted)assisted->wait_idle();
#endif
            wall=us(t);cost=cpu()-c;};
        const auto pool=[&](){
#ifndef CRANK_BASELINE
            return pool_json(assisted?assisted->pool_statistics():crank::AssistancePoolStatistics{});
#else
            return std::string("{}");
#endif
        };
        const auto execute=[&](std::string_view input,bool want,bool diagnostics){
            try{
#ifndef CRANK_BASELINE
                if(assisted){matched=assisted->full_match(input,options,diagnostics?&a:nullptr);if(diagnostics){stats=a.matching;assist=assist_json(a);}}
                else if(automatic){crank::RunOptions o;o.occurrence=options;o.match.max_states=options.max_states;matched=automatic->full_match(input,o,diagnostics?&d:nullptr);if(diagnostics){stats=d.occurrence;backend=d.backend;}}
                else if(marpa){auto r=marpa->run(input,false,false,true,diagnostics,false);matched=r.matched;if(diagnostics)m=std::move(r);}
                else if(presence){
                    crank::detail::AtomicExclusions bits(ast.nodes.size());std::atomic_bool stop{false};
                    crank::detail::PrivateCuller::Stats ps;crank::detail::NativeAssistanceStatistics ns;
                    presence->scan_presence(input,stop,bits,{4'000'000,64*1024*1024},ps);
                    matched=!bits.root_rejected()&&native->full_match_assisted(input,options,bits,diagnostics?&stats:nullptr,diagnostics?&ns:nullptr);
                    if(diagnostics){Json p;p.add("bytes_scanned",ps.bytes_scanned);p.add("steps",ps.steps);p.add("inactive_nodes",ps.inactive_nodes);p.add("declined",ps.declined);p.add("root_rejected",bits.root_rejected());p.add("culled_requests",ns.culled_requests);p.add("culled_tasks",ns.culled_tasks);presence_stats=p.str();}
                }
                else if(indexed){auto built=indexed->build(input,{4'000'000,1'000'000,64*1024*1024});if(diagnostics)i=built.stats();matched=built.root_possible()&&native->full_match(input,options,diagnostics?&stats:nullptr);}
                else
#endif
                    matched=native->full_match(input,options,diagnostics?&stats:nullptr);
                if(matched!=want)throw std::runtime_error("independent oracle mismatch");
                status=matched?"match":"no_match";
            }catch(const crank::ResourceLimit&e){status="limit";detail=e.what();}
            catch(const std::bad_alloc&){status="memory";detail="allocation failed";}
        };
        const double fc=cpu();t=Clock::now();execute(subject,expected,false);first_us=us(t);first_cpu=cpu()-fc;
        Json pj;pj.raw("first_return",pool());drain(first_tail,first_tail_cpu);pj.raw("first_idle",pool());
        const auto first_status=status;
        if(sequence){
            sequence_rows="[";
            // Same compiled grammar and log2(length) bucket, with distributions
            // intentionally shifted. Diagnostics are equal across all modes.
            for(std::size_t k=0;k<48;++k){const bool use_bad=order=="valid_then_reject"?k>=32:order=="reject_then_valid"?k<16:k%2;
                const std::string_view input=use_bad?std::string_view(other):std::string_view(subject);
                const double c=cpu();auto begin=Clock::now();execute(input,!use_bad,true);Json row;
                row.add("query",k);row.add("expected",!use_bad);row.add("wall_us",us(begin));row.add("cpu_us",cpu()-c);row.add("status",status);row.raw("assistance",assist);row.raw("statistics",stats_json(stats));if(k)sequence_rows+=',';sequence_rows+=row.str();
            }
            sequence_rows+=']';double ignored=0;drain(warm_tail,ignored);
        }else if(!check&&(status=="match"||status=="no_match")){
            batch=std::clamp<std::size_t>(std::size_t(500.0/std::max(first_us,0.05)),1,64);
            auto start=Clock::now();const double cstart=cpu();pj.raw("warm_before",pool());
            bool varied=false;
            for(size_t sample=0;sample<5;++sample){auto start=Clock::now();const double cstart=cpu();
                for(size_t k=0;k<batch;++k){execute(subject,expected,false);varied|=status!=first_status;}
                samples.push_back(us(start)/batch);cpus.push_back((cpu()-cstart)/batch);queries+=batch;
            }
            pj.raw("warm_return",pool());drain(warm_tail,warm_tail_cpu);warm_wall=us(start);warm_cpu=cpu()-cstart;pj.raw("warm_idle",pool());
            t=Clock::now();execute(subject,expected,true);diagnostic_us=us(t);double ignored=0;pj.raw("diagnostic_return",pool());drain(diagnostic_tail,ignored);pj.raw("diagnostic_idle",pool());
            if(varied||status!=first_status){status="limit";detail="resource outcome varied across repeated queries";}
        }else{t=Clock::now();execute(subject,expected,true);diagnostic_us=us(t);double ignored=0;drain(diagnostic_tail,ignored);}
        pools=pj.str();
#ifndef CRANK_BASELINE
        Json ij;ij.add("steps",i.steps);ij.add("bytes_scanned",i.bytes_scanned);ij.add("literal_hits",i.literal_hits);ij.add("inactive_nodes",i.inactive_nodes);ij.add("declined",i.declined);ij.add("rejection_reason",i.rejection_reason);index=ij.str();
        Json mj;mj.add("items",m.earley_items);mj.add("sets",m.earley_sets);mj.add("leo_enabled",m.leo_enabled);mj.add("version",marpa?marpa->version():std::string{});marpa_stats=mj.str();
#endif
    }
#ifndef CRANK_BASELINE
    catch(const comparison::CompileRejected&e){status="compile_rejected";detail=e.what();}
#endif
    catch(const std::bad_alloc&){status="memory";detail="allocation failed";}
    catch(const std::exception&e){status="error";detail=e.what();}
    rusage r{};getrusage(RUSAGE_SELF,&r);Json out;out.add("id",id);out.add("mode",mode);out.add("backend",backend);out.add("bytes",subject.size());out.add("ast_nodes",ast_nodes);out.add("expected",expected);out.add("status",status);out.add("detail",detail);out.add("check_only",check);
    out.raw("matched",status=="match"?"true":status=="no_match"?"false":"null");out.add("parse_us",parse_us);out.add("prepare_us",prepare_us);out.add("first_query_us",first_us);out.add("first_query_cpu_us",first_cpu);out.add("first_tail_us",first_tail);out.add("first_tail_cpu_us",first_tail_cpu);
    out.array("sample_us",samples);out.array("cpu_sample_us",cpus);out.raw("median_us",samples.empty()?"null":std::to_string(median(samples)));out.raw("cpu_median_us",cpus.empty()?"null":std::to_string(median(cpus)));out.add("batch",batch);out.add("timed_queries",queries);out.add("warm_wall_through_idle_us",warm_wall);out.add("warm_cpu_through_idle_us",warm_cpu);out.add("warm_tail_us",warm_tail);out.add("warm_tail_cpu_us",warm_tail_cpu);out.add("diagnostic_us",diagnostic_us);out.add("diagnostic_tail_us",diagnostic_tail);out.add("peak_rss_kib",r.ru_maxrss);out.raw("statistics",stats_json(stats));out.raw("assistance",assist);out.raw("pools",pools);out.raw("indexed",index);out.raw("marpa",marpa_stats);out.raw("presence",presence_stats);out.raw("sequence",sequence_rows);std::cout<<out.str()<<'\n';return status=="error"?1:0;
}
