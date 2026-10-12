#include "crank.hpp"
#include "equality_matcher.hpp"
#include "fragment_matcher.hpp"
#include "optimizations.hpp"
#include "occurrence_analysis.hpp"
#include <algorithm>
#include <sstream>
#include <mutex>
#include <stdexcept>
#include <type_traits>

namespace crank {
namespace {
struct UnobservedMatch {};
}

struct Program::Impl {
    explicit Impl(std::string pattern, CompileOptions options)
        : ast(parse(pattern)), lengths(ast, options.prepare_integer_row), greedy(make_greedy_plan(ast)),
          count(make_count_plan(ast)),
          // This AST came from parse(), so its reference flag is authoritative.
          // Keep direct make_equality_plan(Ast) structural admission unchanged.
          equality(ast.has_references ? make_equality_plan(ast) : std::nullopt) {
        regular_eligible = ast.regular();
        if (ast.has_calls || ast.has_extended) {
            // Keep the observable call/capture structure for witness/history
            // queries even when pure membership has a finite automaton.
            occurrences = std::make_unique<OccurrenceProgram>(ast);
            context_free_calls = ast.has_calls && occurrences->supports_shared_calls();
            regular_eligible = RegularProgram::eligible(ast);
            if (regular_eligible) {
                try {
                    fragment = std::make_unique<RegularFragment>(ast, ast.root, options.regular);
                    regular = std::make_unique<RegularProgram>(fragment->regular_program());
                }
                catch (const std::length_error& error) { compile_limit = error.what(); }
            }
            return;
        }
        // The cheaper one-variable plans need no subject histogram. Only
        // compile the broader equation system when those plans do not apply.
        if (!count && !equality && !ast.regular()) affine = make_affine_plan(ast);
        if (ast.regular()) {
            try {
                fragment = std::make_unique<RegularFragment>(ast, ast.root, options.regular);
                regular = std::make_unique<RegularProgram>(fragment->regular_program());
            }
            catch (const std::length_error& error) {
                compile_limit = error.what();
                occurrences = std::make_unique<OccurrenceProgram>(ast);
            }
        }
    }
    Ast ast;
    LengthFilter lengths;
    std::optional<GreedyPlan> greedy;
    std::optional<CountPlan> count;
    std::optional<EqualityPlan> equality;
    std::optional<AffinePlan> affine;
    std::unique_ptr<RegularFragment> fragment;
    std::unique_ptr<RegularProgram> regular;
    bool context_free_calls = false;
    bool regular_eligible = false;
    std::unique_ptr<OccurrenceProgram> occurrences;
    // Query-specific native/history compilation is retained after first use.
    // Keeping this separate from `occurrences` leaves automatic dispatch fixed
    // when concurrent callers request different result projections.
    mutable std::once_flag query_once;
    mutable std::unique_ptr<OccurrenceProgram> query_program;
    OccurrenceProgram occurrence_program() const {
        if (occurrences) return *occurrences;
        std::call_once(query_once, [&] {
            query_program = std::make_unique<OccurrenceProgram>(ast);
        });
        return *query_program;
    }
    template<class Query>
    auto state_query(RunOptions options, RunStatistics* statistics, Query&& query) const {
        OccurrenceMatchOptions translated;
        translated.max_states = options.match.max_states;
        translated.max_steps = options.match.max_steps;
        translated.max_repeat = options.match.max_repeat;
        translated.prune = options.match.prune && options.optimize;
        translated.share_calls = false; // preserve the scalar repeat-budget contract
        OccurrenceStatistics observed;
        const auto publish = [&] {
            if (!statistics) return;
            statistics->state = {observed.states, observed.work_steps,
                                 observed.memo_hits, observed.pruned};
            statistics->occurrence = observed;
        };
        try {
            auto result = query(occurrence_program(), translated,
                                statistics ? &observed : nullptr);
            publish();
            return result;
        } catch (...) {
            publish();
            throw;
        }
    }
    std::string compile_limit;
    template<bool Observe>
    bool full_match(std::string_view, RunOptions, RunStatistics*) const;
    void require_backend(Backend backend) const {
        if (backend == Backend::Occurrences) return;
        if (occurrences && !ast.regular() && backend != Backend::Auto &&
            !(backend == Backend::Dfa && regular_eligible))
            throw std::invalid_argument("recursive and scoped matching requires the native occurrences core");
        if (backend == Backend::Pcre2) {
            if (!ExternalRegularProgram::compiled_with_pcre2())
                throw std::invalid_argument("PCRE2 support is not enabled; build crank_pcre2");
            if (!ast.regular() || !fragment)
                throw std::invalid_argument("PCRE2 backend requires a compiled regular fragment");
            if (!fragment->direct() && !fragment->external_available())
                throw std::invalid_argument("PCRE2 JIT declined this regular fragment");
        }
        if (backend == Backend::Greedy && !greedy)
            throw std::invalid_argument("no certified greedy plan for this expression");
        if (backend == Backend::Dfa) {
            if (!regular_eligible) throw std::invalid_argument("DFA backend requires a certified regular expression");
            if (!regular) throw ResourceLimit("DFA compilation: " + compile_limit);
        }
    }
};

Program::Program(std::string pattern, CompileOptions options)
    : impl_(std::make_shared<const Impl>(std::move(pattern), options)) {}
const Ast& Program::ast() const { return impl_->ast; }
detail::SourcePortfolioSnapshot Program::source_portfolio_snapshot() const {
    // Do not call occurrence_program(): diagnostics must not prepare a query.
    return impl_->occurrences ? impl_->occurrences->source_portfolio_snapshot() : detail::SourcePortfolioSnapshot{};
}

template<bool Observe>
bool Program::Impl::full_match(std::string_view subject, RunOptions options, RunStatistics* statistics) const {
    // The common Boolean-only call does not construct diagnostic vectors or
    // strings. Both instantiations follow the same selection and validation.
    std::conditional_t<Observe, RunStatistics, UnobservedMatch> stats;
    const auto regular_stats = [&]() -> RegularStatistics* {
        if constexpr (Observe) return &stats.regular;
        else return nullptr;
    };
    const auto finish = [&](bool answer) {
        if constexpr (Observe) *statistics = std::move(stats);
        return answer;
    };
    // Validate explicitly requested semantics before any negative certificate.
    // Cheap necessary conditions precede scans, DFAs and native graph creation;
    // proving feasibility (or declining a proof) never establishes membership.
    require_backend(options.backend);
    if (options.optimize && !lengths.accepts(subject.size())) {
        if constexpr (Observe) { stats.backend = "length-filter"; stats.length_rejected = true; }
        return finish(false);
    }
    if (options.optimize && options.integer_length_certificate) {
        const auto proof = lengths.check_integer_row(subject.size(), options.integer_length_max_steps);
        if constexpr (Observe) {
            stats.integer_length_checks = proof.checked;
            stats.integer_length_steps = proof.steps;
            stats.integer_length_exhaustions = proof.exhausted;
        }
        if (proof.impossible) {
            if constexpr (Observe) {
                stats.backend = "integer-length-filter";
                stats.length_rejected = stats.integer_length_rejected = true;
            }
            return finish(false);
        }
    }
    // This certificate is for a complete Boolean span. Prefix, endpoint and
    // history APIs never apply the suffix condition. Explicit backend controls
    // retain their execution route so specialization comparisons stay useful.
    if (options.backend == Backend::Auto && options.optimize && options.boundary_proofs &&
        fragment && fragment->has_boundary_certificate()) {
        if constexpr (Observe) ++stats.boundary_checks;
        if (fragment->boundary_impossible(subject)) {
            if constexpr (Observe) {
                stats.backend = "boundary-filter"; stats.boundary_rejected = true;
                stats.regular = fragment->compiled_statistics();
            }
            return finish(false);
        }
    }
    // A DFA compilation limit must not suppress a certified linear scan.
    if ((occurrences && options.backend == Backend::Auto &&
         !regular && (!regular_eligible || !options.optimize || !greedy)) ||
        options.backend == Backend::Occurrences) {
        require_backend(options.backend);
        OccurrenceStatistics* observed = nullptr;
        const auto program = occurrence_program();
        if constexpr (Observe) {
            stats.backend = options.occurrence.share_calls && program.supports_shared_calls()
                ? "native-ast" : "occurrences";
            observed = &stats.occurrence;
        }
        options.occurrence.prune = options.occurrence.prune && options.optimize;
        try { return finish(program.full_match(subject, options.occurrence, observed)); }
        catch (const ResourceLimit&) {
            if constexpr (Observe) *statistics = std::move(stats);
            throw;
        }
    }
    auto backend = options.backend;
    if (backend == Backend::Auto) {
        if (!regular_eligible) backend = Backend::States;
        else if (options.optimize && greedy) backend = Backend::Greedy;
        else backend = Backend::Dfa;
    }
    require_backend(backend);
    if (options.backend == Backend::Auto && options.optimize) {
        // The fragment's retained DFA is shared with forced/endpoints
        // requests, so selecting a bulk scanner does not compile it twice.
        if (fragment && fragment->direct() && (!ast.has_calls || options.call_aware_scan)) {
            if constexpr (Observe) stats.backend = "regular-scan";
            return finish(fragment->full_match(subject, regular_stats()));
        }
        if (fragment && fragment->external_would_run(subject.size(), ExternalRegularPolicy::Auto)) {
            if constexpr (Observe) stats.backend = "pcre2-or-dfa";
            return finish(fragment->full_match(subject, regular_stats()));
        }
        if (count) {
            CountStatistics* observed = nullptr;
            if constexpr (Observe) { stats.backend = "count-plan"; observed = &stats.count; }
            return finish(count->match(subject, observed));
        }
        if (equality) {
            if constexpr (Observe) stats.backend = "equality-plan";
            return finish(equality->match(subject, regular_stats()));
        }
        if (affine) {
            AffineStatistics* observed = nullptr;
            if constexpr (Observe) {
                stats.backend = affine->residual() ? "affine-search" : "affine-plan";
                observed = &stats.affine;
            }
            try { return finish(affine->match(subject, observed, options.affine)); }
            catch (const ResourceLimit&) {
                if constexpr (Observe) *statistics = std::move(stats);
                throw;
            }
        }
    }
    bool matched = false;
    switch (backend) {
    case Backend::Pcre2:
        if constexpr (Observe) stats.backend = fragment->direct() ? "regular-scan" : "pcre2-or-dfa";
        matched = fragment->full_match(subject, regular_stats(), ExternalRegularPolicy::Pcre2);
        break;
    case Backend::Occurrences: throw std::logic_error("unresolved occurrences backend");
    case Backend::Greedy:
        if constexpr (Observe) stats.backend = "greedy";
        matched = greedy->match(subject); break;
    case Backend::Dfa:
        if constexpr (Observe) stats.backend = "dfa";
        matched = regular->full_match(subject, regular_stats());
        break;
    case Backend::States: {
        RunStatistics* observed = nullptr;
        if constexpr (Observe) {
            stats.backend = options.optimize ? "states-projected" : "states";
            observed = &stats;
        }
        try {
            matched = state_query(options, observed, [&](const auto& program, auto translated, auto* detail) {
                return program.exists_from(subject, 0, true, translated, detail);
            });
        } catch (const ResourceLimit&) {
            if constexpr (Observe) *statistics = std::move(stats);
            throw;
        }
        break;
    }
    case Backend::Auto: throw std::logic_error("unresolved backend");
    }
    return finish(matched);
}

bool Program::full_match(std::string_view subject, RunOptions options, RunStatistics* statistics) const {
    return statistics ? impl_->full_match<true>(subject, options, statistics)
                      : impl_->full_match<false>(subject, options, nullptr);
}

std::vector<std::size_t> Program::endpoints(std::string_view subject, std::size_t start,
                                          RunOptions options, RunStatistics* statistics) const {
    RunStatistics stats;
    if (start > subject.size()) throw std::out_of_range("match start exceeds subject size");
    if (options.backend == Backend::Greedy)
        throw std::invalid_argument("greedy plans do not enumerate every endpoint");
    impl_->require_backend(options.backend);
    if ((impl_->occurrences && !impl_->regular && options.backend == Backend::Auto) ||
        options.backend == Backend::Occurrences) {
        const auto program = impl_->occurrence_program();
        stats.backend = options.occurrence.share_calls && program.supports_shared_calls()
            ? "native-ast" : "occurrences";
        options.occurrence.prune = options.occurrence.prune && options.optimize;
        try {
            auto result = program.endpoints(subject, start, options.occurrence, &stats.occurrence);
            if (statistics) *statistics = std::move(stats);
            return result;
        } catch (const ResourceLimit&) {
            if (statistics) *statistics = std::move(stats);
            throw;
        }
    }
    std::vector<std::size_t> result;
    if (impl_->regular_eligible && options.backend != Backend::States) {
        stats.backend = "dfa";
        result = impl_->regular->endpoints(subject, start, &stats.regular);
    } else {
        stats.backend = options.optimize ? "states-projected" : "states";
        try {
            result = impl_->state_query(options, &stats, [&](const auto& program, auto translated, auto* detail) {
                return program.endpoints(subject, start, translated, detail);
            });
        } catch (const ResourceLimit&) {
            if (statistics) *statistics = std::move(stats);
            throw;
        }
    }
    if (statistics) *statistics = stats;
    return result;
}

std::vector<Outcome> Program::outcomes(std::string_view subject, RunOptions options,
                                     RunStatistics* statistics) const {
    RunStatistics stats;
    if (impl_->context_free_calls && options.backend != Backend::Occurrences)
        throw std::invalid_argument("all recursive capture outcomes require explicit Occurrences; use first_match or --first for one witness");
    impl_->require_backend(options.backend);
    if ((impl_->occurrences && options.backend == Backend::Auto) || options.backend == Backend::Occurrences) {
        impl_->require_backend(options.backend);
        stats.backend = "occurrences";
        options.occurrence.prune = options.occurrence.prune && options.optimize;
        try {
            auto result = impl_->occurrence_program().match_outcomes(subject, 0, true, options.occurrence, &stats.occurrence);
            if (statistics) *statistics = std::move(stats);
            return result;
        } catch (const ResourceLimit&) {
            if (statistics) *statistics = std::move(stats);
            throw;
        }
    }
    if (options.backend != Backend::Auto && options.backend != Backend::States)
        throw std::invalid_argument("capture outcomes require the states backend");
    stats.backend = "states";
    if (options.optimize && !impl_->lengths.accepts(subject.size())) {
        stats.backend = "length-filter"; stats.length_rejected = true;
        if (statistics) *statistics = stats;
        return {};
    }
    try {
        auto result = impl_->state_query(options, &stats, [&](const auto& program, auto translated, auto* detail) {
            return program.match_outcomes(subject, 0, true, translated, detail);
        });
        if (statistics) *statistics = std::move(stats);
        return result;
    } catch (const ResourceLimit&) {
        if (statistics) *statistics = std::move(stats);
        throw;
    }
}

std::optional<Outcome> Program::first_match(std::string_view subject, RunOptions options,
                                           RunStatistics* statistics) const {
    RunStatistics stats;
    impl_->require_backend(options.backend);
    if ((impl_->occurrences && options.backend == Backend::Auto) || options.backend == Backend::Occurrences) {
        impl_->require_backend(options.backend);
        const auto program = impl_->occurrence_program();
        stats.backend = options.occurrence.share_calls && program.supports_shared_calls()
            ? "native-ast-witness" : "occurrences-witness";
        options.occurrence.prune = options.occurrence.prune && options.optimize;
        try {
            auto result = program.first_match(subject, options.occurrence, &stats.occurrence);
            if (statistics) *statistics = std::move(stats);
            return result;
        } catch (const ResourceLimit&) {
            if (statistics) *statistics = std::move(stats);
            throw;
        }
    }
    if (options.backend != Backend::Auto && options.backend != Backend::States)
        throw std::invalid_argument("a nonrecursive capture witness requires Auto or States");
    if (options.optimize && !impl_->lengths.accepts(subject.size())) {
        stats.backend = "length-filter"; stats.length_rejected = true;
        if (statistics) *statistics = std::move(stats);
        return {};
    }
    stats.backend = "states-witness";
    try {
        auto result = impl_->state_query(options, &stats, [&](const auto& program, auto translated, auto* detail) {
            return program.first_from(subject, 0, true, translated, detail);
        });
        if (statistics) *statistics = std::move(stats);
        return result;
    } catch (const ResourceLimit&) {
        if (statistics) *statistics = std::move(stats);
        throw;
    }
}

std::optional<TracedOutcome> Program::first_trace(std::string_view subject, RunOptions options,
                                                RunStatistics* statistics) const {
    if (options.backend != Backend::Auto && options.backend != Backend::Occurrences)
        throw std::invalid_argument("occurrence traces require Auto or Occurrences");
    RunStatistics stats;
    stats.backend = "occurrences-trace";
    options.occurrence.prune = options.occurrence.prune && options.optimize;
    try {
        auto result = impl_->occurrence_program().first_traced_match(subject, options.occurrence, &stats.occurrence);
        if (statistics) *statistics = std::move(stats);
        return result;
    } catch (const ResourceLimit&) {
        if (statistics) *statistics = std::move(stats);
        throw;
    }
}

std::string Program::explain() const {
    std::ostringstream out;
    out << "AST nodes: " << impl_->ast.nodes.size() << "\nCaptures: "
        << impl_->ast.capture_names.size() - 1 << "\nCount variables: " << impl_->ast.count_names.size()
        << "\nRegular language backend: " << (impl_->regular_eligible ? "eligible" : impl_->ast.has_calls ? "recursive grammar or stateful calls" : "stateful constraints")
        << '\n' << impl_->lengths.describe() << '\n';
    if (impl_->greedy) out << impl_->greedy->describe() << '\n';
    else out << "Greedy plan: no certificate\n";
    if (impl_->count) out << impl_->count->describe() << '\n';
    else out << "Count plan: not eligible\n";
    if (impl_->equality) out << impl_->equality->describe() << '\n';
    else out << "Equality plan: not eligible\n";
    if (impl_->affine) out << impl_->affine->describe() << '\n';
    else out << "Affine plan: not selected\n";
    if (impl_->regular_eligible && impl_->ast.has_calls)
        out << "Regular call graph: acyclic executed calls; bounded NFA expansion\n";
    if (impl_->context_free_calls) {
        out << "Native AST plan: shared requests, return links, arithmetic end demands\n"
               "Recursive results: membership, all endpoints, or one finite capture witness\n";
    }
    if (impl_->occurrences) {
        out << "Occurrence plan: fair continuations with scoped histories and pending constraints\n"
               "Rule metrics: entry depth zero; leaf height zero\n";
        const OccurrenceAnalysis analysis(impl_->ast);
        if (const auto& eq = analysis.height_equation())
            out << "Height certificate: bytes = " << eq->offset << " + " << eq->coefficient << " * height\n";
    }
    out << "Optional PCRE2 support: "
        << (ExternalRegularProgram::compiled_with_pcre2() ? "enabled" : "not built") << '\n';
    if (impl_->regular) {
        out << "Regular fragment scan: "
            << (impl_->fragment && impl_->fragment->direct() ? "eligible" : "DFA required") << '\n';
        out << "PCRE2 fragment: " << (impl_->fragment && impl_->fragment->external_available() ? "JIT available" : "native") << '\n';
        RegularStatistics stats;
        (void)impl_->regular->full_match("", &stats);
        out << "NFA states: " << stats.nfa_states << "\nDFA states: " << stats.dfa_states << '\n';
    } else if (impl_->regular_eligible) out << "DFA compile limit: " << impl_->compile_limit << "\nFallback: native AST requests and returns\n";
    return out.str();
}
}
