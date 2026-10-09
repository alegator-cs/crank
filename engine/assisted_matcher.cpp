#include "assisted_matcher.hpp"
#include "private_culler.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace crank {
namespace {
std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
template<class T> void record_max(std::atomic<T>& target, T value) {
    auto old = target.load(std::memory_order_relaxed);
    while (old < value && !target.compare_exchange_weak(old, value, std::memory_order_relaxed)) {}
}

bool root_possible(const OccurrenceAnalysis& analysis, NodeId node, std::string_view input) {
    if (!analysis.length(node).accepts(input.size())) return false;
    const auto& facts = analysis.byte_conditions(node);
    if (input.empty()) return facts.boundary.nullable;
    if (!facts.boundary.first[static_cast<unsigned char>(input.front())] ||
        !facts.boundary.last[static_cast<unsigned char>(input.back())]) return false;
    if (input.size() < 2) return true;
    const auto bit = [&](std::size_t at) {
        return std::uint64_t{1} << ((17U * static_cast<unsigned char>(input[at]) +
            static_cast<unsigned char>(input[at + 1])) & 63U);
    };
    return (facts.pairs.first & bit(0)) && (facts.pairs.last & bit(input.size() - 2));
}

AssistanceOptions checked(AssistanceOptions options) {
    if (!options.workers || options.workers > 64)
        throw std::invalid_argument("assisted workers must be between 1 and 64");
    return options;
}

// Jobs retain compilation independently of the pool and input independently of
// the caller. Returning from full_match cannot invalidate a helper's pointers.
struct Compilation {
    detail::AstMachineProgram core;
    std::shared_ptr<const OccurrenceAnalysis> analysis;
    std::unique_ptr<detail::PrivateCuller> culler;
    Compilation(const Ast& ast, bool helpers) : core(ast), analysis(core.shared_analysis()) {
        if (helpers) culler = std::make_unique<detail::PrivateCuller>(core.ast(), *analysis);
    }
};

struct Job {
    std::shared_ptr<const Compilation> compilation;
    std::shared_ptr<const std::string> input;
    detail::AtomicExclusions exclusions;
    detail::PrivateCuller::Limits limits;
    std::size_t lanes = 0;
    std::atomic<std::size_t> finished{0};
    alignas(64) std::atomic_bool stop{false};
    std::atomic<std::uint64_t> stop_requested_ns{0};

    Job(std::shared_ptr<const Compilation> compiled,
        std::shared_ptr<const std::string> text, std::size_t helpers, AssistanceOptions options)
        : compilation(std::move(compiled)), input(std::move(text)),
          exclusions(compilation->core.ast().nodes.size()), lanes(helpers) {
        limits.max_steps = options.max_helper_steps / helpers;
        limits.max_input_bytes = options.max_input_bytes;
    }
    void retire() {
        // Timestamp is atomic too: relaxed stop publication conveys no mutable
        // payload. A late observation can only affect diagnostic precision.
        stop_requested_ns.store(now_ns(), std::memory_order_relaxed);
        stop.store(true, std::memory_order_relaxed);
    }
};

struct PoolCounters {
    std::atomic<std::size_t> queries_admitted{0}, queries_busy{0}, tasks_submitted{0},
        queries_policy_skipped{0}, queries_no_useful_proof{0},
        tasks_finished{0}, tasks_cancelled{0}, tasks_declined{0}, helper_steps{0},
        helper_bytes{0}, nodes_excluded{0}, root_rejections{0}, active_helpers{0};
    std::atomic<std::uint64_t> max_stop_observation_ns{0}, max_cleanup_ns{0};
};
}

struct AssistedProgram::Impl {
    const AssistanceOptions options;
    std::shared_ptr<const Compilation> compilation;
    struct Slot {
        std::shared_ptr<Job> pending;
        std::size_t lane = 0;
        bool busy = false;
    };
    mutable std::mutex mutex;
    mutable std::condition_variable ready, idle;
    std::vector<Slot> slots;
    std::vector<std::thread> threads;
    bool stopping = false;
    PoolCounters counters;

    // Each size class owns one packed advisory word: high bits count up to six
    // consecutive unsuccessful probes; low six bits count queries to skip.
    // A stale/racing update changes only optional work, never recognition.
    // No query performs a retry loop or waits for policy state.
    struct alignas(64) AdmissionBucket { std::atomic<unsigned> state{0}; };
    std::array<AdmissionBucket, std::numeric_limits<std::size_t>::digits + 1> admission;

