#pragma once

#include "regex_ast.hpp"
#include <limits>
#include <new>
#include <vector>

namespace crank::detail {

// Append-only lookup over IDs in an external arena. Keys stay in that arena;
// only IDs move when the table grows. Insert each non-no_id record once.
class IdIndex {
    std::vector<std::size_t> slots_;
    std::size_t size_ = 0;

    static std::size_t mixed(std::size_t value) noexcept {
        static_assert(std::numeric_limits<std::size_t>::digits == 32 ||
                      std::numeric_limits<std::size_t>::digits == 64);
        if constexpr (std::numeric_limits<std::size_t>::digits == 64) {
            value ^= value >> 30;
            value *= static_cast<std::size_t>(0xbf58476d1ce4e5b9ULL);
            value ^= value >> 27;
            value *= static_cast<std::size_t>(0x94d049bb133111ebULL);
            value ^= value >> 31;
        } else {
            value ^= value >> 16;
            value *= static_cast<std::size_t>(0x7feb352dU);
            value ^= value >> 15;
            value *= static_cast<std::size_t>(0x846ca68bU);
            value ^= value >> 16;
        }
        return value;
    }

    template<class HashOf>
    void grow(const HashOf& hash_of) {
        if (slots_.size() > slots_.max_size() / 2) throw std::bad_alloc();
        const auto capacity = slots_.empty() ? std::size_t{16} : slots_.size() * 2;
        if (capacity > slots_.max_size()) throw std::bad_alloc();
        std::vector<std::size_t> replacement(capacity, no_id);
        const auto mask = capacity - 1;
        for (const auto id : slots_) {
            if (id == no_id) continue;
            auto slot = mixed(hash_of(id)) & mask;
            while (replacement[slot] != no_id) slot = (slot + 1) & mask;
            replacement[slot] = id;
        }
        slots_.swap(replacement);
    }

public:
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

    template<class Equal>
    [[nodiscard]] std::size_t find(std::size_t hash, const Equal& equal) const {
        if (slots_.empty()) return no_id;
        const auto mask = slots_.size() - 1;
        auto slot = mixed(hash) & mask;
        // A power-of-two table at most half full always has an empty stop slot.
        while (slots_[slot] != no_id) {
            if (equal(slots_[slot])) return slots_[slot];
            slot = (slot + 1) & mask;
        }
        return no_id;
    }

    template<class HashOf>
    void insert(std::size_t hash, std::size_t id, const HashOf& hash_of) {
        if (slots_.empty() || size_ == slots_.size() / 2) grow(hash_of);
        const auto mask = slots_.size() - 1;
        auto slot = mixed(hash) & mask;
        while (slots_[slot] != no_id) slot = (slot + 1) & mask;
        slots_[slot] = id;
        ++size_;
    }
};

} // namespace crank::detail
