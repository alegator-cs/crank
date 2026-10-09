#pragma once
#include "regex_ast.hpp"
#include <compare>
#include <stdexcept>

namespace crank {
struct Span {
    std::size_t begin = 0, end = 0;
    auto operator<=>(const Span&) const = default;
};
struct Outcome {
    std::size_t end = 0;
    std::vector<std::optional<Span>> captures;
    std::vector<std::optional<std::size_t>> counts;
    auto operator<=>(const Outcome&) const = default;
};
// Compatibility options for the shared occurrence/continuation VM. State units
// now count cumulative allocations, not the removed scalar engine's peak-live
// storage. Numerical thresholds and statistics need not match historical runs.
struct MatchOptions {
    std::size_t max_states = 200000;
    std::size_t max_steps = 2000000;
    std::size_t max_repeat = 10000;
    bool prune = true;
};
struct MatchStatistics {
    std::size_t states = 0;
    std::size_t steps = 0;
    std::size_t memo_hits = 0;
    std::size_t pruned = 0;
};
class ResourceLimit : public std::runtime_error {
public: using std::runtime_error::runtime_error;
};
// All distinct observable outcomes. No leftmost-first or POSIX capture priority.
// Unresolved forward obligations are excluded. All offsets refer to original input.
[[nodiscard]] std::vector<Outcome> match_states(
    const Ast&, std::string_view subject, std::size_t start = 0,
    bool full = true, MatchOptions options = {}, MatchStatistics* statistics = nullptr);
// Boolean membership with globally unobserved captures/counts projected away.
// Every reference target and every used count remains in the matching state;
// no capture outcomes or capture-priority behavior are exposed by this API.
[[nodiscard]] bool match_exists(
    const Ast&, std::string_view subject, std::size_t start = 0,
    bool full = true, MatchOptions options = {}, MatchStatistics* statistics = nullptr);
// One valid observable outcome; selection is deterministic but has no
// Perl/POSIX priority contract. Preserves full environments during evaluation,
// then materializes only the first complete result.
[[nodiscard]] std::optional<Outcome> match_first(
    const Ast&, std::string_view subject, std::size_t start = 0,
    bool full = true, MatchOptions options = {}, MatchStatistics* statistics = nullptr);
// All successful prefix endpoints with the same compact environment as
// match_exists. Unresolved forward obligations are excluded; results are sorted
// and unique, with no capture-outcome materialization.
[[nodiscard]] std::vector<std::size_t> match_endpoints(
    const Ast&, std::string_view subject, std::size_t start = 0,
    MatchOptions options = {}, MatchStatistics* statistics = nullptr);
}
