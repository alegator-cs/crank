#pragma once
#include "exact_program_statistics.hpp"
#include <atomic>
#include <memory>
#include <mutex>
namespace crank::detail {
// A common ownership/publication protocol, not a common instruction set.
// The source and certificates are immutable members of the enclosing Impl and
// outlive these caches. A negative lowering is cached just like a positive one.
template<class Program>
class ExactLoweringCache {
    mutable std::once_flag once_;
    mutable std::unique_ptr<const Program> program_;
    mutable std::atomic_bool complete_{false};
public:
    // Does not initiate construction. Negative certificates remain inspectable.
    const Program* cached() const {
        return complete_.load(std::memory_order_acquire) ? program_.get() : nullptr;
    }
    template<class... Args>
    const Program* get(const Args&... args) const {
        if (!complete_.load(std::memory_order_acquire)) {
            std::call_once(once_, [&] {
                try { program_ = std::make_unique<Program>(args...); }
                catch (const std::bad_alloc&) { /* The certificate interpreter remains available. */ }
                complete_.store(true, std::memory_order_release);
            });
        }
        return program_ && program_->ready() ? program_.get() : nullptr;
    }
    LoweringPreparationStatistics statistics() const {
        LoweringPreparationStatistics result;
        if (!complete_.load(std::memory_order_acquire)) return result;
        result.attempted = true;
        if (program_) {
            if constexpr (requires { Program::compiler_version; })
                result.compiler_version = Program::compiler_version;
            result.ready = program_->ready();
            result.compilation_steps = program_->compilation_steps();
            result.retained_bytes = program_->retained_bytes();
            result.code_bytes = program_->code_bytes();
            if constexpr (requires { program_->statistics().supported; })
                result.supported = program_->statistics().supported;
            if constexpr (requires { program_->statistics().limited; })
                result.limited = program_->statistics().limited;
            else if constexpr (requires { program_->limit_reached; })
                result.limited = program_->limit_reached;
            if constexpr (requires { program_->statistics().executable_memory_failed; })
                result.executable_memory_failed = program_->statistics().executable_memory_failed;
        } else result.limited = true;
        return result;
    }
};

} // namespace crank::detail
