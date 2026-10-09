#include "fragment_matcher.hpp"
#include <stdexcept>
#include <iostream>
void require(bool x) { if (!x) throw std::runtime_error("smoke_fragments failed"); }
int main() {
auto ast=crank::parse("[a-z]{2}[0-9]"); crank::RegularFragment p(ast,ast.root);
require(p.full_match("ab3")); require(!p.full_match("a33"));
require(p.matching_copies("ab3cd4",3,2)==2); require(p.matching_copies("ab3c44",3,2)==1);
crank::ExternalRegularProgram e(ast,ast.root);
if (!e.compiled_with_pcre2()) require(!e.try_full_match("ab3").has_value());
std::cout << "smoke_fragments passed\n";
}
