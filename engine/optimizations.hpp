#pragma once
#include "regex_ast.hpp"
#include <array>

namespace crank {
struct GreedySegment {
    std::bitset<256> characters;
    std::size_t lower = 0;
    std::optional<std::size_t> upper;
};
// A certificate for Boolean full membership only. Captures are erased, and
// canonical regrouping may change both captures and intermediate endpoints.
struct GreedyPlan {
    std::vector<GreedySegment> segments;
    std::size_t certified_sites = 0;
    std::size_t simplifications = 0;
    [[nodiscard]] bool match(std::string_view) const;
    [[nodiscard]] std::string describe() const;
};
[[nodiscard]] std::optional<GreedyPlan> make_greedy_plan(const Ast&);

// A bounded proof attempt. Only impossible licenses rejection; a feasible row
// is not a byte/order match, and an interrupted attempt carries no conclusion.
struct IntegerLengthCheck {
    bool checked = false;
    bool impossible = false;
    bool exhausted = false;
    std::size_t steps = 0;
};
struct IntegerLengthTerm {
    std::size_t width = 0, lower = 0;
    std::optional<std::size_t> upper;
};
struct IntegerLengthRow {
    std::size_t offset = 0;
    std::array<IntegerLengthTerm, 2> terms;
};

// Necessary conditions only: ranges, a congruence, and (when extraction is
// justified) a shared-count affine length equation. Never proves membership.
class LengthFilter {
public:
    explicit LengthFilter(const Ast&, bool prepare_integer_row = true);
    [[nodiscard]] bool accepts(std::size_t length) const;
    // Root full-span necessary condition only. The independent local allowance
    // never consumes native recognition's state or work budget.
    [[nodiscard]] IntegerLengthCheck check_integer_row(
        std::size_t length, std::size_t max_steps = 256) const;
    [[nodiscard]] bool has_integer_row() const { return integer_row_.has_value(); }
    [[nodiscard]] std::string describe() const;
private:
    std::size_t minimum_ = 0;
    std::optional<std::size_t> maximum_;
    std::size_t offset_ = 0, modulus_ = 1;
    bool affine_ = false;
    std::size_t affine_offset_ = 0, affine_gcd_ = 0;
    std::vector<std::size_t> coefficients_;
    std::vector<std::string> names_;
    std::optional<IntegerLengthRow> integer_row_;
};
}
