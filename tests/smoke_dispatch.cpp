#include "crank.hpp"
#include <stdexcept>
#include <iostream>
void require(bool x) { if (!x) throw std::runtime_error("smoke_dispatch failed"); }
int main() {
crank::Program regular("(a|b)*"); require(regular.full_match("abab"));
crank::Program recursive("(?<S>a(?&S)b|)"); crank::RunStatistics stats;
require(recursive.full_match("aaabbb",{},&stats)); require(stats.backend=="native-ast");
crank::Program history("(?:a{#cols};){#rows}b{=rows[1].cols}"); require(history.full_match("a;aaa;aa;bbb"));
require(history.first_trace("a;aaa;aa;bbb").has_value());
require(regular.endpoints("aba!")==std::vector<std::size_t>({0,1,2,3}));
std::cout << "smoke_dispatch passed\n";
}
