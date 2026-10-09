#include "../bidirectional_comparison/cases.hpp"
#include "../marpa_comparison/marpa_adapter.hpp"
#include "occurrence_analysis.hpp"
#include "assisted_matcher.hpp"
#include "bidirectional_matcher.hpp"
#include "input_constraints.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <sys/resource.h>
#include <time.h>

namespace {
using Clock = std::chrono::steady_clock;
double elapsed_us(Clock::time_point start) {
    return std::chrono::duration<double, std::micro>(Clock::now() - start).count();
}
double process_cpu_us() {
    timespec value{};
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &value) != 0)
        throw std::runtime_error("process CPU clock unavailable");
    return double(value.tv_sec) * 1000000.0 + double(value.tv_nsec) / 1000.0;
}
std::string quote(const std::string& value) {
    std::ostringstream out;
    out << '"';
    for (unsigned char byte : value) {
        if (byte == '"' || byte == '\\') out << '\\' << byte;
        else if (byte < 32 || byte >= 127)
            out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                << unsigned(byte) << std::dec;
        else out << byte;
    }
    out << '"';
    return out.str();
}
class Json {
public:
    Json() { out_ << std::setprecision(12) << '{'; }
    void add(const char* key, const std::string& value) { prefix(key); out_ << quote(value); }
    void add(const char* key, bool value) { prefix(key); out_ << (value ? "true" : "false"); }
    template<class Number> void add(const char* key, Number value) { prefix(key); out_ << value; }
    void optional(const char* key, std::optional<double> value) {
        prefix(key); if (value) out_ << *value; else out_ << "null";
    }
    void raw(const char* key, const std::string& value) { prefix(key); out_ << value; }
    template<class Number> void array(const char* key, const std::vector<Number>& values) {
        prefix(key); out_ << '[';
        for (std::size_t i = 0; i < values.size(); ++i) { if (i) out_ << ','; out_ << values[i]; }
        out_ << ']';
    }
    std::string finish() { out_ << '}'; return out_.str(); }
private:
    void prefix(const char* key) { if (!first_) out_ << ','; first_ = false; out_ << quote(key) << ':'; }
    std::ostringstream out_;
    bool first_ = true;
};
std::string matching_json(const crank::OccurrenceStatistics& stats) {
    Json out;
#define FIELD(name) out.add(#name, stats.name)
    FIELD(states); FIELD(work_steps); FIELD(down_steps); FIELD(up_steps);
    FIELD(pruned); FIELD(arithmetic_pruned); FIELD(path_checks); FIELD(forward_obligations);
    FIELD(max_call_depth); FIELD(occurrences); FIELD(memo_entries); FIELD(memo_hits);
    FIELD(return_links); FIELD(completions); FIELD(replay_steps); FIELD(proof_nodes);
    FIELD(max_pending); FIELD(compression_lookups); FIELD(compression_hops);
    FIELD(compressed_completions); FIELD(compression_cycles); FIELD(control_redirects);
    FIELD(ordered_positions); FIELD(duplicate_completions); FIELD(limit_reached);
#undef FIELD
    return out.finish();
}
std::string indexed_json(const crank::BidirectionalStatistics& stats) {
    Json out;
    out.add("demanded_goals",stats.demanded_goals);
    out.add("facts",stats.facts);
    out.add("joins_attempted",stats.joins_attempted);
    out.add("joins_published",stats.joins_published);
    out.add("culls",stats.culls);
    out.add("work_steps",stats.work_steps);
    out.add("states",stats.states);
    out.add("max_pending",stats.max_pending);
    out.add("workers_used",stats.workers_used);
    out.add("max_active_workers",stats.max_active_workers);
    out.add("demanded_steps",stats.demanded_steps);
    out.add("speculative_steps",stats.speculative_steps);
    out.add("mandatory_continuations",stats.mandatory_continuations);
    out.add("deferred_speculation",stats.deferred_speculation);
    out.add("index_nodes_culled",stats.index_nodes_culled);
    out.add("index_leaves",stats.index_leaves);
    out.add("index_occurrences",stats.index_occurrences);
    out.add("index_bytes_scanned",stats.index_bytes_scanned);
    out.add("index_steps",stats.index_steps);
    out.add("index_declined",stats.index_declined);
    out.add("limit_reached",stats.limit_reached);
    return out.finish();
}
double median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    return values.at(values.size() / 2);
}
struct Samples { std::size_t batch = 0; std::vector<double> us; };
template<class Action> Samples setup_samples(Action action) {
    const auto start = Clock::now();
    action();
    Samples result;
    result.batch = std::clamp<std::size_t>(std::size_t(250.0 / std::max(0.05, elapsed_us(start))), 1, 8);
    for (std::size_t sample = 0; sample < 3; ++sample) {
        const auto begin = Clock::now();
        for (std::size_t i = 0; i < result.batch; ++i) action();
        result.us.push_back(elapsed_us(begin) / double(result.batch));
    }
    return result;
}
struct Answer {
    std::string status;
    bool matched = false;
    std::string detail;
};
} // namespace

