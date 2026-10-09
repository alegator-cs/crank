#pragma once
#include "regular_matcher.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace crank {

// A Boolean full-match specialization for a single flat capture and its exact
// copies. Capture outcomes and arbitrary endpoints still require environments.
class EqualityPlan {
public:
    // Statistics aggregate component DFA scans. Exact byte comparisons are not
    // DFA transitions and are excluded from transition_steps.
    [[nodiscard]] bool match(std::string_view subject, RegularStatistics* = nullptr) const;
    [[nodiscard]] std::string describe() const;

private:
    struct Impl;
    std::shared_ptr<const Impl> impl_;
    explicit EqualityPlan(std::shared_ptr<const Impl>);
    friend std::optional<EqualityPlan> make_equality_plan(const Ast&, RegularOptions);
};

// Unsupported shapes or regular compilation limits produce no plan, so callers
// can retain their existing state matcher and resource-limit behavior.
[[nodiscard]] std::optional<EqualityPlan> make_equality_plan(
    const Ast&, RegularOptions options = {});

} // namespace crank
