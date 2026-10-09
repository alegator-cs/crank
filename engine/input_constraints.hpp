#pragma once

#include "occurrence_analysis.hpp"
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace crank::detail {

// Input-dependent necessary conditions for pure Boolean recognition. Nothing
// here edits the compiled AST. A declined optional index means "unknown", not
// "no match". Ast and OccurrenceAnalysis must outlive this object and its Index.
class InputConstraints {
    struct Compiled;
public:
    struct Limits {
        std::size_t max_steps = 4'000'000;
        std::size_t max_occurrences = 1'000'000;
        std::size_t max_input_bytes = 64 * 1024 * 1024;
    };
    struct Stats {
        std::size_t bytes_scanned = 0;
        // Distinct fixed words selected for the attempted shared scan. A
        // declined scan may not have completed their occurrence lists.
        std::size_t literal_fragments = 0;
        std::size_t literal_hits = 0;
        std::size_t inactive_nodes = 0;
        std::size_t steps = 0;
        bool declined = false;
        std::string rejection_reason;
    };
    class Index {
    public:
        // False proves impossibility. True may still require structural work.
        [[nodiscard]] bool possible(NodeId, std::size_t begin, std::size_t end) const;
        // A complete fixed fragment, one-byte leaf, epsilon, or repetition of
        // an indexed fixed fragment can answer exactly; otherwise nullopt.
        [[nodiscard]] std::optional<bool> exact(NodeId, std::size_t begin,
                                                std::size_t end) const;
        // Starts are sorted and complete, including overlaps. A null pointer
        // means that this node has no completed fixed-fragment occurrence index.
        [[nodiscard]] const std::vector<std::size_t>* starts(NodeId) const;
        [[nodiscard]] std::optional<std::size_t> literal_size(NodeId) const;
        [[nodiscard]] bool root_possible() const;
        [[nodiscard]] const Stats& stats() const { return stats_; }

    private:
        friend class InputConstraints;
        std::shared_ptr<const Compiled> compiled_;
        std::string_view subject_;
        std::vector<std::vector<std::size_t>> starts_;
        // For each occurrence, farthest endpoint reached by immediately
        // consecutive occurrences of that same word. This is linear storage,
        // even for a* on an all-a input.
        std::vector<std::vector<std::size_t>> run_ends_;
        std::vector<unsigned char> live_;
        bool scanned_ = false;
        bool runs_complete_ = false;
        bool productivity_complete_ = false;
        bool rejected_ = false;
        Stats stats_;
    };

    explicit InputConstraints(const Ast&, const OccurrenceAnalysis&);
    [[nodiscard]] Index build(std::string_view subject, Limits) const;
    [[nodiscard]] Index build(std::string_view subject) const {
        return build(subject, Limits{});
    }

private:
    std::shared_ptr<const Compiled> compiled_;
};

} // namespace crank::detail
