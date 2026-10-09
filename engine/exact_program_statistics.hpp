#pragma once
#include <cstddef>
#include <array>
#include <cstdint>
#include <limits>

namespace crank::detail {
struct ExactProgramPreparationStatistics {
    bool attempted = false, certified = false, limited = false;
    std::size_t analysis_steps = 0, productions = 0, states = 0, items = 0;
    std::size_t terminal_classes = 0, retained_bytes = 0, conflicts = 0;
    bool alias_normalization_attempted = false;
    std::size_t normalized_alias_calls = 0, original_conflicts = 0;
    std::size_t conflict_state = std::numeric_limits<std::size_t>::max(), conflict_token = std::numeric_limits<std::size_t>::max();
    std::int32_t conflict_existing_action = 0, conflict_new_action = 0;
};

struct BoundedWordPreparationStatistics {
    bool attempted = false, certified = false, limited = false;
    std::size_t analysis_steps = 0, refined_bytes = 0, decision_rows = 0, retained_bytes = 0;
};

struct NormalizedProgramPreparationStatistics {
    bool attempted = false, certified = false, limited = false, changed = false;
    bool nullable_complete = false;
    std::size_t normalization_steps = 0, nullable_steps = 0;
    std::size_t repetition_rewrites = 0, redirected_calls = 0, cloned_nodes = 0;
    std::size_t source_nodes = 0, normalized_nodes = 0, retained_bytes = 0;
    ExactProgramPreparationStatistics slr;
};

// Snapshot of a lazy lowering, bound to the source/certificate owned by the
// prepared program. Statistics reads never initiate compilation.
struct LoweringPreparationStatistics {
    bool attempted = false, ready = false;
    bool supported = true, limited = false, executable_memory_failed = false;
    std::size_t compiler_version = 1;
    std::size_t compilation_steps = 0, retained_bytes = 0, code_bytes = 0;
};
struct CompiledProgramPreparationStatistics {
    LoweringPreparationStatistics slr, predictive, predictive_rim, bounded_word, spine;
    LoweringPreparationStatistics native_slr;
    // Index bits: repetition quotient=1, transparent calls=2.
    std::array<LoweringPreparationStatistics, 4> normalized_slr, normalized_native_slr;
};
} // namespace crank::detail
