#pragma once
#include "regular_matcher.hpp"
#include "state_matcher.hpp"
#include <memory>

namespace crank {
struct AffineMatchOptions {
    // Residual plans only: candidates and conservative validation span bytes.
    // Exhaustion throws ResourceLimit; uniquely solved plans ignore these limits.
    std::size_t max_candidates = 100000;
    std::size_t max_validation_bytes = 64 * 1024 * 1024;
};
struct AffineStatistics {
    std::size_t variables = 0;
    std::size_t equations = 0;
    std::size_t histogram_bytes = 0;
    std::size_t direct_count_bytes = 0; // Sum over selected range scans.
    std::size_t observed_bytes = 0; // Saturating histogram + range + delimiter work.
    std::size_t delimiter_bytes = 0;
    std::size_t delimiter_searches = 0;
    std::size_t validated_bytes = 0;
    std::size_t equality_bytes = 0;
    std::size_t candidate_assignments = 0;
    std::size_t free_variables = 0;
    std::size_t candidate_lower = 0, candidate_upper = 0, candidate_stride = 1;
    // Boolean question decided: successful witness or exhaustive rejection.
    bool search_complete = false;
    bool resource_limited = false;
    bool arithmetic_rejected = false;
    std::vector<std::size_t> values;
};

// Exact Boolean membership for a once-executed sequence whose span/count
// variables are determined by exact observations, optionally leaving one
// bounded integer degree of freedom. Ordered validation certifies each candidate.
class AffinePlan {
public:
    [[nodiscard]] bool match(std::string_view, AffineStatistics* = nullptr,
                             AffineMatchOptions = {}) const;
    [[nodiscard]] bool residual() const;
    [[nodiscard]] std::string describe() const;
private:
    struct Impl;
    std::shared_ptr<const Impl> impl_;
    explicit AffinePlan(std::shared_ptr<const Impl> impl) : impl_(std::move(impl)) {}
    friend std::optional<AffinePlan> make_affine_plan(const Ast&, RegularOptions);
};
// Unsupported shapes, more than one free variable, arithmetic growth and compile limits
// disable this optimization. No constraint is approximated into acceptance.
[[nodiscard]] std::optional<AffinePlan> make_affine_plan(const Ast&, RegularOptions = {});
}
