#include "reference/span_regex.hpp"
#include <stdexcept>
#include <iostream>
void require(bool x) { if (!x) throw std::runtime_error("smoke_span failed"); }
int main() {
crank::experimental::Program p("(a|b)*");
require(p.full_match("abab")); require(!p.full_match("abac"));
require(p.endpoints("abac") == std::vector<std::size_t>({0,1,2,3}));
crank::experimental::Program q("(a|aa)*b"); require(q.full_match("aaaab"));
std::cout << "smoke_span passed\n";
}
