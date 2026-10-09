#include "state_matcher.hpp"
#include <stdexcept>
#include <iostream>
void require(bool x) { if (!x) throw std::runtime_error("smoke_state failed"); }
int main() {
auto ast = crank::parse("(?<x>a*)b\\k<x>");
require(crank::match_exists(ast,"aabaa")); require(!crank::match_exists(ast,"aaba"));
auto first=crank::match_first(ast,"aabaa"); require(first.has_value());
require(first->captures.at(1)->begin==0 && first->captures.at(1)->end==2);
auto count=crank::parse("a{#n}b{=n}"); require(crank::match_exists(count,"aaabbb")); require(!crank::match_exists(count,"aabbb"));
std::cout << "smoke_state passed\n";
}
