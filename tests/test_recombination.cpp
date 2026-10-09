#include "ast_machine.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
using namespace crank;
static std::size_t checks;
void check(bool value, const std::string& message) { ++checks; if (!value) throw std::runtime_error(message); }
std::vector<std::string> words(std::string alphabet, unsigned length) {
    std::vector<std::string> result{std::string{}}, layer=result;
    while(length--) { std::vector<std::string> next; for(const auto& prefix:layer) for(char c:alphabet) next.push_back(prefix+c); result.insert(result.end(),next.begin(),next.end()); layer=std::move(next); }
    return result;
}
int main() {
  try {
    std::vector<std::string> patterns = {
      "", "a|", "a*", "(?:a|aa)*", "(?:a?|a)*", "a*ab", "a*(aa)*a*b",
      "(?:a?|b?){2,4}a?", "(?:|a){3,}", "(?<A>a|aa|)a*", "((a)|(a))?",
      "(?<S>(?&S)(?&S)|a)", "(?<S>(?&S)(?&S)|a|)",
      "(?<S>(?&S)a|)", "(?<S>a(?&S)|)", "(?<S>a(?&S)b|)",
      "(?<S>[(](?&S)[)](?&S)|)", "(?<E>(?&E)[+](?&E)|a)",
      "(?(DEFINE)(?<A>(?&B)|a)(?<B>(?&A)|))(?&A)(?&B)",
      "(?(DEFINE)(?<A>(?&B))(?<B>(?&A)))(?&A)",
      "(?(DEFINE)(?<A>(?&A)|a|))(?&A){2,4}",
      "(?(DEFINE)(?<A>(?&B)a|b)(?<B>(?&A)b|a))(?&A)",
      "((a|aa)*)(a*)", "(?:a|aa){2,5}(?:|a)*", "(?:a?|b?)*a?",
      "(?(DEFINE)(?<R>(?&R)(?&R)|a))(?&R)b|(?&R)a",
      "(?:a?|(?:a|aa)*|a){2,4}b", "(?<A>a|)(?&A)(?&A)a*"
    };
    std::string wide;
    for(unsigned i=0;i<70;++i) wide += "a?";
    patterns.push_back(wide + "b");
    patterns.push_back("(?:a?){63,66}b");
    patterns.push_back("(?:a?|b?){64,}b");
    const auto subjects=words("ab",5);
    for(const auto& pattern:patterns) {
      const auto ast=parse(pattern); detail::AstMachineProgram program(ast);
      for(const auto& subject:subjects) for(bool prune:{false,true}) for(bool collapse:{false,true}) {
        OccurrenceMatchOptions oldopt, newopt;
        // Isolate the historical return-coalescing representation. Regional
        // propagation is independently tested and has its own workspace cost.
        oldopt.regional_execution=oldopt.regional_constraints=false;
        // Preserve this historical representation under the newer root tiers.
        oldopt.bounded_word_prediction=oldopt.deterministic_programs=false;
        oldopt.coalesce_returns=false; oldopt.prune=prune;oldopt.collapse_control=collapse;
        newopt=oldopt; newopt.coalesce_returns=true;
        OccurrenceStatistics oldstats,newstats;
        const bool expected=program.full_match(subject,oldopt,&oldstats);
        check(program.full_match(subject,newopt,&newstats)==expected,"membership "+pattern+" : "+subject);
        check(newstats.states<=oldstats.states,"states "+pattern+" : "+subject);
        check(newstats.work_steps<=oldstats.work_steps,"work "+pattern+" : "+subject);
        newopt.max_states=oldstats.states;newopt.max_steps=oldstats.work_steps;
        check(program.full_match(subject,newopt)==expected,"exact original budget "+pattern+" : "+subject);
        newopt=oldopt;newopt.coalesce_returns=true;
        for(std::size_t start=0;start<=subject.size();++start) {
          const auto ends=program.endpoints(subject,start,oldopt,&oldstats);
          check(program.endpoints(subject,start,newopt,&newstats)==ends,"endpoints "+pattern+" : "+subject);
          check(newstats.states<=oldstats.states,"endpoint states "+pattern+" : "+subject);
          check(newstats.work_steps<=oldstats.work_steps,"endpoint work "+pattern+" : "+subject);
        }
        const auto a=program.first_match(subject,oldopt,&oldstats), b=program.first_match(subject,newopt,&newstats);
        check(a.has_value()==b.has_value(),"witness presence");
        if(a) check(a->captures==b->captures,"witness captures");
        check(oldstats.states==newstats.states && oldstats.work_steps==newstats.work_steps,"witness untouched");
      }
    }
    // Catalan grammar has exactly the nonempty unary words despite exponentially
    // many trees. This independent oracle tests dense shared completion fanout.
    detail::AstMachineProgram catalan(parse("(?<R>(?&R)(?&R)|a)"));
    for(unsigned n=0;n<=80;++n) {
      const std::string subject(n,'a'); OccurrenceStatistics stats;
      check(catalan.full_match(subject,{},&stats)==(n>0),"Catalan membership oracle");
      const auto ends=catalan.endpoints(subject);
      std::vector<std::size_t> expected;for(unsigned i=1;i<=n;++i) expected.push_back(i);
      check(ends==expected,"Catalan endpoints oracle");
    }
    // A pure nullable repeat can pad with epsilon; enormous written counts
    // must not force enormous control-count unfolding. The upper bound still
    // limits nonempty iterations when it is smaller than input capacity.
    const auto huge = std::to_string(std::numeric_limits<std::size_t>::max());
    const auto almost = std::to_string(std::numeric_limits<std::size_t>::max()-1);
    for(const auto& count : std::vector<std::string>{"{"+huge+"}", "{"+almost+","+huge+"}",
             "{"+huge+",}", "{0,"+huge+"}", "{3,"+huge+"}"}) {
      detail::AstMachineProgram unary(parse("(a?)"+count));
      detail::AstMachineProgram pairs(parse("(?:|aa)"+count));
      detail::AstMachineProgram suffix(parse("(a?)"+count+"bc"));
      for(bool prune : {false,true}) {
        OccurrenceMatchOptions options;options.prune=prune;
        options.max_states=20000;options.max_steps=80000;
        for(const auto& subject : words("ab",5)) {
          const bool only_a=subject.find('b')==std::string::npos;
          check(unary.full_match(subject,options)==only_a,"huge nullable unary membership");
          check(pairs.full_match(subject,options)==(only_a && subject.size()%2==0),"huge nullable pair membership");
        }
        check(unary.endpoints("xxaaab",2,options)==std::vector<std::size_t>({2,3,4,5}),"huge nullable nonzero endpoints");
        check(pairs.endpoints("xxaaaab",2,options)==std::vector<std::size_t>({2,4,6}),"huge nullable residue endpoints");
        check(suffix.full_match("aaabc",options),"huge nullable exact suffix demand");
        check(!suffix.full_match("aaabbc",options),"huge nullable exact suffix rejection");
        check(suffix.endpoints("xxaaabcq",2,options)==std::vector<std::size_t>({7}),"huge nullable suffix endpoints at nonzero start");
      }
    }
    for(unsigned lower=0;lower<=4;++lower) for(unsigned upper=lower;upper<=4;++upper) {
      detail::AstMachineProgram finite(parse("(?:|a){"+std::to_string(lower)+","+std::to_string(upper)+"}"));
      for(bool prune : {false,true}) {
        OccurrenceMatchOptions options;options.prune=prune;
        for(unsigned length=0;length<=6;++length) {
          const std::string subject(length,'a');
          check(finite.full_match(subject,options)==(length<=upper),"nullable bounded upper remains binding");
          std::vector<std::size_t> expected;
          for(unsigned end=0;end<=std::min(length,upper);++end) expected.push_back(end);
          check(finite.endpoints(subject,0,options)==expected,"nullable bounded endpoint oracle");
        }
      }
    }
    detail::AstMachineProgram nested_nullable(parse("(?<N>a(?&N)b|){"+huge+"}"));
    for(bool prune : {false,true}) {
      OccurrenceMatchOptions options;options.prune=prune;
      for(const auto& subject : {"", "ab", "aabb", "aabbab", "abaabbaaabbb"})
        check(nested_nullable.full_match(subject,options),"huge nullable recursive positive");
      for(const auto& subject : {"a", "abb", "aababb", "ba", "abaabbx"})
        check(!nested_nullable.full_match(subject,options),"huge nullable recursive negative");
      detail::AstMachineProgram nonnullable(parse("(?:ab){"+huge+"}"));
      check(!nonnullable.full_match("ab",options),"huge nonnullable lower preserved");
      detail::AstMachineProgram cycle(parse("(?<N>(?&N)){"+huge+"}"));
      check(!cycle.full_match("",options),"unproductive cycle never treated as nullable");
    }
    // Deferred optional work must neither restart recognition nor consume its
    // state/work quotas; declining the hook keeps precisely the native work.
    struct AdmissionContext {
      const detail::AtomicExclusions* bits;
      std::size_t calls=0, at=0;
      static const detail::AtomicExclusions* admit(void* opaque, std::size_t steps) noexcept {
        auto& context=*static_cast<AdmissionContext*>(opaque);
        ++context.calls; context.at=steps; return context.bits;
      }
    };
    const auto delayed_ast=parse("(?<R>(?&R)(?&R)|a)");
    detail::AstMachineProgram delayed(delayed_ast);
    detail::AtomicExclusions empty(delayed_ast.nodes.size());
    for(const bool supply_empty_table : {false,true}) {
      AdmissionContext context{supply_empty_table ? &empty : nullptr};
      detail::DeferredAssistance hook{129, &context, &AdmissionContext::admit};
      OccurrenceStatistics ordinary, assisted;
      const std::string subject(80,'a');
      check(delayed.full_match(subject,{},&ordinary),"deferred oracle native");
      OccurrenceMatchOptions exact;
      exact.max_states=ordinary.states;exact.max_steps=ordinary.work_steps;
      check(delayed.full_match_deferred(subject,exact,hook,&assisted),"deferred empty/decline match");
      check(context.calls==1 && context.at==192,"deferred poll rounds threshold once");
      check(ordinary.states==assisted.states && ordinary.work_steps==assisted.work_steps,"deferred empty/decline budget parity");
    }
    AdmissionContext cheap{&empty};
    detail::DeferredAssistance late{1000000,&cheap,&AdmissionContext::admit};
    check(delayed.full_match_deferred("a",{},late),"cheap query native result");
    check(cheap.calls==0,"cheap query never starts optional work");
    detail::AtomicExclusions impossible(delayed_ast.nodes.size());impossible.reject_root();
    AdmissionContext reject{&impossible};
    detail::DeferredAssistance early{65,&reject,&AdmissionContext::admit};
    OccurrenceStatistics rejected;
    check(!delayed.full_match_deferred(std::string(40,'a')+"x"+std::string(40,'a'),{},early,&rejected),"delayed certified root rejection");
    check(reject.calls==1 && reject.at==128 && rejected.work_steps==128,"root proof consumed at admission");
    detail::AtomicExclusions wrong(delayed_ast.nodes.size()+1);
    AdmissionContext invalid{&wrong};
    detail::DeferredAssistance malformed{0,&invalid,&AdmissionContext::admit};
    bool invalid_thrown=false;
    try { (void)delayed.full_match_deferred("a",{},malformed); }
    catch(const std::invalid_argument&) { invalid_thrown=true; }
    check(invalid_thrown,"deferred table identity count validation");
    std::cout << "native recombination checks: " << checks << "\n";
  } catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
