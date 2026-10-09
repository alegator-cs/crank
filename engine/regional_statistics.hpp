#pragma once
#include <cstddef>

namespace crank::detail {
// Program-wide cumulative lazy preparation, distinct from per-query regional
// invocations. A shared program may be queried concurrently by many threads.
struct RegionalPreparationStatistics {
    std::size_t follow_steps = 0;
    std::size_t predictive_builds = 0, spine_builds = 0;
    std::size_t retained_certificates = 0, retained_bytes = 0;
    std::size_t analysis_steps = 0, declined = 0;
    std::size_t limit_declines = 0, allocation_declines = 0;
};
} // namespace crank::detail
