#pragma once

#include "atomic_exclusions.hpp"
#include "occurrence_analysis.hpp"
#include <atomic>
#include <cstdint>
#include <memory>
#include <string_view>

namespace crank::detail {

// Helper-owned speculative work. Compilation borrows immutable AST/analysis;
// their owner must outlive this object and all scans. Each scan keeps mutable
// state private and publishes only independently sound, permanent exclusions.
class PrivateCuller {
    struct Compiled;
public:
    struct Limits {
        std::size_t max_steps = 4'000'000;
        std::size_t max_input_bytes = 64 * 1024 * 1024;
    };
    enum class RejectionReason { None, Alphabet, Adjacency, Productivity };
    struct Stats {
        std::size_t bytes_scanned = 0;
        std::size_t steps = 0;
        std::size_t literal_fragments = 0;
        std::size_t literals_found = 0;
        std::size_t inactive_nodes = 0;
        // steady_clock epoch, measured when a stop is first observed, before
        // local buffers are destroyed. Zero means cancellation was not observed.
        std::uint64_t cancellation_observed_ns = 0;
        bool declined = false;
        bool cancelled = false;
        bool completed = false;
        RejectionReason rejection_reason = RejectionReason::None;
    };

    explicit PrivateCuller(const Ast&, const OccurrenceAnalysis&);
    [[nodiscard]] bool available() const noexcept;
    // Compile-time capability tests. False means this proof family cannot
    // add information for a nonempty subject; true promises no actual gain.
    [[nodiscard]] bool can_reject_bytes() const noexcept;
    [[nodiscard]] bool can_exclude_nodes() const noexcept;

    // One full presence scan, then private input-relative productivity closure.
    // Polls stop/root-rejection within 64 charged operations, including output
    // links and closure propagation. No occurrence vectors: a fixed word is
    // reported at most once. No node absence is published before both phases
    // complete. Cancel/limit leaves
    // unfinished knowledge unknown. Stats is reset at the start of each call.
    void scan_presence(std::string_view, const std::atomic_bool& cancel,
                       AtomicExclusions&, Limits, Stats&) const;

    // A separate helper can inspect a disjoint byte shard. Checks the pair
    // whose right byte is at begin, so partition boundaries cannot lose pairs.
    // It may publish a whole-query rejection but never per-node absence.
    void scan_range(std::string_view, std::size_t begin, std::size_t end,
                    const std::atomic_bool& cancel, AtomicExclusions&,
                    Limits, Stats&) const;

private:
    std::shared_ptr<const Compiled> compiled_;
};

} // namespace crank::detail
