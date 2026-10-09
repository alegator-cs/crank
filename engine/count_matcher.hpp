#pragma once
#include "regular_matcher.hpp"
#include <utility>

namespace crank {
struct CountStatistics {
    std::size_t candidate_assignments = 0; // Either zero or one; no enumeration.
    std::size_t body_matches = 0;
    std::size_t transition_steps = 0; // Bytes inspected, including byte-set checks.
    std::size_t count_value = 0;
    bool length_rejected = false;
};

// Boolean full membership for a single shared count in a fixed-width sequence.
// Captures are erased; use the state matcher to obtain observable outcomes.
// Runtime work is linear in input length plus the number of fixed fragments.
class CountPlan {
public:
    [[nodiscard]] bool match(std::string_view subject, CountStatistics* = nullptr) const;
    [[nodiscard]] std::string describe() const;
private:
    struct Impl;
    std::shared_ptr<const Impl> impl_;
    explicit CountPlan(std::shared_ptr<const Impl> impl) : impl_(std::move(impl)) {}
    friend std::optional<CountPlan> make_count_plan(const Ast&, RegularOptions);
};

// Declines references, multiple count variables, non-mandatory/repeated count
// binders, variable-width fragments, arithmetic overflow, and DFA compile caps.
[[nodiscard]] std::optional<CountPlan> make_count_plan(const Ast&, RegularOptions = {});
}
