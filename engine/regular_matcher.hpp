#pragma once
#include "regex_ast.hpp"
#include <memory>

namespace crank {
struct RegularOptions {
    std::size_t max_nfa_states = 4096;
    std::size_t max_dfa_states = 512;
    // Covers transparent capture/call chains that need no additional NFA state.
    std::size_t max_expansion_depth = 2048;
    // Extra structural analysis may decline without losing the retained DFA.
    std::size_t max_fragment_analysis_steps = 65536;
    // Optional composition of two exact DFA transitions. Original one-byte
    // tables remain available for endpoint/step APIs and construction decline.
    bool paired_transitions = true;
    std::size_t max_paired_table_bytes = 65536;
};
struct RegularStatistics {
    std::size_t nfa_states = 0;
    std::size_t dfa_states = 0;
    std::size_t workers = 1;
    // Logical transition coverage, retained even when a rejecting sink lets
    // execution skip the remainder of a subject or chunk.
    std::size_t transition_steps = 0;
    std::size_t fragment_analysis_steps = 0;
    std::size_t dfa_table_bytes = 0;
    std::size_t paired_table_bytes = 0;
    std::size_t paired_analysis_steps = 0;
};
// Capture nodes are transparent for Boolean membership and endpoints. Acyclic
// pure call graphs are regular: each call site receives its own NFA continuation,
// without copying or expanding the AST. Runtime references/counts and reachable
// call cycles are rejected; explicit compilation limits never change answers.
class RegularProgram {
public:
    explicit RegularProgram(const Ast&, RegularOptions options = {});
    // Compile an exact pure sublanguage without changing whole-pattern (?R)
    // targets. This is used by native requests entering a regular region.
    RegularProgram(const Ast&, NodeId entry, RegularOptions options);
    [[nodiscard]] static bool eligible(const Ast&, NodeId entry);
    // Opaque immutable transition offsets; only states produced by this program
    // may be supplied. Native execution advances one byte per ordered frontier.
    [[nodiscard]] std::size_t transition(std::size_t state, unsigned char byte) const;
    [[nodiscard]] bool accepting(std::size_t state) const;
    [[nodiscard]] bool dead(std::size_t state) const;
    [[nodiscard]] RegularStatistics construction_statistics() const;
    [[nodiscard]] std::size_t retained_bytes() const; // owned payload, excluding allocator bookkeeping
    // Inspect executed nodes and call targets, not cached Ast convenience flags.
    // DEFINE bodies are reached only through calls; unrelated definitions do not
    // affect eligibility. This is a sufficient certificate, not a decision
    // procedure for whether every recursive grammar denotes a regular language.
    [[nodiscard]] static bool eligible(const Ast&);
    [[nodiscard]] bool full_match(std::string_view subject, RegularStatistics* = nullptr) const;
    // Number of consecutive independently accepted width-byte bodies, stopping
    // before the first failure. The dimensions must cover the whole subject;
    // zero-width copies require an empty subject and test empty membership.
    [[nodiscard]] std::size_t matching_copies(std::string_view subject,
        std::size_t width, std::size_t copies) const;
    [[nodiscard]] std::vector<std::size_t> endpoints(std::string_view subject,
        std::size_t start = 0, RegularStatistics* = nullptr) const;
private:
    struct Impl;
    std::shared_ptr<const Impl> impl_;
};
}
