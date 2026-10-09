#include "occurrence_matcher.hpp"
#include <stdexcept>
#include <iostream>
void require(bool x) { if (!x) throw std::runtime_error("smoke_occurrence failed"); }
int main() {
crank::OccurrenceProgram p(crank::parse("(?<S>a(?&S)b|)"));
require(p.supports_shared_calls()); require(p.full_match("aaabbb")); require(!p.full_match("aaabb"));
crank::OccurrenceProgram left(crank::parse("(?<S>(?&S)a|)")); require(left.full_match("aaa"));
crank::OccurrenceProgram history(crank::parse("(?:a{#cols};){#rows}b{=rows[1].cols}"));
require(history.full_match("a;aaa;aa;bbb")); require(!history.full_match("a;aaa;aa;bb"));
require(history.first_traced_match("a;aaa;aa;bbb").has_value());
std::cout << "smoke_occurrence passed\n";
}