namespace {
std::string case_json(const revisit::Case& item) {
    Json out;
    out.add("id",item.id);out.add("family",item.family);out.add("domain",item.domain);
    out.add("variant",item.variant);out.add("pattern",item.pattern);out.add("subject",item.subject);
    out.add("expected",item.expected);out.add("scale",item.scale);
    if(item.mutation_offset==crank::no_id) out.raw("mutation_offset","null");
    else out.add("mutation_offset",item.mutation_offset);
    out.add("practical",item.practical);out.add("oracle",item.oracle);out.add("scope",item.scope);
    out.add("allow_limit",item.allow_limit);out.add("prune",item.prune);
    out.add("max_states",item.max_states);out.add("max_steps",item.max_steps);
    return out.finish();
}
// Sound necessary conditions only. This optional hybrid is separate from
// unfiltered Marpa, and its analysis construction is charged to preparation.
const char* filter_reason(const crank::OccurrenceAnalysis& analysis, crank::NodeId root,
                          std::string_view input) {
    if(!analysis.length(root).accepts(input.size())) return "length";
    const auto& facts=analysis.byte_conditions(root);
    if(input.empty()) return facts.boundary.nullable ? "none" : "nullable";
    const auto byte=[&](std::size_t i){return static_cast<unsigned char>(input[i]);};
    if(!facts.boundary.first[byte(0)]) return "first";
    if(!facts.boundary.last[byte(input.size()-1)]) return "last";
    if(input.size()>1) {
        const auto bit=[&](std::size_t i){return std::uint64_t{1} << ((17U*byte(i)+byte(i+1))&63U);};
        if(!(facts.pairs.first&bit(0))) return "first_pair";
        if(!(facts.pairs.last&bit(input.size()-2))) return "last_pair";
    }
    return "none";
}
}


namespace {
std::string decision_name(crank::AssistanceDecision value) {
    using D=crank::AssistanceDecision;
    switch(value) {
    case D::Disabled:return "disabled";case D::SmallInput:return "small_input";
    case D::LargeInput:return "large_input";case D::NoBudget:return "no_budget";
    case D::Unavailable:return "unavailable";case D::RootRejected:return "root_rejected";
    case D::Busy:return "busy";case D::AllocationDeclined:return "allocation_declined";
    case D::Admitted:return "admitted";
    }
    return "unknown";
}
std::string assistance_json(const crank::AssistedStatistics& s) {
    Json out;out.add("decision",decision_name(s.decision));
    out.add("helpers_admitted",s.helpers_admitted);
    out.add("helpers_finished_at_return",s.helpers_finished_at_return);
    out.add("copied_input_bytes",s.copied_input_bytes);
    out.add("root_rejected_at_return",s.root_rejected_at_return);
    out.add("native_limit_rescued",s.native_limit_rescued);
    Json c;c.add("checks",s.consumption.checks);c.add("culled_requests",s.consumption.culled_requests);
    c.add("culled_tasks",s.consumption.culled_tasks);c.add("root_rejections",s.consumption.root_rejections);
    out.raw("consumption",c.finish());return out.finish();
}
std::string pool_json(const crank::AssistancePoolStatistics& s) {
    Json out;
#define F(name) out.add(#name,s.name)
    F(queries_admitted);F(queries_busy);F(tasks_submitted);F(tasks_finished);
    F(tasks_cancelled);F(tasks_declined);F(helper_steps);F(helper_bytes);
    F(nodes_excluded);F(root_rejections);F(active_helpers);
    F(max_stop_observation_ns);F(max_cleanup_ns);
#undef F
    return out.finish();
}
}

