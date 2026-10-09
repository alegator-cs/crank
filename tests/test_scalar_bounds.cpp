#include "occurrence_matcher.hpp"
#include "regex_ast.hpp"
#include "helpers/test_progress.hpp"
#include <algorithm>
#include <functional>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace crank;
crank_test::Progress progress("test_scalar_bounds");
std::size_t checks = 0;
std::string default_budget_detail() {
    const OccurrenceMatchOptions defaults;
    return " max_states="+std::to_string(defaults.max_states)+
        " max_steps="+std::to_string(defaults.max_steps)+
        " max_occurrences="+std::to_string(defaults.max_occurrences)+
        " max_call_depth="+std::to_string(defaults.max_call_depth);
}
OccurrenceProgram prepare_program(const std::string& pattern, const std::string& detail,
                                  bool risky=false) {
    const auto ast=progress.call("parse",detail,[&]{return parse(pattern);},risky);
    return progress.call("program.setup",detail,[&]{return OccurrenceProgram(ast);},risky);
}
void require(bool condition, const std::string& context) {
    ++checks;
    if (!condition) throw std::runtime_error(context);
}
struct State {
    std::size_t p = 0;
    std::vector<std::optional<Span>> caps, pending;
    auto operator<=>(const State&) const = default;
};
using States = std::set<State>;
using Body = std::function<States(const std::string&, const State&)>;
Body epsilon() { return [](const std::string&, const State& s) { return States{s}; }; }
Body literal(char byte) { return [byte](const std::string& text, const State& s) {
    if (s.p == text.size() || text[s.p] != byte) return States{};
    auto t = s; ++t.p; return States{t};
}; }
Body alternate(Body a, Body b) { return [a,b](const std::string& text, const State& s) {
    auto result = a(text,s); auto other = b(text,s); result.insert(other.begin(),other.end()); return result;
}; }
Body sequence(Body a, Body b) { return [a,b](const std::string& text, const State& s) {
    States result; for (const auto& x : a(text,s)) for (const auto& y : b(text,x)) result.insert(y); return result;
}; }
Body capture(std::size_t slot, Body body) { return [slot,body](const std::string& text, const State& s) {
    States result;
    for (auto t : body(text,s)) {
        const Span span{s.p,t.p};
        if (t.pending[slot]) {
            const auto expected = *t.pending[slot];
            if (text.substr(expected.begin,expected.end-expected.begin) != text.substr(span.begin,span.end-span.begin)) continue;
        }
        t.caps[slot] = span; t.pending[slot].reset(); result.insert(t);
    }
    return result;
}; }
Body optional(Body body) { return alternate(epsilon(),body); }
Body bounded(Body body, std::size_t lower, std::size_t upper) {
    return [body,lower,upper](const std::string& text, const State& s) {
        States frontier{s}, result;
        for(std::size_t k=0;k<=upper;++k) {
            if(k>=lower) result.insert(frontier.begin(),frontier.end());
            if(k==upper) break;
            States next; for(const auto& x:frontier) for(const auto& y:body(text,x)) next.insert(y);
            frontier=std::move(next);
        }
        return result;
    };
}
Body backward(std::size_t slot) { return [slot](const std::string& text, const State& s) {
    if (!s.caps[slot]) return States{};
    const auto span = *s.caps[slot]; const auto bytes = text.substr(span.begin,span.end-span.begin);
    if (text.substr(s.p,bytes.size()) != bytes) return States{};
    auto t=s; t.p+=bytes.size(); return States{t};
}; }
std::vector<Outcome> oracle(const Body& body, const std::string& text, std::size_t slots,
                            std::size_t lower, std::size_t upper, bool forward=false, bool back=false) {
    States frontier;
    for (std::size_t p=0; p<=(forward?text.size():0); ++p) {
        State s; s.p=p; s.caps.resize(slots); s.pending.resize(slots);
        if (forward) s.pending[1]=Span{0,p};
        frontier.insert(s);
    }
    std::set<Outcome> outputs;
    for (std::size_t k=0; k<=upper; ++k) {
        if (k>=lower) for (const auto& s:frontier) {
            const auto endings=back?backward(1)(text,s):States{s};
            for (const auto& t:endings) {
                if (t.p!=text.size() || std::any_of(t.pending.begin(),t.pending.end(),[](auto x){return bool(x);})) continue;
                Outcome o; o.end=t.p; o.captures=t.caps; o.captures[0]=Span{0,t.p}; outputs.insert(std::move(o));
            }
        }
        if (k==upper) break;
        States next; for (const auto& s:frontier) for (const auto& t:body(text,s)) next.insert(t);
        frontier=std::move(next);
    }
    return {outputs.begin(),outputs.end()};
}
std::vector<std::string> words() {
    std::vector<std::string> result{""};
    std::size_t begin=0,end=1;
    for(int n=0;n<4;++n) { for(auto i=begin;i<end;++i) for(char c:std::string("ab")) result.push_back(result[i]+c); begin=end;end=result.size(); }
    return result;
}
void differential() {
    const auto a=optional(literal('a')), b=optional(literal('b'));
    struct Fixture { std::string pattern; Body body; std::size_t slots; bool forward=false,back=false; };
    const std::vector<Fixture> fixtures{
        {"(a?)",capture(1,a),2},
        {"(?:(a?)|(b?))",alternate(capture(1,a),capture(2,b)),3},
        {"(?:(a?)(b?))",sequence(capture(1,a),capture(2,b)),3},
        {"((a?)?)",capture(1,optional(capture(2,a))),3},
        {"(?:(a?)|)",alternate(capture(1,a),epsilon()),2},
        {"(?:(a?)|(b?)(a?))",alternate(capture(1,a),sequence(capture(2,b),capture(3,a))),4},
        {"(?:(a?){0,2})",bounded(capture(1,a),0,2),2},
        {"(?:(a?){2}(b?)?)",sequence(bounded(capture(1,a),2,2),optional(capture(2,b))),3},
        {"(?<x>a?)",capture(1,a),2,true},
        {"(?<x>a?)",capture(1,a),2,false,true},
        {"(?<x>a?)",capture(1,a),2,true,true},
        {"(?<x>(a?)?)",capture(1,optional(capture(2,a))),3,true,true},
        {"(?<x>(a?){0,2})",capture(1,bounded(capture(2,a),0,2)),3,true,true},
    };
    std::size_t clamps=0;
    for(const auto& f:fixtures) for(const auto& text:words()) {
        const std::size_t bound=text.size()+2*f.slots+1;
        const std::vector<std::pair<std::size_t,std::size_t>> ranges{
            {0,0},{1,1},{2,2},{3,3},{bound-1,bound-1},{bound,bound},
            {bound+1,bound+1},{bound+5,bound+5},{0,bound+5},{2,bound+5},{bound-1,bound+3}};
        for(auto [lo,hi]:ranges) {
            const auto pattern=std::string(f.forward?"\\k<x>":"")+f.pattern+"{"+std::to_string(lo)+","+std::to_string(hi)+"}"+(f.back?"\\k<x>":"");
            std::string detail;
            if(progress.enabled()) detail="expression="+pattern+" input='"+text+"' bytes="+std::to_string(text.size())+
                " lo="+std::to_string(lo)+" hi="+std::to_string(hi)+" max_repeat=128 share_calls=false"+
                " observer_gate=scalar_capture_relation forward="+std::to_string(f.forward)+" back="+std::to_string(f.back)+default_budget_detail();
            auto case_progress=progress.scope("case",detail);
            const OccurrenceProgram vm(prepare_program(pattern,detail));
            OccurrenceMatchOptions on; on.share_calls=false; on.max_repeat=128;
            auto off=on;off.bound_scalar_repetitions=false;
            OccurrenceStatistics s;
            const auto actual=progress.call("match_outcomes.on",detail,[&]{return vm.match_outcomes(text,0,true,on,&s);});
            const auto control=progress.call("match_outcomes.off",detail,[&]{return vm.match_outcomes(text,0,true,off);});
            const auto expected=progress.call("oracle.scalar_outcomes",detail,[&]{return oracle(f.body,text,f.slots,lo,hi,f.forward,f.back);});
            const auto label=pattern+" on '"+text+"'";
            require(actual==expected,"independent outcome oracle: "+label);
            require(actual==control,"ablation outcome differential: "+label);
            clamps+=s.scalar_repetition_clamps;
            require(progress.call("exists_from.projected.on",detail,[&]{return vm.exists_from(text,0,true,on);})==!expected.empty(),"Boolean projected contract: "+label);
        }
    }
    require(clamps>0,"differential corpus exercised clamp");
}
void huge_and_gates() {
    const auto max=std::to_string(std::numeric_limits<std::size_t>::max());
    const std::vector<std::string> patterns{"(a?){"+max+"}","(a?){1000000,}","(a?){0,"+max+"}"};
    for(const auto& pattern:patterns) {
        std::string setup_detail;
        if(progress.enabled()) {
            const auto lower=pattern.find("{0,")!=std::string::npos?"0":pattern.find("{1000000,")!=std::string::npos?"1000000":max;
            const auto upper=pattern.find("{1000000,")!=std::string::npos?"unbounded":max;
            setup_detail="expression="+pattern+" input_group=empty,a,aaa,b lo="+lower+" hi="+upper+
                " max_repeat=64 share_calls=false observer_gate=scalar_capture_relation"+default_budget_detail();
        }
        OccurrenceProgram vm(prepare_program(pattern,setup_detail,true));
        OccurrenceMatchOptions on;on.share_calls=false;on.max_repeat=64;
        auto off=on;off.bound_scalar_repetitions=false;
        for(const auto& text:std::vector<std::string>{"","a","aaa","b"}) {
            std::string detail;
            if(progress.enabled()) detail=setup_detail+" input='"+text+"' bytes="+std::to_string(text.size())+
                " oracle_lo="+(pattern.find("{0,")!=std::string::npos?"0":"32")+" oracle_hi=32";
            auto case_progress=progress.scope("case",detail);
            OccurrenceStatistics s;
            const auto actual=progress.call("huge.match_outcomes.on",detail,[&]{return vm.match_outcomes(text,0,true,on,&s);},true);
            const auto expected=progress.call("huge.oracle.scalar_outcomes",detail,[&]{return oracle(capture(1,optional(literal('a'))),text,2,
                                      pattern.find("{0,")!=std::string::npos?0:32,32);},true);
            require(actual==expected,"huge repetition exact scalar relation: "+pattern);
            require(s.scalar_repetition_clamps>0 || text=="b","huge repetition clamp recorded");
            if(text=="a") {
                bool limited=false;try{(void)progress.call("huge.match_outcomes.off",detail,[&]{return vm.match_outcomes(text,0,true,off);},true);}catch(const ResourceLimit&){limited=true;}
                require(limited,"unclamped same-budget control exhausts finite unfolding budget");
            }
        }
    }
    // The old unsound one-edge shortcut lost the nonempty final capture here.
    std::string regression_detail;
    if(progress.enabled()) regression_detail="expression=(a?){3} input='a' bytes=1 lo=3 hi=3 max_repeat="+
        std::to_string(OccurrenceMatchOptions{}.max_repeat)+" share_calls=default observer_gate=scalar_capture_relation"+default_budget_detail();
    auto regression_progress=progress.scope("case",regression_detail);
    const OccurrenceProgram regression(prepare_program("(a?){3}",regression_detail));
    const auto outcomes=progress.call("regression.match_outcomes.on",regression_detail,[&]{return regression.match_outcomes("a");});
    require(outcomes.size()==2,"epsilon placement preserves two final captures");
    require(outcomes[0].captures[1]==Span{0,1} && outcomes[1].captures[1]==Span{1,1},"last capture oracle");
    regression_progress.finish();
    // Observable outer counts, occurrence paths and traces cannot be clamped.
    OccurrenceMatchOptions small;small.share_calls=false;small.max_repeat=20;
    for(const auto& pattern:std::vector<std::string>{"(a?){#n:30}","(a?){30}b{=$1[0].@bytes}","(?:(a?)\\1){30}"}) {
        std::string detail;
        if(progress.enabled()) {
            const auto gate=pattern.find("#n")!=std::string::npos?"observable_outer_count":
                pattern.find("@bytes")!=std::string::npos?"observed_occurrence_path":"effectful_backreference";
            detail="expression="+pattern+" input='a' bytes=1 lo=30 hi=30 max_repeat=20 share_calls=false observer_gate="+gate+default_budget_detail();
        }
        auto case_progress=progress.scope("case",detail);
        const OccurrenceProgram vm(prepare_program(pattern,detail,true));OccurrenceStatistics s;
        try{(void)progress.call("gated.match_outcomes.scalar",detail,[&]{return vm.match_outcomes("a",0,true,small,&s);},true);}catch(const ResourceLimit&){}
        require(s.scalar_repetition_clamps==0,"effectful/count/path gate: "+pattern);
    }
    std::string trace_detail;
    if(progress.enabled()) trace_detail="expression=(a?){30} input='' bytes=0 lo=30 hi=30 max_repeat=20 share_calls=false observer_gate=trace_history"+default_budget_detail();
    auto trace_progress=progress.scope("case",trace_detail);
    const OccurrenceProgram traced(prepare_program("(a?){30}",trace_detail,true));OccurrenceStatistics trace_stats;
    bool limited=false;try{(void)progress.call("first_traced_match.trace",trace_detail,[&]{return traced.first_traced_match("",small,&trace_stats);},true);}catch(const ResourceLimit&){limited=true;}
    require(limited && trace_stats.scalar_repetition_clamps==0,"trace retains every finite repetition occurrence");
}
}
int main() {
    differential(); huge_and_gates();
    std::cout<<"scalar repetition quotient checks: "<<checks<<"\n";
}
