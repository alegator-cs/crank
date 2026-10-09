#include "regular_matcher.hpp"
#include <stdexcept>
#include <iostream>
#include <random>
void require(bool x) { if (!x) throw std::runtime_error("smoke_regular failed"); }
int main() {
    crank::RegularProgram p(crank::parse("(?:ab|ba)*c?"));
    for (const auto* good : {"", "ab", "baabc"}) require(p.full_match(good));
    for (const auto* bad : {"a", "abb", "cc"}) require(!p.full_match(bad));
    require(p.endpoints("abba!") == std::vector<std::size_t>({0,2,4}));

    // The paired lowering acts only on sufficiently long Boolean scans. A
    // two-byte exhaustive oracle is embedded into sixteen-byte subjects so
    // every byte class, both parity positions, and the paired path are covered.
    const auto ast=crank::parse("(?:[\\x00-\\x7f][\\x80-\\xff])*");
    crank::RegularOptions old_options; old_options.paired_transitions=false;
    const crank::RegularProgram pairs(ast),old(ast,old_options);
    const auto counts=pairs.construction_statistics();
    require(counts.paired_table_bytes>0 && counts.paired_table_bytes<=65536);
    require(old.construction_statistics().paired_table_bytes==0);
    std::string subject;
    for(unsigned i=0;i<8;++i) {subject+='a';subject+=static_cast<char>(0xff);}
    for(unsigned a=0;a<256;++a) for(unsigned b=0;b<256;++b) {
        subject[0]=static_cast<char>(a);subject[1]=static_cast<char>(b);
        const bool expected=a<128 && b>=128;
        crank::RegularStatistics stats;
        require(pairs.full_match(subject,&stats)==expected);
        require(old.full_match(subject)==expected);
        require(stats.transition_steps==subject.size()); // logical coverage
    }
    for(std::size_t cap:{std::size_t{0},counts.paired_table_bytes-1}) {
        crank::RegularOptions limited;limited.max_paired_table_bytes=cap;
        const crank::RegularProgram fallback(ast,limited);
        require(fallback.construction_statistics().paired_table_bytes==0);
        require(fallback.full_match(subject)==old.full_match(subject));
    }
    std::mt19937 random(91726);
    for(const auto* pattern:{"(?:ab|ba)*c?","(?:a|ab)*b","[ab]*a[ab]{3}",
                            "(?:ab|a|c){0,20}","(?:[\\x00-\\xff]a)*"}) {
        const auto source=crank::parse(pattern);
        const crank::RegularProgram compiled(source),interpreted(source,old_options);
        for(unsigned q=0;q<512;++q) {
            std::string input;
            for(unsigned i=0,n=random()%130;i<n;++i) input+="abcX"[random()%4];
            require(compiled.full_match(input)==interpreted.full_match(input));
            require(compiled.endpoints(input)==interpreted.endpoints(input));
        }
    }
    for(unsigned n:{7U,8U,9U,15U,16U,17U,18U,31U,32U,33U}) {
        std::string input(n,'a');
        const auto source=crank::parse("a*");
        const crank::RegularProgram repeated(source);
        require(repeated.full_match(input));
        for(unsigned i=0;i<n;++i) {input[i]='b';require(!repeated.full_match(input));input[i]='a';}
    }
    std::cout << "smoke_regular passed (paired DFA oracle: 65536 inputs, 2560 differential inputs)\n";
}
