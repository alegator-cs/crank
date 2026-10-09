#include "integer_row.hpp"
#include <stdexcept>
#include <iostream>
void require(bool x) { if (!x) throw std::runtime_error("smoke_integer failed"); }
int main() {
using crank::detail::IntegerRowStatus;
std::size_t steps = 0;
auto tick = [&] { ++steps; };
auto solve = [&](std::size_t total) {
    return crank::detail::solve_integer_row(127, 131, total, 0, {}, 0, {}, tick);
};
require(solve(16379).status == IntegerRowStatus::None);
auto unique = solve(254);
require(unique.status == IntegerRowStatus::Unique && unique.first_x == 2 && unique.corresponding_y == 0);
require(steps != 0);
std::cout << "smoke_integer passed\n";
}
