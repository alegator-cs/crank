#include "crank.hpp"
#include "assisted_matcher.hpp"
#include <time.h>
#include <sched.h>
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
using namespace std;
struct Stamp{double cpu,wall;};
Stamp stamp(){timespec c{},w{};clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&c);clock_gettime(CLOCK_MONOTONIC,&w);return{double(c.tv_sec)*1e9+c.tv_nsec,double(w.tv_sec)*1e9+w.tv_nsec};}
Stamp elapsed(Stamp a,Stamp b){return{b.cpu-a.cpu,b.wall-a.wall};}
string q(string_view s){ostringstream o;o<<'"';for(unsigned char c:s){if(c=='"'||c=='\\')o<<'\\'<<c;else if(c<32||c>126)o<<"\\u"<<hex<<setw(4)<<setfill('0')<<unsigned(c)<<dec;else o<<c;}return o.str()+'"';}
string file(const char*p){ifstream f(p,ios::binary);if(!f)throw runtime_error("cannot read file");return{istreambuf_iterator<char>(f),{}};}
uint32_t u32(string_view b,size_t&at){if(at+4>b.size())throw runtime_error("corpus truncated");uint32_t n=0;for(int k=0;k<4;++k)n|=uint32_t((unsigned char)b[at++])<<(8*k);return n;}
struct Input{shared_ptr<const string> s;bool expected;};
vector<Input> corpus(string_view b){if(b.size()>128*1024*1024||b.substr(0,4)!="CFI1")throw runtime_error("invalid corpus header or size");size_t at=4;auto n=u32(b,at);if(!n||n>1000000)throw runtime_error("invalid corpus count");vector<Input> v;v.reserve(n);for(uint32_t i=0;i<n;++i){auto len=u32(b,at);if(at>=b.size())throw runtime_error("corpus truncated verdict");unsigned char e=b[at++];if(e>1||len>16777216||at+len>b.size())throw runtime_error("invalid corpus row");string s(b.substr(at,len));at+=len;if(any_of(s.begin(),s.end(),[](unsigned char c){return c>127;}))throw runtime_error("unsupported non-ASCII input");v.push_back({make_shared<const string>(move(s)),bool(e)});}if(at!=b.size())throw runtime_error("corpus trailing bytes");return v;}
void phase(ostream&o,Stamp v){o<<"{\"cpu_ns\":"<<v.cpu<<",\"wall_ns\":"<<v.wall<<'}';}
void pool(ostream&o,const crank::AssistancePoolStatistics&p){o<<"{\"queries_admitted\":"<<p.queries_admitted<<",\"queries_busy\":"<<p.queries_busy<<",\"tasks_submitted\":"<<p.tasks_submitted<<",\"tasks_finished\":"<<p.tasks_finished<<",\"tasks_cancelled\":"<<p.tasks_cancelled<<",\"tasks_declined\":"<<p.tasks_declined<<",\"helper_steps\":"<<p.helper_steps<<",\"helper_bytes\":"<<p.helper_bytes<<",\"root_rejections\":"<<p.root_rejections<<",\"nodes_excluded\":"<<p.nodes_excluded<<",\"active_helpers\":"<<p.active_helpers<<'}';}
int main(int argc,char**argv){cout<<setprecision(17);string status="error",stage="arguments";try{
 if(argc!=8)throw runtime_error("ENGINE GRAMMAR CORPUS check|measure COUNT SAMPLES TARGET_NS");
 string mode=argv[1],op=argv[4];bool observe=mode.ends_with("_observe");if(observe)mode.resize(mode.size()-8);if(mode!="auto"&&mode!="progress4")throw runtime_error("unsupported engine");
 size_t count=stoull(argv[5]),samples=stoull(argv[6]);double target=stoull(argv[7]);if((op!="check"&&op!="measure")||count>32768||!samples||samples>31||target<1||target>6e10||(!count&&op=="measure"))throw runtime_error("invalid counts");
 stage="input";string grammar=file(argv[2]),blob=file(argv[3]);auto in=corpus(blob);blob.clear();blob.shrink_to_fit();if(op=="measure"&&in.size()<1+count*samples)throw runtime_error("insufficient fresh corpus");
 crank::RunOptions options;options.match.max_states=options.occurrence.max_states=2000000;options.match.max_steps=options.occurrence.max_steps=8000000;
 crank::RunStatistics rs;crank::AssistedStatistics as;size_t admitted=0,roots=0,culls=0,culled_tasks=0,steps=0;vector<int> verdict(in.size(),-1);vector<size_t> cpus;cpus.reserve(in.size()+1);
 unique_ptr<crank::Program> p;unique_ptr<crank::AssistedProgram>a;auto begin=stamp();stage="preparation";
 if(mode=="auto")p=make_unique<crank::Program>(grammar);else{crank::AssistanceOptions ao;ao.workers=4;ao.policy=crank::AssistancePolicy::Progress;ao.max_helper_steps=4000000;a=make_unique<crank::AssistedProgram>(crank::parse(grammar),ao);}
auto prep=elapsed(begin,stamp());
 auto query=[&](size_t i,bool diagnostic=false){bool matched;if(p)matched=p->full_match(*in[i].s,options,observe||diagnostic?&rs:nullptr);else matched=a->full_match(in[i].s,options.occurrence,observe||diagnostic?&as:nullptr);verdict[i]=matched;if(observe||diagnostic)cpus.push_back(sched_getcpu());if(observe&&!diagnostic){admitted+=as.helpers_admitted;roots+=as.consumption.root_rejections;culls+=as.consumption.culled_requests;culled_tasks+=as.consumption.culled_tasks;steps+=(a?as.matching:rs.occurrence).work_steps;}return matched;};
 stage="first";auto first_begin=stamp();bool first=query(0);auto foreground=elapsed(first_begin,stamp());if(a)a->wait_idle();auto first_complete=elapsed(first_begin,stamp());auto cold=elapsed(begin,stamp());if(first!=in[0].expected)throw runtime_error("incorrect first verdict");
 vector<Stamp>warm;vector<bool>quality;vector<crank::AssistancePoolStatistics>before,after;stage=op;
 if(op=="check"){for(size_t i=1;i<in.size();++i){auto matched=query(i,true);if(a)a->wait_idle();if(matched!=in[i].expected)throw runtime_error("incorrect check verdict");}}
 else{for(size_t sample=0;sample<samples;++sample){before.push_back(a?a->pool_statistics():crank::AssistancePoolStatistics{});auto b=stamp();for(size_t i=0;i<count;++i)query(1+sample*count+i);if(a)a->wait_idle();auto v=elapsed(b,stamp());cpus.push_back(sched_getcpu());warm.push_back(v);quality.push_back(v.cpu>=target&&v.wall>=target);after.push_back(a?a->pool_statistics():crank::AssistancePoolStatistics{});for(size_t i=0;i<count;++i){auto at=1+sample*count+i;if(verdict[at]!=in[at].expected)throw runtime_error("incorrect warm verdict");}}}
 // Separate detailed route probes; actual timed pool counters above remain distinct.
 stage="diagnostic";query(0,true);if(a)a->wait_idle();auto o=a?as.matching:rs.occurrence;
 timespec cr{},wr{};clock_getres(CLOCK_PROCESS_CPUTIME_ID,&cr);clock_getres(CLOCK_MONOTONIC,&wr);
 cout<<"{\"schema\":\"crank-marpa-independent-v1\",\"status\":\"ok\",\"engine\":"<<q(mode)<<",\"operation\":"<<q(op)<<",\"timed_statistics_enabled\":"<<(observe?"true":"false")<<",\"frontend\":\"Crank grammar-source parser\",\"semantics\":{\"status\":\"not_run\",\"capture_outputs\":0,\"forest_enumeration\":0},\"timers\":{\"cpu\":\"CLOCK_PROCESS_CPUTIME_ID\",\"wall\":\"CLOCK_MONOTONIC\",\"cpu_resolution_ns\":"<<(cr.tv_sec*1000000000.0+cr.tv_nsec)<<",\"wall_resolution_ns\":"<<(wr.tv_sec*1000000000.0+wr.tv_nsec)<<"},\"phases\":{\"preparation_including_module_load\":";phase(cout,prep);cout<<",\"first_foreground\":";phase(cout,foreground);cout<<",\"first\":";phase(cout,first_complete);cout<<",\"cold_prepare_and_first_complete\":";phase(cout,cold);cout<<"},\"warm_batches\":[";
 for(size_t i=0;i<warm.size();++i){if(i)cout<<',';cout<<"{\"count\":"<<count<<",\"cpu_ns\":"<<warm[i].cpu<<",\"wall_ns\":"<<warm[i].wall<<",\"target_met\":"<<(quality[i]?"true":"false")<<",\"pool_before\":";pool(cout,before[i]);cout<<",\"pool_after_drain\":";pool(cout,after[i]);cout<<'}';}
 cout<<"],\"verdicts\":[";for(size_t i=0;i<verdict.size();++i){if(i)cout<<',';cout<<verdict[i];}cout<<"],\"corpus_records\":"<<in.size()<<",\"queried_records\":"<<count_if(verdict.begin(),verdict.end(),[](int x){return x>=0;})<<",\"caller_cpu_samples\":[";for(size_t i=0;i<cpus.size();++i){if(i)cout<<',';cout<<cpus[i];}
 cout<<"],\"timed_consumption\":{\"available\":"<<(observe?"true":"false")<<",\"helpers_admitted\":"<<admitted<<",\"root_proofs_consumed\":"<<roots<<",\"culled_requests\":"<<culls<<",\"culled_tasks\":"<<culled_tasks<<",\"native_steps\":"<<steps<<"},\"diagnostic\":{\"helper_decision\":"<<int(as.decision)<<",\"helpers_admitted\":"<<as.helpers_admitted<<",\"root_proofs_consumed\":"<<as.consumption.root_rejections<<",\"backend\":"<<q(p?rs.backend:"assisted-native")<<",\"work_steps\":"<<o.work_steps<<",\"predictive_attempts\":"<<o.predictive_attempts<<",\"exact_program_attempts\":"<<o.exact_program_attempts<<",\"normalized_exact_attempts\":"<<o.normalized_exact_attempts<<"}}\n";
 }catch(const crank::ResourceLimit&e){cout<<"{\"status\":\"resource\",\"stage\":"<<q(stage)<<",\"reason\":"<<q(e.what())<<"}\n";return 5;}
 catch(const bad_alloc&e){cout<<"{\"status\":\"memory_limit\",\"stage\":"<<q(stage)<<",\"reason\":"<<q(e.what())<<"}\n";return 6;}
 catch(const exception&e){cout<<"{\"status\":\"error\",\"stage\":"<<q(stage)<<",\"reason\":"<<q(e.what())<<"}\n";return 4;}
}
