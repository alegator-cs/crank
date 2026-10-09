#include "regex_ast.hpp"
#include "occurrence_analysis.hpp"
#include <stdexcept>
#include <iostream>
void require(bool x) { if (!x) throw std::runtime_error("smoke_analysis failed"); }
int main() {
auto ast = crank::parse("(?<S>a(?&S)b|)");
require(ast.has_calls && !ast.regular());
crank::OccurrenceAnalysis facts(ast);
const auto& length = facts.length(ast.root);
for (std::size_t n=0; n<32; ++n) require(length.accepts(n) == (n%2==0));
const auto& boundary = facts.boundary(ast.root);
require(boundary.nullable && boundary.first.test('a') && boundary.last.test('b'));
require(!boundary.first.test('b'));
bool invalid=false; try { (void)crank::parse("(?&missing)"); } catch(const std::invalid_argument&) { invalid=true; }
require(invalid);
std::cout << "smoke_analysis passed\n";
}
