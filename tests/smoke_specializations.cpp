#include "count_matcher.hpp"
#include "equality_matcher.hpp"
#include <stdexcept>
#include <iostream>
void require(bool x) { if (!x) throw std::runtime_error("smoke_specializations failed"); }
int main() {
auto count=crank::make_count_plan(crank::parse("a{#n}b{=n}c{=n}")); require(count.has_value());
require(count->match("aabbcc")); require(!count->match("aabbc")); require(!count->match("aabcbc"));
auto equal=crank::make_equality_plan(crank::parse("(?<x>[ab]*)\\k<x>")); require(equal.has_value());
require(equal->match("abbaabba")); require(!equal->match("abbaabab"));
require(!crank::make_count_plan(crank::parse("(?:a|aa){#n}b{=n}")).has_value());
std::cout << "smoke_specializations passed\n";
}
