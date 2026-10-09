#include "affine_matcher.hpp"
#include "optimizations.hpp"
#include <stdexcept>
#include <iostream>
void require(bool x) { if (!x) throw std::runtime_error("smoke_affine failed"); }
int main() {
auto p=crank::make_affine_plan(crank::parse("(a*)(b*)\\2\\1")); require(p.has_value());
require(p->match("aabbbbaa")); require(!p->match("aabbbaba"));
crank::LengthFilter f(crank::parse("a{#n}b{=n}c{=n}")); require(f.accepts(6)); require(!f.accepts(5));
auto greedy=crank::make_greedy_plan(crank::parse("a*b*")); require(greedy.has_value()); require(greedy->match("aaabb")); require(!greedy->match("aba"));
std::cout << "smoke_affine passed\n";
}
