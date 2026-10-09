#pragma once

#include <cstddef>
#include <limits>
#include <optional>
#include <vector>

namespace crank {

// Path labels are resolved to AST source IDs during parsing. Runtime lookup
// therefore never depends on spelling or confuses a count with a group.
enum class OccurrenceSource { Count, Group };
enum class OccurrenceMetric { Count, Height, Depth, Bytes };
enum class OccurrenceAnchor { Root, Current, Ancestor };

struct OccurrenceStep {
    OccurrenceSource source = OccurrenceSource::Count;
    std::size_t id = std::numeric_limits<std::size_t>::max();
    std::optional<std::size_t> index;
};

struct CountPath {
    OccurrenceAnchor anchor = OccurrenceAnchor::Root;
    std::size_t parents = 0;
    std::vector<OccurrenceStep> steps;
    OccurrenceMetric metric = OccurrenceMetric::Count;
};

} // namespace crank
