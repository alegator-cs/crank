#include "state_matcher.hpp"
#include "occurrence_matcher.hpp"

namespace crank {
namespace {
// Compatibility is at the query/result level. The unified continuation VM
// accounts cumulative allocations, replacing the scalar evaluator's peak-live
// units; numerical budget thresholds and diagnostic work counts may differ.
OccurrenceMatchOptions translate(MatchOptions options) {
    OccurrenceMatchOptions result;
    result.max_states = options.max_states;
    result.max_steps = options.max_steps;
    result.max_repeat = options.max_repeat;
    result.prune = options.prune;
    // These entrypoints promise the stateful VM's explicit repetition budget.
    // Program's regular/pure dispatch can select the native recognizer itself.
    result.share_calls = false;
    return result;
}

template<class Function>
auto invoke(const Ast& ast, MatchOptions options, MatchStatistics* statistics,
            Function&& query) {
    OccurrenceStatistics observed;
    auto copy_statistics = [&] {
        if (statistics) *statistics = MatchStatistics{observed.states,
            observed.work_steps, observed.memo_hits, observed.pruned};
    };
    try {
        auto result = query(OccurrenceProgram(ast), translate(options), &observed);
        copy_statistics();
        return result;
    } catch (...) {
        copy_statistics();
        throw;
    }
}
}

std::vector<Outcome> match_states(const Ast& ast, std::string_view subject,
        std::size_t start, bool full, MatchOptions options, MatchStatistics* statistics) {
    return invoke(ast, options, statistics, [&](const auto& program, auto translated, auto* observed) {
        return program.match_outcomes(subject, start, full, translated, observed);
    });
}

std::optional<Outcome> match_first(const Ast& ast, std::string_view subject,
        std::size_t start, bool full, MatchOptions options, MatchStatistics* statistics) {
    return invoke(ast, options, statistics, [&](const auto& program, auto translated, auto* observed) {
        return program.first_from(subject, start, full, translated, observed);
    });
}

bool match_exists(const Ast& ast, std::string_view subject,
        std::size_t start, bool full, MatchOptions options, MatchStatistics* statistics) {
    return invoke(ast, options, statistics, [&](const auto& program, auto translated, auto* observed) {
        return program.exists_from(subject, start, full, translated, observed);
    });
}

std::vector<std::size_t> match_endpoints(const Ast& ast, std::string_view subject,
        std::size_t start, MatchOptions options, MatchStatistics* statistics) {
    return invoke(ast, options, statistics, [&](const auto& program, auto translated, auto* observed) {
        return program.endpoints(subject, start, translated, observed);
    });
}
}
