#pragma once
#include "regional_predictive.hpp"
#include <array>
#include <utility>

namespace crank::detail {

class PredictivePayloadViewOwner;

// Experimental read view. The admitted record is distinct from its selected
// immutable table backing. Entry/root metadata and observer identity ALWAYS
// come from the requested record, never from a canonical sibling.
// The same immutable AST, completed facts and control arrays must outlive the
// source cache, this owner, and every view execution (as for the native cache).
class PredictivePayloadView {
    friend class PredictivePayloadViewOwner;
    const RegionalPredictiveCache* owner_ = nullptr;
    std::shared_ptr<const PredictiveRegion> record_, payload_;
    bool exact_ = false;
    RegionalConsumerContract consumer_;
    PredictivePayloadView(const RegionalPredictiveCache& owner,
                          std::shared_ptr<const PredictiveRegion> record,
                          bool exact, RegionalConsumerContract consumer)
        : owner_(&owner), record_(std::move(record)), payload_(record_),
          exact_(exact), consumer_(consumer) {}
public:
    PredictivePayloadView() = default;
    [[nodiscard]] explicit operator bool() const noexcept { return bool(record_); }
    [[nodiscard]] const auto& record() const noexcept { return record_; }
    [[nodiscard]] const auto& payload() const noexcept { return payload_; }
    [[nodiscard]] const auto* source_owner() const noexcept { return owner_; }
    [[nodiscard]] bool exact() const noexcept { return exact_; }
    // On an exact view, eof denotes the constructor's virtual exact boundary;
    // it does not authorize an unrestricted physical-EOF observer.
    [[nodiscard]] const auto& consumer() const noexcept { return consumer_; }
    [[nodiscard]] bool tables_shared() const noexcept {
        return record_ && record_.get() != payload_.get();
    }
    // Valid-view precondition for metadata/table access.
    [[nodiscard]] NodeId entry() const noexcept { return record_->entry; }
    [[nodiscard]] std::size_t root_child_count() const noexcept { return record_->root_child_count; }
    [[nodiscard]] const auto& targets() const noexcept { return payload_->targets; }
    [[nodiscard]] const auto& choices() const noexcept { return payload_->choices; }
    [[nodiscard]] const auto& choice_index() const noexcept { return payload_->choice_index; }
};

// Opt-in, post-admission read-sharing prototype. It never frees/moves duplicate
// buffers or skips a constructor. BOTH original objects and every legacy
// capacity/sizeof(PredictiveRegion) reservation remain in the real cache.
// Therefore this demonstrates shared reads, not physical memory reduction or
// an integrated faster native executor. No existing engine TU imports it.
// Pooling is restricted to this ONE borrowed cache/AST/fact owner. That source
// owner must outlive this facade and all executions of returned views.
class PredictivePayloadViewOwner {
    const RegionalPredictiveCache& cache_;
    const bool share_equal_tables_;
    // Probe-only phase seam. The normal optional stage allocates nothing;
    // throwing here tests optional-stage fallback, not a real allocation site.
    // Hook precondition: terminate promptly and never re-enter this facade;
    // it runs under the registry mutex. A failed hook is retried on a later
    // positive request. The 128 bound is storage/per-call comparisons, not a
    // lifetime cap on failed optional attempts.
    void (*const optional_stage_)(const PredictiveRegion&);
    struct Mapping {
        std::shared_ptr<const PredictiveRegion> record, payload;
    };
    std::array<Mapping, 128> mappings_{};
    std::size_t count_ = 0;
    std::mutex mutex_;

    static bool equal_tables(const PredictiveRegion& a, const PredictiveRegion& b) noexcept {
        if (a.entry != b.entry || a.root_child_count != b.root_child_count ||
            a.targets != b.targets || a.choice_index != b.choice_index ||
            a.choices.size() != b.choices.size()) return false;
        for (std::size_t i = 0; i < a.choices.size(); ++i) {
            if (a.choices[i].byte != b.choices[i].byte ||
                a.choices[i].empty != b.choices[i].empty) return false;
        }
        return true;
    }
public:
    explicit PredictivePayloadViewOwner(const RegionalPredictiveCache& cache,
                                       bool share_equal_tables = false,
                                       void (*optional_stage)(const PredictiveRegion&) = nullptr)
        : cache_(cache), share_equal_tables_(share_equal_tables), optional_stage_(optional_stage) {}
    PredictivePayloadViewOwner(const PredictivePayloadViewOwner&) = delete;
    PredictivePayloadViewOwner& operator=(const PredictivePayloadViewOwner&) = delete;

    [[nodiscard]] PredictivePayloadView certificate(NodeId node, bool exact = false) {
        // This is the only authority source. A failed/tombstoned request MUST
        // return before any mapping lookup, even if a sibling is available.
        auto admitted = cache_.certificate(node, exact);
        if (!admitted) return {};
        auto consumer = exact ? RegionalConsumerContract{{}, true} : *cache_.contract(node);
        PredictivePayloadView result(cache_, std::move(admitted), exact, consumer);
        if (!share_equal_tables_) return result;
        try {
            std::lock_guard lock(mutex_);
            for (std::size_t i = 0; i < count_; ++i) {
                if (mappings_[i].record.get() == result.record_.get()) {
                    result.payload_ = mappings_[i].payload;
                    return result;
                }
            }
            // Fixed capacity and bounded comparisons: no map/vector growth.
            // Exhaustion or any optional-stage failure leaves the independent
            // admitted record usable and changes no production statistics.
            if (count_ == mappings_.size()) return result;
            if (optional_stage_) optional_stage_(*result.record_);
            for (std::size_t i = 0; i < count_; ++i) {
                if (equal_tables(*result.record_, *mappings_[i].payload)) {
                    result.payload_ = mappings_[i].payload;
                    break;
                }
            }
            mappings_[count_++] = {result.record_, result.payload_};
        } catch (...) {
            result.payload_ = result.record_;
        }
        return result;
    }
};
} // namespace crank::detail
