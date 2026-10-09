#pragma once
#include "affine_matcher.hpp"
#include "count_matcher.hpp"
#include "occurrence_matcher.hpp"
#include "regular_matcher.hpp"
#include "state_matcher.hpp"
#include <memory>

namespace crank {
enum class Backend { Auto, Dfa, States, Greedy, Pcre2, Occurrences };
struct CompileOptions {
    // DFA compilation includes eligible acyclic call graphs and is bounded.
    // Auto falls back to the native request/return
    // recognizer when these limits are exceeded; forced Dfa reports a limit.
    RegularOptions regular;
    bool prepare_integer_row = true;
};
struct RunOptions {
    Backend backend = Backend::Auto;
    bool optimize = true;
    // Same-binary controls for cheap Boolean proofs and call-transparent scans.
    bool boundary_proofs = true;
    bool call_aware_scan = true;
    bool integer_length_certificate = true;
    std::size_t integer_length_max_steps = 256;
    MatchOptions match;
    AffineMatchOptions affine;
    OccurrenceMatchOptions occurrence;
};
struct RunStatistics {
    std::string backend;
    bool length_rejected = false;
    std::size_t boundary_checks = 0;
    bool boundary_rejected = false;
    std::size_t integer_length_checks = 0;
    std::size_t integer_length_steps = 0;
    std::size_t integer_length_exhaustions = 0;
    bool integer_length_rejected = false;
    RegularStatistics regular;
    CountStatistics count;
    AffineStatistics affine;
    MatchStatistics state;
    OccurrenceStatistics occurrence;
};
class Program {
public:
    explicit Program(std::string pattern, CompileOptions = {});
    [[nodiscard]] bool full_match(std::string_view subject, RunOptions = {},
                                  RunStatistics* = nullptr) const;
    // Greedy requests are invalid here because committing would discard endpoints.
    [[nodiscard]] std::vector<std::size_t> endpoints(std::string_view subject,
        std::size_t start = 0, RunOptions = {}, RunStatistics* = nullptr) const;
    // Auto/States for scalar captures; Occurrences for scoped or recursive constraints.
    [[nodiscard]] std::vector<Outcome> outcomes(std::string_view subject,
        RunOptions = {}, RunStatistics* = nullptr) const;
    // One valid full-match witness. Recursive grammars retain one finite proof;
    // other patterns retain state environments but materialize only one result.
    // Selection has no Perl/POSIX priority guarantee. Recursive all-outcome
    // enumeration is a separate history query requiring explicit Occurrences.
    [[nodiscard]] std::optional<Outcome> first_match(std::string_view subject,
        RunOptions = {}, RunStatistics* = nullptr) const;
    // One complete occurrence history. Auto/Occurrences only. For patterns that
    // otherwise use a specialized backend, this explicitly compiles an occurrence
    // evaluator on first use and reuses it across subsequent queries.
    [[nodiscard]] std::optional<TracedOutcome> first_trace(std::string_view subject,
        RunOptions = {}, RunStatistics* = nullptr) const;
    [[nodiscard]] const Ast& ast() const;
    [[nodiscard]] detail::SourcePortfolioSnapshot source_portfolio_snapshot() const;
    [[nodiscard]] std::string explain() const;
private:
    struct Impl;
    std::shared_ptr<const Impl> impl_;
};
}
