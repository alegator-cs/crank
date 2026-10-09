#pragma once
#include "regex_ast.hpp"

#include <atomic>
#include <cstdint>
#include <memory>

namespace crank::detail {

// One table belongs to one immutable AST and one immutable query subject.
// A node bit certifies that its language matches NO span of that subject,
// including empty spans. Failure at one requested endpoint is insufficient.
// A reader seeing an old zero merely performs unnecessary native work.
//
// The bit is the entire certificate: it publishes no accompanying payload.
// Relaxed atomic operations suffice. Words never move, resize, or clear while
// queried; the owner retains the table until every publisher has stopped.
class AtomicExclusions {
public:
    explicit AtomicExclusions(std::size_t node_count)
        : node_count_(node_count),
          words_(std::make_unique<std::atomic<std::uint64_t>[]>(
              node_count / 64 + (node_count % 64 != 0))) {
        for (std::size_t i = 0; i < node_count / 64 + (node_count % 64 != 0); ++i)
            words_[i].store(0, std::memory_order_relaxed);
    }

    [[nodiscard]] std::size_t node_count() const noexcept { return node_count_; }

    // node must be a valid ID in the table's immutable AST.
    [[nodiscard]] bool test(NodeId node) const noexcept {
        return (words_[node / 64].load(std::memory_order_relaxed) &
                (std::uint64_t{1} << (node % 64))) != 0;
    }

    // Multiple publishers may exclude nodes in the same word. fetch_or keeps
    // both updates, unlike a load followed by a plain store. Returns true only
    // for the first publication of this node's certificate.
    bool exclude(NodeId node) noexcept {
        const auto bit = std::uint64_t{1} << (node % 64);
        return (words_[node / 64].fetch_or(bit, std::memory_order_relaxed) & bit) == 0;
    }

    // Batch up to 64 independent certificates in one publication. word_index
    // must address an allocated word; mask may contain only valid node bits
    // (including in the final partial word). Returns the newly set bits.
    [[nodiscard]] std::uint64_t exclude_word(std::size_t word_index,
                                            std::uint64_t mask) noexcept {
        return mask & ~words_[word_index].fetch_or(mask, std::memory_order_relaxed);
    }

    // Full-input failure is weaker than global absence of the root language:
    // recursive calls to the root may still succeed on proper subspans.
    void reject_root() noexcept { root_rejected_.store(true, std::memory_order_relaxed); }
    [[nodiscard]] bool root_rejected() const noexcept {
        return root_rejected_.load(std::memory_order_relaxed);
    }

private:
    const std::size_t node_count_;
    const std::unique_ptr<std::atomic<std::uint64_t>[]> words_;
    // This independently polled flag does not share a cache line with the
    // object's read-mostly metadata. Node words occupy their own allocation.
    alignas(64) std::atomic_bool root_rejected_{false};
};

// Foreground-owned counters; helpers never write them.
struct NativeAssistanceStatistics {
    std::size_t checks = 0;
    std::size_t culled_requests = 0;
    std::size_t culled_tasks = 0;
    std::size_t root_rejections = 0;
};

} // namespace crank::detail
