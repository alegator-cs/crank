#pragma once
#include "lowered_deterministic_program.hpp"
#include "lowered_predictive_program.hpp"
#include "lowered_spine_program.hpp"
#include "native_slr_program.hpp"
#include "exact_lowering_cache.hpp"
#include <atomic>
#include <memory>
#include <mutex>

namespace crank::detail {

// Compilation is requested only after the exact observation contract and the
// corresponding source certificate have been admitted. No per-subject state
// is stored here, and copies of AstMachineProgram share the owning Impl.
struct CompiledExactPrograms {
    ExactLoweringCache<LoweredDeterministicProgram> slr;
    ExactLoweringCache<LoweredPredictiveProgram> predictive, predictive_rim;
    ExactLoweringCache<LoweredBoundedWordProgram> bounded_word;
    ExactLoweringCache<LoweredSpineProgram> spine;
    ExactLoweringCache<NativeSlrProgram> native_slr;
    CompiledProgramPreparationStatistics statistics() const {
        CompiledProgramPreparationStatistics result;
        result.slr = slr.statistics(); result.predictive = predictive.statistics();
        result.predictive_rim = predictive_rim.statistics();
        result.bounded_word = bounded_word.statistics(); result.spine = spine.statistics();
        result.native_slr = native_slr.statistics();
        return result;
    }

};

} // namespace crank::detail