int main(int argc,char** argv) {
    try {
        const auto cases=bidi_bench::all_cases();
        if(argc==2 && std::string_view(argv[1])=="--list") {
            std::cout<<'[';
            for(std::size_t i=0;i<cases.size();++i){if(i)std::cout<<',';std::cout<<case_json(cases[i]);}
            std::cout<<"]\n";return 0;
        }
        if(argc!=3 && !(argc==4 && std::string_view(argv[3])=="--check")) {
            std::cerr<<"bench_assistance CASE_ID MODE [--check]\n";return 2;
        }
        const bool check_only=argc==4;
        const std::string id=argv[1],mode=argv[2];
        const auto found=std::find_if(cases.begin(),cases.end(),[&](const auto& c){return c.id==id;});
        if(found==cases.end())throw std::invalid_argument("unknown case id");
        const auto& c=*found;
        const bool is_native=mode=="native",is_indexed=mode=="native_indexed",is_marpa=mode=="marpa";
        const bool is_owned=mode=="assist2_owned";
        const bool is_assisted=mode=="assist1"||mode=="assist2"||mode=="assist4"||mode=="assist8"||is_owned;
        if(!is_native&&!is_indexed&&!is_marpa&&!is_assisted)throw std::invalid_argument("unknown execution mode");
        crank::OccurrenceMatchOptions options;
        options.prune=c.prune;options.max_states=c.max_states;options.max_steps=c.max_steps;
        crank::AssistanceOptions helpers;
        helpers.workers=mode=="assist8"?8:mode=="assist4"?4:mode=="assist2"||is_owned?2:1;
        helpers.min_input_bytes=1024;helpers.max_helper_steps=1000000;
        std::optional<crank::Ast> ast;
        std::unique_ptr<crank::OccurrenceProgram> native;
        std::unique_ptr<crank::AssistedProgram> assisted;
        std::unique_ptr<comparison::MarpaProgram> marpa;
        std::unique_ptr<crank::OccurrenceAnalysis> prefilter;
        std::unique_ptr<crank::detail::InputConstraints> indexed;
        std::shared_ptr<const std::string> owned;
        crank::OccurrenceStatistics statistics;
        crank::BidirectionalStatistics index_statistics;
        crank::AssistedStatistics assistance;
        comparison::Result marpa_statistics;
        crank::AssistancePoolStatistics first_return,first_idle,warm_before,warm_return,warm_idle,diagnostic_before,diagnostic_return,diagnostic_idle;
        Answer answer;Samples parses,preparations;
        std::vector<double> samples,cpu_samples;
        std::vector<std::string> query_statuses;
        std::size_t batch=0,timed_queries=0;
        double first_parse=0,first_build=0,owned_prepare=0,first_query=0,first_total=0;
        double first_cpu=0,first_tail=0,first_tail_cpu=0,warm_tail=0,warm_tail_cpu=0;
        double warm_total=0,warm_total_cpu=0,diagnostic_query=0,diagnostic_tail=0,diagnostic_tail_cpu=0;
        bool queried=false;
        const auto pool=[&]{return assisted?assisted->pool_statistics():crank::AssistancePoolStatistics{};};
        const auto drain=[&](double& wall,double& cpu){
            const auto cpu_start=process_cpu_us();const auto start=Clock::now();
            if(assisted)assisted->wait_idle();
            wall=elapsed_us(start);cpu=process_cpu_us()-cpu_start;
        };
        try {
            const auto total_start=Clock::now();auto start=Clock::now();
            ast.emplace(crank::parse(c.pattern));first_parse=elapsed_us(start);start=Clock::now();
            if(is_assisted)assisted=std::make_unique<crank::AssistedProgram>(*ast,helpers);
            else if(is_native||is_indexed){
                native=std::make_unique<crank::OccurrenceProgram>(*ast);
                if(is_indexed){prefilter=std::make_unique<crank::OccurrenceAnalysis>(*ast);indexed=std::make_unique<crank::detail::InputConstraints>(*ast,*prefilter);}
            }else marpa=std::make_unique<comparison::MarpaProgram>(*ast);
            first_build=elapsed_us(start);
            if(is_owned){start=Clock::now();owned=std::make_shared<const std::string>(c.subject);owned_prepare=elapsed_us(start);}
            auto execute=[&](bool diagnostics){
                Answer current;
                try {
                    if(is_assisted){
                        if(is_owned)current.matched=assisted->full_match(owned,options,diagnostics?&assistance:nullptr);
                        else current.matched=assisted->full_match(c.subject,options,diagnostics?&assistance:nullptr);
                        if(diagnostics)statistics=assistance.matching;
                    }else if(is_indexed){
                        crank::BidirectionalStatistics index_stats;
                        if(options.prune&&std::string_view(filter_reason(*prefilter,ast->root,c.subject))!="none"){
                            current.matched=false;++index_stats.culls;
                        }else{
                            crank::detail::InputConstraints::Limits limits;
                            limits.max_steps=options.max_steps/2;limits.max_occurrences=options.max_states/2;
                            auto input=indexed->build(c.subject,limits);const auto& spent=input.stats();
                            index_stats.index_nodes_culled=spent.inactive_nodes;index_stats.index_leaves=spent.literal_fragments;
                            index_stats.index_occurrences=spent.literal_hits;index_stats.index_bytes_scanned=spent.bytes_scanned;
                            index_stats.index_steps=index_stats.work_steps=spent.steps;index_stats.index_declined=spent.declined;
                            index_stats.states=spent.literal_hits;
                            if(!input.root_possible()){current.matched=false;++index_stats.culls;}
                            else{
                                auto remaining=options;remaining.max_steps-=std::min(remaining.max_steps,spent.steps);
                                remaining.max_states-=std::min(remaining.max_states,spent.literal_hits);
                                index_stats.workers_used=index_stats.max_active_workers=1;
                                if(diagnostics)index_statistics=index_stats;
                                current.matched=native->full_match(c.subject,remaining,diagnostics?&statistics:nullptr);
                                if(diagnostics){index_stats.states+=statistics.states;index_stats.work_steps+=statistics.work_steps;}
                            }
                        }
                        if(diagnostics)index_statistics=index_stats;
                    }else if(is_native)current.matched=native->full_match(c.subject,options,diagnostics?&statistics:nullptr);
                    else{
                        auto result=marpa->run(c.subject,false,false,true,diagnostics,false);
                        current.matched=result.matched;if(diagnostics)marpa_statistics=std::move(result);
                    }
                    current.status=current.matched?"match":"no_match";
                }catch(const crank::ResourceLimit& e){current.status="limit";current.detail=e.what();}
                catch(const std::bad_alloc&){current.status="memory";current.detail="allocation failed";}
                if(is_assisted&&diagnostics)statistics=assistance.matching;
                if(is_indexed&&diagnostics&&(current.status=="limit"||current.status=="memory")){
                    index_statistics.states+=statistics.states;index_statistics.work_steps+=statistics.work_steps;
                    index_statistics.limit_reached=current.status=="limit";
                }
                query_statuses.push_back(current.status);return current;
            };
            const auto verify=[&](const Answer& value){
                if((value.status=="match"||value.status=="no_match")&&value.matched!=c.expected)
                    throw std::runtime_error("independent fixture verdict mismatch");
            };
            const auto first_cpu_start=process_cpu_us();start=Clock::now();
            answer=execute(false);first_query=elapsed_us(start);first_cpu=process_cpu_us()-first_cpu_start;
            first_total=elapsed_us(total_start);queried=true;verify(answer);first_return=pool();
            // Only phase boundaries drain; measured matching never joins helpers.
            drain(first_tail,first_tail_cpu);first_idle=pool();
            const auto first_status=answer.status;
            batch=std::clamp<std::size_t>(std::size_t(500.0/std::max(0.05,first_query)),1,64);
            warm_before=pool();const auto warm_start=Clock::now();const auto warm_cpu_start=process_cpu_us();
            for(std::size_t sample=0;!check_only&&(first_status=="match"||first_status=="no_match")&&sample<5;++sample){
                const auto cpu_start=process_cpu_us();start=Clock::now();
                for(std::size_t i=0;i<batch;++i){answer=execute(false);verify(answer);}
                samples.push_back(elapsed_us(start)/double(batch));cpu_samples.push_back((process_cpu_us()-cpu_start)/double(batch));
                timed_queries+=batch;
            }
            warm_return=pool();drain(warm_tail,warm_tail_cpu);warm_idle=pool();
            warm_total=elapsed_us(warm_start);warm_total_cpu=process_cpu_us()-warm_cpu_start;
            diagnostic_before=pool();start=Clock::now();answer=execute(true);diagnostic_query=elapsed_us(start);verify(answer);
            diagnostic_return=pool();drain(diagnostic_tail,diagnostic_tail_cpu);diagnostic_idle=pool();
            if(std::any_of(query_statuses.begin(),query_statuses.end(),[&](const auto& status){return status!=first_status;})){
                answer.status=std::find(query_statuses.begin(),query_statuses.end(),"memory")!=query_statuses.end()?"memory":"limit";
                answer.detail="Resource outcome varied across repeated queries; completed verdicts all matched the oracle";
            }
            if(!check_only)parses=setup_samples([&]{const auto fresh=crank::parse(c.pattern);if(fresh.nodes.size()!=ast->nodes.size())throw std::runtime_error("parse changed");});
            if(!check_only)preparations=setup_samples([&]{
                if(is_assisted){crank::AssistedProgram fresh(*ast,helpers);}
                else if(is_native||is_indexed){crank::OccurrenceProgram fresh(*ast);if(is_indexed){crank::OccurrenceAnalysis facts(*ast);crank::detail::InputConstraints input(*ast,facts);}}
                else{comparison::MarpaProgram fresh(*ast);}
            });
        }catch(const comparison::CompileRejected& e){answer.status="compile_rejected";answer.detail=e.what();}
        catch(const std::bad_alloc&){answer.status="memory";answer.detail="allocation failed";}
        catch(const std::exception& e){answer.status="error";answer.detail=e.what();}
        std::string reason="unavailable";
        if(ast){try{crank::OccurrenceAnalysis analysis(*ast);reason=filter_reason(analysis,ast->root,c.subject);}catch(const std::exception&){}}
        rusage usage{};getrusage(RUSAGE_SELF,&usage);Json out;
        out.add("id",c.id);out.add("mode",mode);out.add("check_only",check_only);out.add("family",c.family);out.add("domain",c.domain);
        out.add("variant",c.variant);out.add("scale",c.scale);out.add("bytes",c.subject.size());out.add("ast_nodes",ast?ast->nodes.size():0);
        out.add("expected",c.expected);out.add("allow_limit",c.allow_limit);out.add("prune",options.prune);out.add("workers",helpers.workers);
        out.add("max_states",options.max_states);out.add("max_steps",options.max_steps);out.add("max_helper_steps",helpers.max_helper_steps);
        out.add("min_input_bytes",helpers.min_input_bytes);out.add("owned_input",is_owned);out.add("owned_input_prepare_us",owned_prepare);
        out.add("status",answer.status);out.add("detail",answer.detail);
        std::string observed="[";for(std::size_t i=0;i<query_statuses.size();++i){if(i)observed+=',';observed+=quote(query_statuses[i]);}
        out.raw("query_statuses",observed+"]");out.raw("matched",answer.status=="match"?"true":answer.status=="no_match"?"false":"null");
        out.add("first_parse_us",first_parse);out.add("first_build_us",first_build);
        out.optional("first_query_us",queried?std::optional<double>(first_query):std::nullopt);
        out.optional("first_total_us",queried?std::optional<double>(first_total):std::nullopt);
        out.optional("median_us",samples.empty()?std::nullopt:std::optional<double>(median(samples)));
        out.optional("cpu_median_us",cpu_samples.empty()?std::nullopt:std::optional<double>(median(cpu_samples)));
        out.array("sample_us",samples);out.array("cpu_sample_us",cpu_samples);out.add("batch",batch);out.add("timed_queries",timed_queries);
        out.add("first_query_cpu_us",first_cpu);out.add("first_idle_tail_us",first_tail);out.add("first_idle_tail_cpu_us",first_tail_cpu);
        out.add("warm_idle_tail_us",warm_tail);out.add("warm_idle_tail_cpu_us",warm_tail_cpu);
        out.add("warm_total_through_idle_us",warm_total);out.add("warm_total_cpu_through_idle_us",warm_total_cpu);
        out.add("warm_cpu_through_idle_per_query_us",timed_queries?warm_total_cpu/double(timed_queries):0.0);
        out.add("warm_wall_through_idle_per_query_us",timed_queries?warm_total/double(timed_queries):0.0);
        out.add("diagnostic_query_us",diagnostic_query);out.add("diagnostic_idle_tail_us",diagnostic_tail);
        out.add("diagnostic_idle_tail_cpu_us",diagnostic_tail_cpu);
        out.optional("parse_median_us",parses.us.empty()?std::nullopt:std::optional<double>(median(parses.us)));
        out.array("parse_sample_us",parses.us);out.add("parse_batch",parses.batch);
        out.optional("preparation_median_us",preparations.us.empty()?std::nullopt:std::optional<double>(median(preparations.us)));
        out.array("preparation_sample_us",preparations.us);out.add("preparation_batch",preparations.batch);
        out.raw("statistics",matching_json(statistics));out.raw("assistance",assistance_json(assistance));
        out.raw("indexed",indexed_json(index_statistics));out.add("input_index",is_indexed);
        Json pools;pools.raw("first_return",pool_json(first_return));pools.raw("first_idle",pool_json(first_idle));
        pools.raw("warm_before",pool_json(warm_before));pools.raw("warm_return",pool_json(warm_return));pools.raw("warm_idle",pool_json(warm_idle));
        pools.raw("diagnostic_before",pool_json(diagnostic_before));pools.raw("diagnostic_return",pool_json(diagnostic_return));pools.raw("diagnostic_idle",pool_json(diagnostic_idle));
        out.raw("pools",pools.finish());
        Json m;m.add("earley_items",marpa_statistics.earley_items);m.add("earley_sets",marpa_statistics.earley_sets);
        m.add("completed_positions",marpa_statistics.earley_sets?marpa_statistics.earley_sets-1:0);
        m.add("leo_enabled",marpa_statistics.leo_enabled);m.add("version",marpa?marpa->version():std::string{});
        out.raw("marpa",m.finish());out.add("root_filter_reason",reason);out.add("peak_rss_kib",usage.ru_maxrss);
        std::cout<<out.finish()<<'\n';return answer.status=="error"?1:0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}
}
