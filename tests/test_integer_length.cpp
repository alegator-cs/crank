#include "optimizations.hpp"

#include <algorithm>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
std::size_t checks = 0, rows = 0, admitted = 0;
void require(bool condition, const std::string& message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
using Bound = std::optional<std::size_t>;
std::string term(char c, std::size_t width, std::size_t lower, Bound upper) {
    return "(?:" + std::string(1, c) + "{" + std::to_string(width) + "}){" +
        std::to_string(lower) + "," + (upper ? std::to_string(*upper) : "") + "}";
}
bool attainable(std::size_t total, std::size_t offset,
                std::size_t a, std::size_t lx, Bound ux,
                std::size_t b, std::size_t ly, Bound uy) {
    if (total < offset) return false;
    total -= offset;
    const auto last_x = std::min(ux.value_or(total / a), total / a);
    const auto last_y = std::min(uy.value_or(total / b), total / b);
    if (lx > last_x || ly > last_y) return false;
    for (auto x = lx;; ++x) {
        const auto left = total - a * x;
        for (auto y = ly;; ++y) {
            if (y <= left / b && y * b == left) return true;
            if (y == last_y) break;
        }
        if (x == last_x) break;
    }
    return false;
}
void exhaustive_rows() {
    const std::vector<std::pair<std::size_t, Bound>> bounds{
        {0, {}}, {1, {}}, {0, 3}, {2, 5}, {0, 0}, {3, 3}};
    for (std::size_t a = 1; a <= 12; ++a)
        for (std::size_t b = 1; b <= 12; ++b)
            for (std::size_t offset : {0U, 3U})
                for (auto [lx, ux] : bounds)
                    for (auto [ly, uy] : bounds) {
                        const auto pattern = std::string(offset, 'z') +
                            term('a', a, lx, ux) + term('b', b, ly, uy);
                        const auto ast = crank::parse(pattern);
                        const crank::LengthFilter filter(ast), disabled(ast, false);
                        ++rows;
                        admitted += filter.has_integer_row();
                        require(!disabled.has_integer_row(), "construction ablation has no row");
                        for (std::size_t length = 0; length <= 120; ++length) {
                            const auto expected = attainable(length, offset, a, lx, ux, b, ly, uy);
                            const auto answer = filter.check_integer_row(length);
                            require(!expected || filter.accepts(length), "old filter sound");
                            require(filter.accepts(length) == disabled.accepts(length),
                                    "construction leaves old cheap filter unchanged");
                            require(!answer.exhausted, "small row fits default local budget");
                            require(!answer.impossible || !expected, "integer proof never rejects attainable length");
                            if (answer.checked)
                                require(answer.impossible == !expected, "admitted row is exact for independent lengths");
                            else require(answer.steps == 0 && !answer.impossible, "declined row does no query work");
                        }
                    }
}
void supported_and_declined_shapes() {
    for (const auto& pattern : {
        "(ab|cd)*(efg|hij)*", "(?<capture>ab)*(cde)*",
        "()(?:)*z(ab)*(cde)*", "(ignored){0}(ab)*(cde)*",
        "((ab)*){1}(cde)*", "(ab){2,5}(cde){1,4}"}) {
        const crank::LengthFilter filter(crank::parse(pattern));
        require(filter.has_integer_row(), std::string("expected row: ") + pattern);
    }
    for (const auto& pattern : {
        "a*b*", "(aa)*(aaaa)*", "a+", "literal", "(ab)*(cde)*(fg)*",
        "(a|aa)*(bbb)*", "(ab)*|(cde)*", "((ab)*)*(cde)*",
        "((ab)*){2}(cde)*", "(?<x>ab)*\\k<x>(cde)*",
        "a{#n}b{=n}", "(?<S>a(?&S)b|)",
        "(?(DEFINE)(?<S>ab))(?&S)*(cde)*"}) {
        const crank::LengthFilter filter(crank::parse(pattern));
        require(!filter.has_integer_row(), std::string("expected conservative decline: ") + pattern);
        const auto answer = filter.check_integer_row(17);
        require(!answer.checked && !answer.impossible && !answer.exhausted && answer.steps == 0,
                "unsupported form does no query work");
    }
    std::string depth = "(ab)*(cde)*";
    for (unsigned i = 0; i < 130; ++i) depth = "(" + depth + ")";
    require(!crank::LengthFilter(crank::parse(depth)).has_integer_row(), "bounded traversal depth declines");
    require(!crank::LengthFilter(crank::parse(std::string(5000, 'z') + "(ab)*(cde)*")).has_integer_row(),
            "bounded traversal size declines");
}
void boundaries_and_budgets() {
    const crank::LengthFilter hole(crank::parse("(a{127})*(a{131})*"));
    require(hole.has_integer_row() && hole.accepts(16379), "target passes weak filter");
    const auto proof = hole.check_integer_row(16379);
    require(proof.checked && proof.impossible && !proof.exhausted && proof.steps > 3,
            "target is proved impossible");
    for (std::size_t budget = 0; budget < proof.steps; ++budget) {
        const auto partial = hole.check_integer_row(16379, budget);
        require(partial.checked && partial.exhausted && !partial.impossible && partial.steps == budget,
                "every interrupted cut point has no conclusion");
    }
    require(hole.check_integer_row(16379, proof.steps).impossible, "exact solver budget suffices");
    require(!hole.check_integer_row(16380).impossible, "adjacent feasible is not rejected");
    const auto maximum = std::numeric_limits<std::size_t>::max();
    const auto max = std::to_string(maximum);
    const crank::LengthFilter large(crank::parse("(a{" + max + "})*(b{" +
        std::to_string(maximum - 1) + "})*"));
    require(large.has_integer_row(), "machine-width coefficients admitted safely");
    require(!large.check_integer_row(maximum).impossible, "maximum feasible coefficient");
    require(!large.check_integer_row(maximum - 1).impossible, "adjacent large coefficient");
    require(large.check_integer_row(maximum - 2).impossible, "large impossible length");
    require(!crank::LengthFilter(crank::parse("a{" + max + "}a(ab)*(cde)*")).has_integer_row(),
            "offset overflow declines");
    require(!crank::LengthFilter(crank::parse("(a{" + max + "}a)*(cde)*")).has_integer_row(),
            "body width overflow declines");
    require(!crank::LengthFilter(crank::parse("(a{" + max + "}){2}(ab)*(cde)*")).has_integer_row(),
            "constant repetition product overflow declines");
    const crank::LengthFilter zero(crank::parse("(?:)*(ab)*(cde)*"));
    require(zero.has_integer_row() && !zero.check_integer_row(0).impossible,
            "zero-width repetitions contribute no coefficient");
    const crank::LengthFilter offset(crank::parse("xyz(ab)*(cde)*"));
    require(offset.check_integer_row(2).impossible, "fixed offset excludes shorter length");
    const auto stopped = offset.check_integer_row(2, 0);
    require(stopped.exhausted && !stopped.impossible, "zero budget also disables offset conclusion");
}
}
int main() {
    exhaustive_rows();
    supported_and_declined_shapes();
    boundaries_and_budgets();
    std::cout << "integer length certificate passed: checks=" << checks
              << " rows=" << rows << " admitted=" << admitted << '\n';
}
