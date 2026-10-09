#pragma once
#include "regex_ast.hpp"
#include <cstdint>

namespace crank {

// Necessary conditions on the length of one successful invocation. A false
// productive flag proves that no finite derivation exists; true may also mean
// that a runtime reference or count still has to be resolved. SIZE_MAX is a
// saturated lower bound, never a substitute for the productivity flag.
struct OccurrenceLength {
    bool productive = false;
    std::size_t minimum = std::numeric_limits<std::size_t>::max();
    // Modulus zero is the singleton {offset}; modulus one is unconstrained.
    std::size_t offset = 0;
    std::size_t modulus = 0;
    // Bit r admits lengths congruent to r modulo 64. This preserves unions
    // that the single congruence above cannot express. All bits means unknown;
    // the default also keeps four-field aggregate construction conservative.
    // This field is ignored for an unproductive summary.
    std::uint64_t residues = ~std::uint64_t{0};
    // Bit n admits the actual length n, for 0 <= n < 64. Unlike residues,
    // these bits never wrap: short unattainable lengths remain excluded even
    // when repetition fills the modular hull. All bits means unknown, so old
    // aggregate initializers remain conservative. Ignore when unproductive.
    std::uint64_t short_lengths = ~std::uint64_t{0};

    [[nodiscard]] bool accepts(std::size_t size) const;
};

// Necessary byte conditions for one invocation. The sets describe nonempty
// words only; nullable separately admits epsilon. Runtime-dependent values
// are overapproximated. Empty sets with nullable=false denote no derivation.
struct OccurrenceBoundary {
    bool nullable = false;
    std::bitset<256> first;
    std::bitset<256> last;
    bool operator==(const OccurrenceBoundary&) const = default;
};

// Ordered first/last byte pairs of words of length at least two, represented
// by bit ((17 * first_byte + second_byte) & 63). Collisions admit extra work;
// a missing bit proves impossibility. The last pair is in input order too.
struct OccurrencePairs {
    std::uint64_t first = 0;
    std::uint64_t last = 0;
    bool operator==(const OccurrencePairs&) const = default;
};

// Keep the byte conditions used together by request pruning in one record.
// The existing boundary and pair accessors remain available independently.
struct OccurrenceByteConditions {
    OccurrenceBoundary boundary;
    OccurrencePairs pairs;
};

// A deliberately narrow full-root certificate. One root source invocation has
// a fixed-width base and a single recursive spine adding the same width at
// every level; the root then repeats fixed-width bodies by that source height.
// Every match obeys size = offset + coefficient * height. The coefficient is
// positive, so a subject determines at most one possible height. Path IDs are
// exact queries certified to select this one root occurrence.
struct OccurrenceHeightEquation {
    std::size_t capture_id = no_id;
    std::size_t offset = 0;
    std::size_t coefficient = 0;
    std::vector<std::size_t> count_paths;

    [[nodiscard]] std::optional<std::size_t> height_for_length(std::size_t size) const;
    [[nodiscard]] bool accepts(std::size_t size) const;
};

// This analysis starts with empty languages and propagates finite derivations
// to a fixed point. Recursive calls therefore retain useful lower bounds and
// congruences, residue unions and short-length sets without mistaking a cycle for an
// epsilon production. Residues reach their own finite least fixed point in the
// same worklist; incomplete approximations are never used to reject matches.
// Runtime references and unresolved counts are deliberately overapproximated.
class OccurrenceAnalysis {
public:
    explicit OccurrenceAnalysis(const Ast&);
    [[nodiscard]] const OccurrenceLength& length(NodeId id) const;
    [[nodiscard]] const OccurrenceBoundary& boundary(NodeId id) const;
    [[nodiscard]] const OccurrencePairs& pairs(NodeId id) const;
    [[nodiscard]] const OccurrenceByteConditions& byte_conditions(NodeId id) const;
    // For Concat nodes, entries [0, children.size()] give the remaining minimum
    // at each child boundary. The final entry is zero; other node kinds have an
    // empty vector. Consult length(child).productive separately when needed.
    [[nodiscard]] const std::vector<std::size_t>& suffix_minimum(NodeId id) const;
    // The whole remaining concatenation, not an individual child's length.
    // Built after recursive node summaries converge. Boundary children.size()
    // is epsilon. This is only an exact-length rejection test when the caller
    // requires a particular end; the subject's remaining capacity is not one.
    [[nodiscard]] const OccurrenceLength& suffix_length(NodeId id, std::size_t boundary) const;
    [[nodiscard]] const std::optional<OccurrenceHeightEquation>& height_equation() const;

private:
    struct SuffixRange {
        // Earlier suffixes are appended in reverse boundary order.
        std::size_t base = 0;
        NodeId last_child = no_id;
    };
    std::vector<OccurrenceLength> lengths_;
    std::vector<OccurrenceByteConditions> byte_conditions_;
    std::vector<std::vector<std::size_t>> suffix_minima_;
    std::vector<SuffixRange> suffix_ranges_;
    std::vector<OccurrenceLength> suffix_lengths_;
    std::optional<OccurrenceHeightEquation> height_equation_;
};

} // namespace crank
