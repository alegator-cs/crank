#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace crank::experimental {

// Counts work within one match call; all counters are reset on entry.
struct Statistics {
    std::size_t node_evaluations = 0;
    std::size_t memo_hits = 0;
    std::size_t endpoint_insertions = 0; // Total endpoints stored in memo entries.
    std::size_t repetition_rounds = 0;
};

// A deliberately separate, byte-oriented Boolean regex prototype.
// Construction validates the supported syntax and throws std::invalid_argument
// for unsupported or malformed syntax. Groups do not expose captures.
// The AST is immutable; each call owns its subject-specific memo table, so
// separate match calls on the same Program may run concurrently.
class Program {
public:
    explicit Program(std::string pattern);

    [[nodiscard]] bool full_match(std::string_view subject,
                                  Statistics* statistics = nullptr) const;

    // All exclusive end offsets for matches beginning exactly at start,
    // in increasing order. Throws std::out_of_range if start > subject.size().
    [[nodiscard]] std::vector<std::size_t>
    endpoints(std::string_view subject, std::size_t start = 0,
              Statistics* statistics = nullptr) const;

private:
    struct Impl;
    std::shared_ptr<const Impl> impl_;
};

} // namespace crank::experimental
