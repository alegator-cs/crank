#pragma once
#include "regular_matcher.hpp"
#include "external_regular.hpp"

namespace crank {

// A reusable Boolean regular-language validator. Proven Cartesian byte-set
// sequences and their repetitions use direct scans; all other fragments retain
// the DFA. Normal construction still compiles the DFA, preserving compile caps
// and the established NFA/DFA statistics. Selected structural subtrees are
// validated even inside zero repetitions; cycles, malformed nodes and depth
// above 2048 are rejected. Unrelated AST nodes are not inspected.
class RegularFragment {
public:
    RegularFragment(const Ast&, NodeId root, RegularOptions = {});
    RegularFragment(const Ast&, const std::vector<NodeId>& roots, RegularOptions = {});
    [[nodiscard]] bool full_match(std::string_view, RegularStatistics* = nullptr,
        ExternalRegularPolicy = ExternalRegularPolicy::Auto,
        std::uint32_t external_match_limit = external_default_match_limit) const;
    [[nodiscard]] RegularStatistics compiled_statistics() const;
    [[nodiscard]] bool direct() const;
    // Bounded, negative-only root certificate for complete Boolean spans.
    // Do not apply suffix conditions to unrestricted endpoint/prefix queries.
    [[nodiscard]] bool has_boundary_certificate() const;
    [[nodiscard]] bool boundary_impossible(std::string_view) const;
    // Direct scans retain priority even under Pcre2. Auto uses the optional
    // adapter only above external_auto_min_bytes. Runtime errors always fall
    // back to the retained exact DFA; this reports eligibility, not success.
    [[nodiscard]] bool external_available() const;
    [[nodiscard]] bool external_would_run(std::size_t bytes,
        ExternalRegularPolicy = ExternalRegularPolicy::Auto) const;
    // AST constructors retain their compiled DFA; copies share immutable data.
    // The byte_set factory has no DFA and this accessor throws logic_error.
    [[nodiscard]] const RegularProgram& regular_program() const;

    // A certified one-byte class repeated within this fragment's length bounds.
    // Prefix validation deliberately ignores those bounds: callers must enforce
    // them separately. It returns the first invalid byte, or subject.size().
    [[nodiscard]] bool byte_class_repetition() const;
    [[nodiscard]] std::size_t byte_class_prefix(std::string_view) const;

    // Validate exactly `copies` adjacent slices of `width` bytes. Return the
    // number of accepted slices preceding the first failure, or copies. The
    // caller must supply an exact-size span; invalid dimensions throw.
    [[nodiscard]] std::size_t matching_copies(std::string_view, std::size_t width,
                                             std::size_t copies) const;

    // CountPlan historically validates one-byte classes without compiling an
    // automaton; retain that exemption from regular compilation limits.
    [[nodiscard]] static RegularFragment byte_set(const std::bitset<256>&);

private:
    struct Impl;
    std::shared_ptr<const Impl> impl_;
    explicit RegularFragment(std::shared_ptr<const Impl>);
};

} // namespace crank
