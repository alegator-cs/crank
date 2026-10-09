#pragma once
#include <cstddef>
#include <optional>

namespace crank::detail {

enum class IntegerRowStatus { Unsupported, None, Unique, Multiple };

// An exact description of the bounded nonnegative solutions to one positive
// two-variable integer row. For Unique/Multiple, x ranges from first_x through
// last_x in increments of x_step. corresponding_y belongs to first_x; increasing
// x decreases y by a/gcd(a,b). Counts are actual counts, not lower-bound shifts.
// Other fields carry no meaning for Unsupported/None. No count uses a sentinel.
struct IntegerRowResult {
    IntegerRowStatus status = IntegerRowStatus::Unsupported;
    std::size_t first_x = 0;
    std::size_t corresponding_y = 0;
    std::size_t x_step = 0;
    std::size_t last_x = 0;
};

namespace integer_row_detail {

// Preconditions: modulus > 0 and both operands are below modulus. Subtraction
// selects the wrap case before addition, so even SIZE_MAX moduli are safe.
[[nodiscard]] inline std::size_t add_mod(std::size_t x, std::size_t y,
                                         std::size_t modulus) {
    return x >= modulus - y ? x - (modulus - y) : x + y;
}

template<class Step>
[[nodiscard]] std::size_t multiply_mod(std::size_t x, std::size_t y,
                                        std::size_t modulus, Step& step) {
    x %= modulus;
    y %= modulus;
    std::size_t result = 0;
    while (y) {
        step();
        if (y & 1) result = add_mod(result, x, modulus);
        y >>= 1;
        if (y) x = add_mod(x, x, modulus);
    }
    return result;
}

template<class Step>
[[nodiscard]] std::size_t gcd(std::size_t a, std::size_t b, Step& step) {
    while (b) {
        step();
        const auto remainder = a % b;
        a = b;
        b = remainder;
    }
    return a;
}

// Preconditions: modulus > 1, 0 < value < modulus, gcd(value,modulus)==1.
// Track alternating-sign Bezout coefficients by unsigned magnitudes. The
// invariant r*v+s*u=modulus implies u+(r/s)*v <= modulus/s <= modulus.
// Consequently the coefficient multiplication and addition cannot overflow.
template<class Step>
[[nodiscard]] std::size_t inverse_mod(std::size_t value, std::size_t modulus,
                                       Step& step) {
    std::size_t r = modulus, s = value, u = 0, v = 1;
    bool positive = true;
    for (;;) {
        step();
        const auto remainder = r % s;
        if (!remainder) return positive ? v : modulus - v;
        const auto quotient = r / s;
        const auto next = u + quotient * v;
        r = s;
        s = remainder;
        u = v;
        v = next;
        positive = !positive;
    }
}

} // namespace integer_row_detail

// Solve a*x+b*y=total exactly under inclusive nonnegative count bounds.
// Absent upper bounds are unbounded; zero widths are Unsupported. Invalid
// positive-width bounds are None. The caller subtracts any fixed row offset.
// This is a length/count certificate: feasibility does not prove a byte match.
//
// All arithmetic is portable unsigned size_t arithmetic. Each product below
// has a preceding bound that proves it fits. step() runs initially and in every
// loop; any budget/cancellation exception propagates without publishing a result.
template<class Step>
[[nodiscard]] IntegerRowResult solve_integer_row(
    std::size_t a, std::size_t b, std::size_t total,
    std::size_t lower_x, std::optional<std::size_t> upper_x,
    std::size_t lower_y, std::optional<std::size_t> upper_y, Step&& step) {
    step();
    if (!a || !b) return {};
    const auto none = [] { return IntegerRowResult{IntegerRowStatus::None}; };
    if ((upper_x && *upper_x < lower_x) ||
        (upper_y && *upper_y < lower_y)) return none();

    auto remaining = total;
    if (lower_x > remaining / a) return none();
    remaining -= a * lower_x;
    if (lower_y > remaining / b) return none();
    remaining -= b * lower_y;

    // Shift x=lower_x+X and y=lower_y+Y. These upper bounds also prove every
    // multiplication used to tighten the interval is bounded by remaining.
    auto upper_X = remaining / a;
    auto upper_Y = remaining / b;
    if (upper_x && *upper_x - lower_x < upper_X)
        upper_X = *upper_x - lower_x;
    if (upper_y && *upper_y - lower_y < upper_Y)
        upper_Y = *upper_y - lower_y;
    const auto residual = remaining - b * upper_Y;
    const auto lower_X = residual / a + (residual % a != 0);
    if (lower_X > upper_X) return none();

    const auto divisor = integer_row_detail::gcd(a, b, step);
    if (remaining % divisor) return none();
    const auto reduced_a = a / divisor;
    const auto reduced_b = b / divisor;
    const auto reduced_total = remaining / divisor;
    std::size_t residue = 0;
    if (reduced_b != 1) {
        const auto inverse = integer_row_detail::inverse_mod(
            reduced_a % reduced_b, reduced_b, step);
        residue = integer_row_detail::multiply_mod(
            reduced_total, inverse, reduced_b, step);
    }

    // Align the lower bound to the solution residue without adding a modulus
    // to an arbitrary size_t. Never compute the potentially overflowing count
    // of solutions: x+y=SIZE_MAX already has SIZE_MAX+1 possibilities.
    const auto lower_residue = lower_X % reduced_b;
    const auto delta = residue >= lower_residue
        ? residue - lower_residue : reduced_b - (lower_residue - residue);
    if (delta > upper_X - lower_X) return none();
    const auto first_X = lower_X + delta;
    const auto last_X = first_X + ((upper_X - first_X) / reduced_b) * reduced_b;
    const auto first_Y = (reduced_total - reduced_a * first_X) / reduced_b;

    // Positive widths and the original checked row imply that these restored
    // counts are <= total/a and total/b, so restoring lower bounds is safe.
    return {first_X == last_X ? IntegerRowStatus::Unique : IntegerRowStatus::Multiple,
            lower_x + first_X, lower_y + first_Y, reduced_b, lower_x + last_X};
}

} // namespace crank::detail