    bool policy_admits(std::size_t size, AssistedStatistics& stats) {
        if (options.policy == AssistancePolicy::Always) return true;
        const auto& culler = *compilation->culler;
        if (size && !culler.can_reject_bytes() && !culler.can_exclude_nodes()) {
            stats.decision = AssistanceDecision::NoUsefulProof;
            counters.queries_no_useful_proof.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (options.policy == AssistancePolicy::Progress) return true;
        auto& state = admission[std::bit_width(size)].state;
        auto observed = state.load(std::memory_order_relaxed);
        if (!(observed & 63U)) return true;
        // A failed CAS means another caller made progress. Still decline this
        // optional launch; no recognition result depends on who got the slot.
        state.compare_exchange_strong(observed, observed - 1, std::memory_order_relaxed);
        stats.decision = AssistanceDecision::PolicyBackoff;
        counters.queries_policy_skipped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    void policy_feedback(std::size_t size, const AssistedStatistics& stats) {
        if (options.policy != AssistancePolicy::Selective) return;
        const bool useful = stats.consumption.culled_requests ||
            stats.consumption.culled_tasks || stats.consumption.root_rejections;
        auto& state = admission[std::bit_width(size)].state;
        auto observed = state.load(std::memory_order_relaxed);
        const auto misses = useful ? 0U : std::min(6U, (observed >> 6) + 1);
        const auto updated = (misses << 6) | ((1U << misses) - 1);
        // Concurrent feedback may be dropped. This deliberately avoids a
        // bookkeeping synchronization loop on the foreground path.
        state.compare_exchange_strong(observed, updated, std::memory_order_relaxed);
    }

    Impl(const Ast& ast, AssistanceOptions requested)
        : options(checked(requested)),
          compilation(std::make_shared<Compilation>(ast, options.workers > 1)) {
        if (!compilation->culler || !compilation->culler->available()) return;
        if (options.policy != AssistancePolicy::Always && options.min_input_bytes &&
            !compilation->culler->can_reject_bytes() &&
            !compilation->culler->can_exclude_nodes()) return;
        slots.resize(options.workers - 1);
        threads.reserve(slots.size());
        try {
            for (std::size_t index = 0; index < slots.size(); ++index)
                threads.emplace_back([this, index] { work(index); });
        } catch (...) {
            { std::lock_guard lock(mutex); stopping = true; }
            ready.notify_all();
            for (auto& thread : threads) thread.join();
            throw;
        }
    }

    ~Impl() {
        // Destruction is a reclamation barrier. Query return itself never takes
        // this path; retained program handles keep persistent workers available.
        { std::lock_guard lock(mutex); stopping = true; }
        ready.notify_all();
        for (auto& thread : threads) thread.join();
    }

    void work(std::size_t index) noexcept {
        for (;;) {
            std::shared_ptr<Job> job;
            std::size_t lane;
            {
                std::unique_lock lock(mutex);
                ready.wait(lock, [&] { return stopping || bool(slots[index].pending); });
                if (!slots[index].pending) return;
                job = std::move(slots[index].pending);
                lane = slots[index].lane;
            }
            detail::PrivateCuller::Stats stats;
            try {
                if (!lane) {
                    job->compilation->culler->scan_presence(*job->input, job->stop,
                        job->exclusions, job->limits, stats);
                } else {
                    // Lane zero covers the whole input. Other lanes scan later
                    // partitions, so a late forbidden pair can beat that scan.
                    const auto size = job->input->size();
                    const auto boundary = [&](std::size_t part) {
                        return (size / job->lanes) * part +
                            std::min(part, size % job->lanes);
                    };
                    job->compilation->culler->scan_range(*job->input, boundary(lane),
                        boundary(lane + 1), job->stop, job->exclusions, job->limits, stats);
                }
            } catch (...) {
                // Optional acceleration can fail without consuming native
                // budgets or changing the authoritative membership result.
                stats.declined = true;
            }
            const auto end = now_ns();
            const auto requested = job->stop_requested_ns.load(std::memory_order_relaxed);
            if (stats.cancelled && stats.cancellation_observed_ns) {
                if (requested && stats.cancellation_observed_ns >= requested)
                    record_max(counters.max_stop_observation_ns,
                        stats.cancellation_observed_ns - requested);
                record_max(counters.max_cleanup_ns, end - stats.cancellation_observed_ns);
            }
            counters.helper_steps.fetch_add(stats.steps, std::memory_order_relaxed);
            counters.helper_bytes.fetch_add(stats.bytes_scanned, std::memory_order_relaxed);
            counters.nodes_excluded.fetch_add(stats.inactive_nodes, std::memory_order_relaxed);
            if (stats.cancelled) counters.tasks_cancelled.fetch_add(1, std::memory_order_relaxed);
            if (stats.declined) counters.tasks_declined.fetch_add(1, std::memory_order_relaxed);
            if (stats.rejection_reason != detail::PrivateCuller::RejectionReason::None)
                counters.root_rejections.fetch_add(1, std::memory_order_relaxed);
            job->finished.fetch_add(1, std::memory_order_release);
            // Potentially expensive private reclamation belongs to this worker.
            job.reset();
            {
                std::lock_guard lock(mutex);
                slots[index].busy = false;
                counters.tasks_finished.fetch_add(1, std::memory_order_relaxed);
                if (counters.active_helpers.fetch_sub(1, std::memory_order_relaxed) == 1)
                    idle.notify_all();
            }
        }
    }

    std::shared_ptr<Job> submit(std::string_view input,
        std::shared_ptr<const std::string> owner, AssistedStatistics& stats) {
        // The foreground attempts admission once. It never waits for a busy
        // mutex or a busy helper, and there is no speculative work queue.
        std::unique_lock lock(mutex, std::try_to_lock);
        if (!lock) {
            stats.decision = AssistanceDecision::Busy;
            counters.queries_busy.fetch_add(1, std::memory_order_relaxed);
            return {};
        }
        std::size_t available = 0;
        for (const auto& slot : slots) available += !slot.busy;
        // When every byte/pair is allowed, extra byte-only lanes can publish
        // no proof. The single presence lane can still prune absent branches.
        if (options.policy != AssistancePolicy::Always &&
            !compilation->culler->can_reject_bytes()) available = std::min(available, std::size_t{1});
        available = std::min(available, options.max_helper_steps);
        if (!available) {
            stats.decision = AssistanceDecision::Busy;
            counters.queries_busy.fetch_add(1, std::memory_order_relaxed);
            return {};
        }
        std::shared_ptr<Job> job;
        try {
            if (!owner) {
                owner = std::make_shared<const std::string>(input);
                stats.copied_input_bytes = input.size();
            }
            job = std::make_shared<Job>(compilation, std::move(owner), available, options);
        } catch (const std::bad_alloc&) {
            stats.decision = AssistanceDecision::AllocationDeclined;
            return {};
        }
        std::size_t lane = 0;
        for (auto& slot : slots) {
            if (slot.busy) continue;
            slot.busy = true;
            slot.pending = job;
            slot.lane = lane++;
            if (lane == available) break;
        }
        counters.queries_admitted.fetch_add(1, std::memory_order_relaxed);
        counters.tasks_submitted.fetch_add(available, std::memory_order_relaxed);
        counters.active_helpers.fetch_add(available, std::memory_order_relaxed);
        stats.decision = AssistanceDecision::Admitted;
        stats.helpers_admitted = available;
        lock.unlock();
        ready.notify_all();
        return job;
    }

    bool full_match(std::string_view input, std::shared_ptr<const std::string> owner,
                    OccurrenceMatchOptions native_options, AssistedStatistics* statistics) {
        AssistedStatistics local;
        auto& stats = statistics ? *statistics : local;
        stats = {};
        const auto native = [&] {
            return compilation->core.full_match(input, native_options,
                statistics ? &stats.matching : nullptr);
        };
        if (options.workers == 1) return native();
        if (input.size() < options.min_input_bytes) {
            stats.decision = AssistanceDecision::SmallInput; return native();
        }
        if (input.size() > options.max_input_bytes) {
            stats.decision = AssistanceDecision::LargeInput; return native();
        }
        if (!options.max_helper_steps) {
            stats.decision = AssistanceDecision::NoBudget; return native();
        }
        if (!compilation->culler || !compilation->culler->available()) {
            stats.decision = AssistanceDecision::Unavailable; return native();
        }
        if (native_options.prune &&
            !root_possible(*compilation->analysis, compilation->core.ast().root, input)) {
            stats.decision = AssistanceDecision::RootRejected;
            ++stats.matching.pruned;
            return false;
        }
        if (!policy_admits(input.size(), stats)) return native();
        std::shared_ptr<Job> job;
        const bool deferred = options.policy == AssistancePolicy::Progress;
        if (deferred) stats.decision = AssistanceDecision::BelowWorkThreshold;
        else {
            // Retain the owned input through fallback too: unsuccessful
            // admission must not destroy the sole owner behind the native view.
            job = submit(input, owner, stats);
            if (!job) return native();
        }
        struct Launch {
            Impl* pool;
            std::string_view input;
            const std::shared_ptr<const std::string>* owner;
            AssistedStatistics* statistics;
            std::shared_ptr<Job>* job;
        } launch{this, input, &owner, &stats, &job};
        const detail::DeferredAssistance deferred_assistance{
            options.min_native_steps, &launch,
            [](void* context, std::size_t work_steps) noexcept -> const detail::AtomicExclusions* {
                auto& pending = *static_cast<Launch*>(context);
                pending.statistics->native_steps_before_admission = work_steps;
                try {
                    // The borrowed input remains valid on this foreground
                    // stack. Only an actual admission copies or retains it.
                    *pending.job = pending.pool->submit(pending.input, *pending.owner,
                                                        *pending.statistics);
                } catch (...) {
                    // Optional infrastructure failure never aborts native work.
                    pending.statistics->decision = AssistanceDecision::AllocationDeclined;
                }
                return *pending.job ? &(*pending.job)->exclusions : nullptr;
            }};
        const auto finish = [&] {
            if (!job) return;
            job->retire();
            stats.helpers_finished_at_return = job->finished.load(std::memory_order_acquire);
            stats.root_rejected_at_return = job->exclusions.root_rejected();
        };
        try {
            const auto answer = deferred
                ? compilation->core.full_match_deferred(input, native_options,
                    deferred_assistance, statistics ? &stats.matching : nullptr, &stats.consumption)
                : compilation->core.full_match_assisted(input, native_options,
                    job->exclusions, statistics ? &stats.matching : nullptr, &stats.consumption);
            finish();
            policy_feedback(input.size(), stats);
            return answer;
        } catch (const ResourceLimit&) {
            finish();
            // Consume a proof already available at this point, without waiting
            // for an unfinished helper to potentially rescue an exhausted run.
            if (stats.root_rejected_at_return) {
                stats.native_limit_rescued = true;
                ++stats.consumption.root_rejections;
                policy_feedback(input.size(), stats);
                return false;
            }
            policy_feedback(input.size(), stats);
            throw;
        } catch (...) {
            finish();
            throw;
        }
    }

    AssistancePoolStatistics snapshot() const {
        AssistancePoolStatistics result;
#define READ(name) result.name = counters.name.load(std::memory_order_relaxed)
        READ(queries_admitted); READ(queries_busy); READ(tasks_submitted);
        READ(queries_policy_skipped); READ(queries_no_useful_proof);
        READ(tasks_finished); READ(tasks_cancelled); READ(tasks_declined);
        READ(helper_steps); READ(helper_bytes); READ(nodes_excluded);
        READ(root_rejections); READ(active_helpers);
        READ(max_stop_observation_ns); READ(max_cleanup_ns);
#undef READ
        return result;
    }
};

AssistedProgram::AssistedProgram(const Ast& ast, AssistanceOptions options)
    : impl_(std::make_shared<Impl>(ast, options)) {}
bool AssistedProgram::eligible(const Ast& ast) { return detail::AstMachineProgram::eligible(ast); }
bool AssistedProgram::full_match(std::string_view subject, OccurrenceMatchOptions options,
                                AssistedStatistics* statistics) const {
    return impl_->full_match(subject, {}, options, statistics);
}
bool AssistedProgram::full_match(std::shared_ptr<const std::string> subject,
                                OccurrenceMatchOptions options, AssistedStatistics* statistics) const {
    if (!subject) throw std::invalid_argument("owned assisted input must not be null");
    const auto view = std::string_view(*subject);
    return impl_->full_match(view, std::move(subject), options, statistics);
}
AssistancePoolStatistics AssistedProgram::pool_statistics() const { return impl_->snapshot(); }
void AssistedProgram::wait_idle() const {
    std::unique_lock lock(impl_->mutex);
    impl_->idle.wait(lock, [&] { return !impl_->counters.active_helpers.load(std::memory_order_relaxed); });
}

} // namespace crank
