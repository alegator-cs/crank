#include "ast_machine.hpp"
#include "boolean_source_view.hpp"
#include "source_view_portfolio.hpp"

#include <algorithm>
#include <atomic>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

// Bounded serial correctness pilot. Exactly 128 production recognition/observer
// calls are frozen below; no timing or concurrent execution belongs to this test.
// The cursor and span-relations oracle import no predictor or analysis facts.
using namespace crank;
using namespace crank::detail;
namespace {
const SourceCandidateId chain_id{4, SourceRepresentation::Predictive};
const BooleanSourceView::Options chain_options{false, false, false, true};
const std::string arithmetic = R"((?(DEFINE)(?<E>(?&E)\+(?&E)|(?&E)\*(?&E)|\((?&E)\)|[a-z]+))(?:(?&E);)+)";
const std::string transfer = R"((?(DEFINE)(?<Q>(?&Q)/(?&Q)|(?&Q)\|(?&Q)|\[(?&Q)\]|[xy]+))(?:(?&Q)#)+)";
std::string context;
std::size_t checks = 0, calls = 0, boolean_calls = 0, observer_calls = 0, control_calls = 0;
std::size_t boolean_subject_matches = 0, boolean_subject_nonmatches = 0;
std::size_t control_matches = 0, control_nonmatches = 0, resource_limits = 0, cancellations = 0, unavailable = 0;
void require(bool value, const std::string& why) {
    ++checks;
    if (!value) throw std::runtime_error(context + ": " + why);
}
void counted(std::size_t& group) {
    require(calls < 128, "production-call cap exceeded"); ++calls; ++group;
}
OccurrenceMatchOptions bounded() {
    OccurrenceMatchOptions options;
    options.max_states = 200000; options.max_steps = 1000000;
    options.max_occurrences = 200000; options.max_call_depth = 32; options.max_repeat = 16;
    return options;
}
OccurrenceMatchOptions observer_options(bool normalized = true) {
    auto options = bounded(); options.normalized_exact_programs = normalized;
    return options;
}
OccurrenceMatchOptions boolean_options(bool normalized = true) {
    auto options = observer_options(normalized); options.prune = false;
    return options;
}
void observed_original(const OccurrenceStatistics& stats) {
    require(stats.normalized_exact_attempts == 0 && stats.selected_source_view == no_id,
        "Boolean source view escaped its observer/entry/assistance gate");
}
bool same_node(const Node& a, const Node& b, bool derived = true) {
    return a.kind == b.kind && a.characters == b.characters && a.children == b.children &&
        a.lower == b.lower && a.upper == b.upper && a.capture_id == b.capture_id &&
        a.call_target == b.call_target && a.bind_count == b.bind_count &&
        a.use_count == b.use_count && a.count_path == b.count_path && a.forward == b.forward &&
        (!derived || (a.nullable == b.nullable && a.minimum_length == b.minimum_length && a.stateful == b.stateful));
}
void same_source(const Ast& a, const Ast& b) {
    require(a.root == b.root && a.nodes.size() == b.nodes.size() &&
        a.capture_names == b.capture_names && a.capture_nodes == b.capture_nodes &&
        a.count_names == b.count_names && a.count_nodes == b.count_nodes &&
        a.count_paths.size() == b.count_paths.size(), "source identity or metadata changed");
    for (NodeId id = 0; id < a.nodes.size(); ++id)
        require(same_node(a.nodes[id], b.nodes[id]), "source node changed at " + std::to_string(id));
}
NodeId expression(const Ast& ast) {
    require(ast.capture_nodes.size() == 2, "fixture must have one source capture");
    return ast.capture_nodes[1];
}
std::size_t edges(const Ast& ast) {
    std::size_t result = 0; for (const auto& node : ast.nodes) result += node.children.size(); return result;
}

struct Cursor {
    std::string_view input;
    std::string letters;
    char first_operator, second_operator, open, close, separator;
    std::size_t at = 0;
    bool atom() {
        if (at < input.size() && input[at] == open) {
            ++at; if (!expr() || at == input.size() || input[at] != close) return false;
            ++at; return true;
        }
        const auto start = at;
        while (at < input.size() && letters.find(input[at]) != std::string::npos) ++at;
        return at != start;
    }
    bool expr() {
        if (!atom()) return false;
        while (at < input.size() && (input[at] == first_operator || input[at] == second_operator)) {
            ++at; if (!atom()) return false;
        }
        return true;
    }
    bool stream() {
        if (input.empty()) return false;
        do { if (!expr() || at == input.size() || input[at] != separator) return false; ++at; }
        while (at < input.size());
        return true;
    }
};
bool independent(std::string_view input, bool renamed) {
    Cursor cursor{input, renamed ? "xy" : "abcdefghijklmnopqrstuvwxyz",
        renamed ? '/' : '+', renamed ? '|' : '*', renamed ? '[' : '(', renamed ? ']' : ')', renamed ? '#' : ';'};
    return cursor.stream();
}

// Least fixed point of finite span relations, from bottom. Every production
// retains its original node identity; this tests old-node language equality,
// including source nodes that become unreachable from the isolated stream.
using Matrix = std::vector<unsigned char>;
Matrix identity(std::size_t size) {
    Matrix result(size * size); for (std::size_t i = 0; i < size; ++i) result[i * size + i] = 1; return result;
}
Matrix product(const Matrix& a, const Matrix& b, std::size_t size) {
    Matrix result(size * size);
    for (std::size_t i = 0; i < size; ++i) for (std::size_t k = i; k < size; ++k)
        if (a[i * size + k]) for (std::size_t j = k; j < size; ++j)
            if (b[k * size + j]) result[i * size + j] = 1;
    return result;
}
std::vector<Matrix> relations(const Ast& ast, std::string_view input) {
    const auto size = input.size() + 1;
    std::vector<Matrix> result(ast.nodes.size(), Matrix(size * size));
    for (std::size_t round = 0; round < 256; ++round) {
        bool changed = false;
        for (NodeId id = 0; id < ast.nodes.size(); ++id) {
            const auto& node = ast.nodes[id]; Matrix next(size * size);
            switch (node.kind) {
            case Kind::Empty: case Kind::Define: next = identity(size); break;
            case Kind::CharSet:
                for (std::size_t i = 0; i < input.size(); ++i)
                    if (node.characters.test(static_cast<unsigned char>(input[i]))) next[i * size + i + 1] = 1;
                break;
            case Kind::Call: next = result[node.call_target]; break;
            case Kind::Capture: next = result[node.children.front()]; break;
            case Kind::Alt:
                for (auto child : node.children) for (std::size_t i = 0; i < next.size(); ++i) next[i] |= result[child][i];
                break;
            case Kind::Concat:
                next = identity(size); for (auto child : node.children) next = product(next, result[child], size); break;
            case Kind::Repeat: {
                require(!node.upper && node.lower <= 1, "oracle fixture outside its repeat contract");
                auto closure = result[node.children.front()];
                for (std::size_t i = 0; i < size; ++i) closure[i * size + i] = 1;
                for (std::size_t k = 0; k < size; ++k) for (std::size_t i = 0; i <= k; ++i)
                    if (closure[i * size + k]) for (std::size_t j = k; j < size; ++j)
                        closure[i * size + j] |= closure[k * size + j];
                next = node.lower ? product(result[node.children.front()], closure, size) : std::move(closure);
                break;
            }
            default: throw std::runtime_error("oracle encountered observed state");
            }
            if (next != result[id]) { result[id] = std::move(next); changed = true; }
        }
        if (!changed) return result;
    }
    throw std::runtime_error("finite relation oracle exceeded its fixed round cap");
}
struct IndependentFact {
    bool nullable = false, productive = false;
    std::bitset<256> first;
    std::size_t minimum = no_id;
    bool operator==(const IndependentFact& other) const {
        return nullable == other.nullable && productive == other.productive && first == other.first && minimum == other.minimum;
    }
};
std::vector<IndependentFact> fresh_facts(const Ast& ast) {
    std::vector<IndependentFact> previous(ast.nodes.size());
    // Synchronous rounds intentionally differ from production's in-place sweeps.
    for (unsigned round = 0; round < 512; ++round) {
        auto next = previous;
        for (NodeId id = 0; id < ast.nodes.size(); ++id) {
            const auto& node = ast.nodes[id]; IndependentFact fact;
            switch (node.kind) {
            case Kind::Empty: case Kind::Define: fact.nullable = fact.productive = true; fact.minimum = 0; break;
            case Kind::CharSet: fact.first = node.characters; fact.productive = node.characters.any();
                if (fact.productive) fact.minimum = 1; break;
            case Kind::Call: fact = previous[node.call_target]; break;
            case Kind::Capture: fact = previous[node.children.front()]; break;
            case Kind::Alt:
                for (auto child : node.children) {
                    const auto& part = previous[child]; fact.nullable |= part.nullable; fact.productive |= part.productive;
                    fact.first |= part.first; fact.minimum = std::min(fact.minimum, part.minimum);
                }
                break;
            case Kind::Concat:
                fact.nullable = fact.productive = true; fact.minimum = 0;
                for (auto child : node.children) {
                    const auto& part = previous[child]; fact.nullable &= part.nullable; fact.productive &= part.productive;
                    fact.minimum = fact.minimum > no_id - part.minimum ? no_id : fact.minimum + part.minimum;
                }
                if (fact.productive) for (auto child : node.children) {
                    fact.first |= previous[child].first; if (!previous[child].nullable) break;
                }
                else fact.minimum = no_id;
                break;
            case Kind::Repeat: {
                const auto& part = previous[node.children.front()];
                fact.nullable = !node.lower || part.nullable; fact.productive = !node.lower || part.productive;
                if (!node.lower) fact.minimum = 0;
                else if (part.productive) fact.minimum = part.minimum > no_id / node.lower ? no_id : part.minimum * node.lower;
                if ((!node.upper || *node.upper) && part.productive) fact.first = part.first;
                break;
            }
            default: throw std::runtime_error("fact oracle encountered observed state");
            }
            next[id] = fact;
        }
        if (next == previous) return next; previous = std::move(next);
    }
    throw std::runtime_error("fact oracle exceeded its fixed round cap");
}
void verify_view(const Ast& source, const BooleanSourceView& view) {
    require(view.ready(), "arithmetic view unavailable");
    const auto& ast = view.ast(); const auto n = source.nodes.size(), e = expression(source), old_alt = source.nodes[e].children.front();
    require(ast.root == source.root && ast.capture_names == source.capture_names && ast.capture_nodes == source.capture_nodes,
        "root or capture metadata changed in Boolean view");
    require(ast.nodes.size() == n + 5 && edges(ast) == edges(source) + 9,
        "five-helper / nine-edge growth contract changed");
    require(view.statistics().arithmetic_chain_rewrites == 1 && view.statistics().cloned_nodes == 5,
        "rewrite statistics do not describe construction");
    for (NodeId id = 0; id < n; ++id) {
        auto original = source.nodes[id]; if (id == e) original.children = {n + 4};
        require(same_node(original, ast.nodes[id], false), "an original structural node changed beyond the capture edge");
        require(view.provenance()[id] == id, "old provenance identity changed");
    }
    require(ast.nodes[n].kind == Kind::Alt && ast.nodes[n + 1].kind == Kind::Alt &&
        ast.nodes[n + 2].kind == Kind::Concat && ast.nodes[n + 3].kind == Kind::Repeat &&
        ast.nodes[n + 4].kind == Kind::Concat && ast.nodes[n + 2].children == std::vector<NodeId>{n + 1, n} &&
        ast.nodes[n + 3].children == std::vector<NodeId>{n + 2} && ast.nodes[n + 4].children == std::vector<NodeId>{n, n + 3} &&
        ast.nodes[n + 3].lower == 0 && !ast.nodes[n + 3].upper, "helper graph shape changed");
    const auto first_binary = source.nodes[old_alt].children.front();
    for (NodeId id = n; id < n + 5; ++id) {
        require(view.provenance()[id] == (id == n + 2 ? first_binary : old_alt), "helper construction-site provenance differs");
        require(ast.nodes[id].capture_id == no_id && ast.nodes[id].call_target == no_id &&
            ast.nodes[id].bind_count == no_id && ast.nodes[id].use_count == no_id && !ast.nodes[id].forward,
            "helper introduced a capture, call or effect");
    }
    const auto independent_facts = fresh_facts(ast); const auto& actual = view.analysis();
    require(actual.nullable_complete && actual.first_complete && actual.productive_complete && actual.minimum_complete,
        "partial fresh facts were published");
    for (NodeId id = 0; id < ast.nodes.size(); ++id) {
        const auto& want = independent_facts[id]; const auto& boundary = actual.boundary(id); const auto& length = actual.length(id);
        require(boundary.nullable == want.nullable && boundary.first == want.first &&
            length.productive == want.productive && length.minimum == want.minimum,
            "fresh facts differ at node " + std::to_string(id));
    }
    require(actual.boundary(n + 3).nullable && !actual.boundary(old_alt).nullable &&
        actual.boundary(n + 1).first != actual.boundary(old_alt).first,
        "fixture no longer distinguishes helper facts from provenance-anchor facts");
}
void same_preparation(const SourcePortfolioSnapshot& a, const SourcePortfolioSnapshot& b) {
    require(a.view_attempts == b.view_attempts && a.candidate_attempts == b.candidate_attempts &&
        a.transformed_nodes == b.transformed_nodes && a.retained_bytes == b.retained_bytes &&
        a.analysis_steps == b.analysis_steps && a.candidate_analysis_steps == b.candidate_analysis_steps &&
        a.reserved_candidate_work == b.reserved_candidate_work, "cached preparation was charged twice");
    for (std::size_t i = 0; i < a.views.size(); ++i) {
        require(a.views[i].preparation.status == b.views[i].preparation.status &&
            a.views[i].preparation.normalization_steps == b.views[i].preparation.normalization_steps &&
            a.views[i].preparation.nullable_steps == b.views[i].preparation.nullable_steps, "view snapshot changed on cache read");
        for (std::size_t j = 0; j < a.views[i].candidates.size(); ++j)
            require(a.views[i].candidates[j].status == b.views[i].candidates[j].status &&
                a.views[i].candidates[j].analysis_steps == b.views[i].candidates[j].analysis_steps,
                "candidate snapshot changed on cache read");
    }
}
struct Fixture {
    bool renamed;
    std::string pattern;
    Ast ast;
    AstMachineProgram reused_original;
    OccurrenceProgram warmed_public;
    Fixture(bool transfer_case) : renamed(transfer_case), pattern(transfer_case ? transfer : arithmetic), ast(parse(pattern)),
        reused_original(ast), warmed_public(ast) {}
};
void boolean_cases(Fixture& fixture, const std::vector<std::string>& inputs) {
    const auto untouched = fixture.ast;
    SourceViewPortfolio portfolio(fixture.ast); const auto* candidate = portfolio.prepare(chain_id);
    require(candidate && candidate->predictive()->certified, "actual predictor did not admit fixture");
    verify_view(fixture.ast, candidate->source_view());
    const auto before = portfolio.snapshot();
    require(portfolio.prepare(chain_id) == candidate && portfolio.prepare_view(4) == &candidate->source_view(),
        "repeated preparation replaced immutable cached identity");
    same_preparation(before, portfolio.snapshot());
    std::cout << (fixture.renamed ? "transfer" : "arithmetic") << " source_nodes=" << fixture.ast.nodes.size()
        << " view_nodes=" << candidate->source_view().ast().nodes.size() << " fresh_fact_charges="
        << candidate->source_view().statistics().nullable_steps << " predictor_charges="
        << candidate->statistics().analysis_steps << '\n';
    for (const auto& input : inputs) {
        context = (fixture.renamed ? "transfer " : "arithmetic ") + input;
        const bool expected = independent(input, fixture.renamed);
        const auto old_relations = relations(fixture.ast, input), new_relations = relations(candidate->source_view().ast(), input);
        for (NodeId id = 0; id < fixture.ast.nodes.size(); ++id)
            require(old_relations[id] == new_relations[id], "independent old-node Boolean language changed");
        require(bool(old_relations[fixture.ast.root][input.size()]) == expected, "cursor and relation oracles differ");
        auto original_options = boolean_options(false);
        AstMachineProgram fresh_original(fixture.ast), fresh_candidate(fixture.ast);
        OccurrenceStatistics old_stats, derived_stats;
        counted(boolean_calls); require(fresh_original.full_match(input, original_options, &old_stats) == expected, "fresh original differs");
        observed_original(old_stats);
        counted(boolean_calls); require(fixture.reused_original.full_match(input, original_options, &old_stats) == expected, "reused original differs");
        observed_original(old_stats);
        auto interpreted = boolean_options();
        interpreted.compiled_exact_programs = interpreted.lowered_predictive = false;
        counted(boolean_calls); require(fresh_candidate.full_match_candidate(chain_id, input, interpreted, &derived_stats) == expected,
            "forced five-helper predictor differs");
        require(derived_stats.selected_source_view == 4 && derived_stats.normalized_exact_attempts == 1,
            "forced candidate did not execute arithmetic view");
        require(derived_stats.lowered_predictive_attempts == 0, "interpreter control entered lowering");
        OccurrenceStatistics public_stats; counted(boolean_calls);
        require(fixture.warmed_public.full_match(input, boolean_options(), &public_stats) == expected, "actual automatic Boolean dispatch differs");
        require(public_stats.selected_source_view == 4 && public_stats.normalized_exact_attempts == 1,
            "Auto failed to select arithmetic predictor");
        require(public_stats.lowered_predictive_attempts == 1, "default candidate did not execute portable lowering");
        expected ? ++boolean_subject_matches : ++boolean_subject_nonmatches;
    }
    same_source(untouched, fixture.ast); same_source(untouched, fixture.reused_original.ast());
}
bool same_trace(const std::optional<TracedOutcome>& a, const std::optional<TracedOutcome>& b) {
    if (bool(a) != bool(b)) return false; if (!a) return true;
    if (a->outcome != b->outcome || a->occurrences.size() != b->occurrences.size()) return false;
    for (std::size_t i = 0; i < a->occurrences.size(); ++i) {
        const auto& x = a->occurrences[i]; const auto& y = b->occurrences[i];
        if (x.id != y.id || x.parent != y.parent || x.kind != y.kind || x.source_id != y.source_id ||
            x.source_node != y.source_node || x.name != y.name || x.span != y.span || x.count != y.count ||
            x.iteration != y.iteration || x.depth != y.depth || x.height != y.height || x.source_rule != y.source_rule ||
            x.closed != y.closed || x.children != y.children) return false;
    }
    return true;
}
void observers(Fixture& fixture) {
    require(fixture.warmed_public.source_portfolio_snapshot().views[4].preparation.status == SourcePreparationStatus::Ready,
        "observer owner was not demonstrably warmed by Boolean execution");
    OccurrenceProgram original(fixture.ast);
    for (const auto& input : std::vector<std::string>{"a;", "a+a+a;", "(a);", "a+;"}) {
        context = "warm observer " + input;
        auto original_options = observer_options(false), warmed_options = observer_options(); OccurrenceStatistics a, b;
        counted(observer_calls); const auto first_a = original.first_match(input, original_options, &a);
        counted(observer_calls); const auto first_b = fixture.warmed_public.first_match(input, warmed_options, &b);
        require(first_a == first_b, "first witness or scalar captures changed after warming");
        observed_original(a); observed_original(b);
        counted(observer_calls); const auto trace_a = original.first_traced_match(input, original_options, &a);
        counted(observer_calls); const auto trace_b = fixture.warmed_public.first_traced_match(input, warmed_options, &b);
        require(same_trace(trace_a, trace_b), "original occurrence history changed after warming");
        observed_original(a); observed_original(b);
        if (input == "a+a+a;") {
            require(trace_b.has_value(), "flat original trace missing"); std::size_t e_occurrences = 0;
            for (const auto& record : trace_b->occurrences)
                e_occurrences += record.kind == OccurrenceKind::Group && record.source_id == 1;
            require(e_occurrences == 5, "original binary capture history was replaced by the one-capture chain");
        }
        counted(observer_calls); const auto all_a = original.match_outcomes(input, 0, true, original_options, &a);
        counted(observer_calls); const auto all_b = fixture.warmed_public.match_outcomes(input, 0, true, warmed_options, &b);
        require(all_a == all_b, "richer recursive outcomes changed after warming");
        observed_original(a); observed_original(b);
        counted(observer_calls); const auto ends_a = original.endpoints(input, 0, original_options, &a);
        counted(observer_calls); const auto ends_b = fixture.warmed_public.endpoints(input, 0, warmed_options, &b);
        require(ends_a == ends_b, "original endpoint set changed after warming");
        observed_original(a); observed_original(b);
    }
    require(original.source_portfolio_snapshot().views[4].preparation.status == SourcePreparationStatus::Unattempted,
        "observer alone prepared arithmetic view");
}
void negative_schemas(const Ast& source) {
    const auto e = expression(source), body = source.nodes[e].children.front();
    const auto binary = source.nodes[body].children[0], bracket = source.nodes[body].children[2], ident = source.nodes[body].children[3];
    const auto reject = [&](Ast changed, const char* name) {
        context = std::string("negative schema ") + name;
        BooleanSourceView view(changed, chain_options);
        require(!view.ready() && !view.statistics().arithmetic_chain_rewrites, "unsafe arithmetic shape admitted");
    };
    auto changed = source; changed.nodes[binary].children[2] = changed.nodes[ident].children[0]; reject(changed, "asymmetric operand");
    changed = source; Node other = changed.nodes[e]; other.children = {ident};
    const auto other_id = changed.nodes.size(); changed.nodes.push_back(other);
    changed.nodes[changed.nodes[binary].children[2]].call_target = other_id; reject(changed, "different target with same capture id/name");
    changed = source; changed.nodes[body].children.push_back(ident); reject(changed, "extra alternative");
    changed = source; changed.nodes[ident].lower = 0; reject(changed, "nullable identifier");
    changed = source; changed.nodes[ident].upper = 2; reject(changed, "finite identifier");
    changed = source; changed.nodes[changed.nodes[binary].children[1]].characters.set('a'); reject(changed, "non-singleton operator");
    changed = source; changed.nodes[changed.nodes[binary].children[1]].characters.reset();
    changed.nodes[changed.nodes[binary].children[1]].characters.set('a'); reject(changed, "singleton operator overlaps identifier");
    changed = source; changed.nodes[changed.nodes[bracket].children[2]].characters = changed.nodes[changed.nodes[bracket].children[0]].characters;
    reject(changed, "equal delimiters");
    changed = source; changed.nodes[changed.nodes[binary].children[0]].call_target = source.root; reject(changed, "root target instead of capture");
    changed = source; Node hidden; hidden.forward = true; changed.nodes.push_back(hidden); reject(changed, "unreachable forward effect");
    changed = source; hidden = Node{}; hidden.children = {source.nodes.size() + 4}; changed.nodes.push_back(hidden); reject(changed, "unreachable malformed edge");
    changed = source; hidden = Node{}; hidden.use_count = 0; changed.nodes.push_back(hidden); changed.has_counts = false;
    reject(changed, "forged pure summaries with actual count use");
    changed = source; changed.nodes[ident].children = {body}; reject(changed, "identifier body is not a byte class");
    changed = source; changed.capture_names[1] = "unrelated_spelling";
    BooleanSourceView renamed(changed, chain_options); require(renamed.ready(), "recognizer improperly depends on E spelling");
    changed = source; std::reverse(changed.nodes[body].children.begin(), changed.nodes[body].children.end());
    BooleanSourceView reordered(changed, chain_options); require(reordered.ready(), "branch-order permutation declined");
    BooleanSourceView tiny(source, chain_options, 1);
    require(!tiny.ready() && tiny.statistics().status == SourcePreparationStatus::PreparationLimit,
        "view preparation cap became a certified negative");
    BooleanSourceView complete(source, chain_options);
    require(complete.ready(), "work-boundary fixture unavailable");
    BooleanSourceView exact(source, chain_options, complete.compilation_steps());
    BooleanSourceView one_short(source, chain_options, complete.compilation_steps() - 1);
    require(exact.ready() && !one_short.ready() &&
        one_short.statistics().status == SourcePreparationStatus::PreparationLimit,
        "exact preparation work boundary changed");
    changed = source; hidden = Node{};
    hidden.children.assign(8192 - edges(source), source.root); changed.nodes.push_back(hidden);
    BooleanSourceView full_edges(changed, chain_options);
    require(!full_edges.ready() && full_edges.statistics().status == SourcePreparationStatus::PreparationLimit,
        "nine helper edges escaped output preflight");
    changed = source; while (changed.nodes.size() <= 512) changed.nodes.push_back(Node{});
    BooleanSourceView huge(changed, chain_options);
    require(!huge.ready() && huge.statistics().status == SourcePreparationStatus::PreparationLimit, "source-node cap was bypassed");
    for (unsigned cap = 0; cap < 6; ++cap) {
        SourcePortfolioLimits limits;
        if (cap == 0) limits.max_views = 0;
        if (cap == 1) limits.max_transformed_nodes = source.nodes.size() + 4;
        if (cap == 2) limits.max_analysis_steps = 1;
        if (cap == 3) limits.max_candidate_attempts = 0;
        if (cap == 4) limits.max_candidate_work = 0;
        if (cap == 5) limits.max_retained_bytes = 0;
        SourceViewPortfolio portfolio(source, limits); require(!portfolio.prepare(chain_id), "aggregate cap admitted candidate");
        const auto before = portfolio.snapshot(); require(!portfolio.prepare(chain_id), "cached aggregate failure disappeared");
        same_preparation(before, portfolio.snapshot());
    }
    context = "diagnostic preparation shares the unchanged aggregate view cap";
    SourceViewPortfolio diagnostic(source);
    require(diagnostic.prepare_view(0) && diagnostic.prepare(chain_id), "diagnostic prefix unavailable");
    (void)diagnostic.prepare_view(1); (void)diagnostic.prepare_view(2);
    require(!diagnostic.prepare_view(3) &&
        diagnostic.snapshot().views[3].preparation.status == SourcePreparationStatus::PortfolioLimit,
        "research preparation silently enlarged the aggregate view allowance");
}
enum class Result { Match, NoMatch, ResourceLimit, Cancelled, Unavailable };
template<class Action> Result control(Action action) {
    counted(control_calls);
    try { const bool value = action(); value ? ++control_matches : ++control_nonmatches; return value ? Result::Match : Result::NoMatch; }
    catch (const ResourceLimit&) { ++resource_limits; return Result::ResourceLimit; }
    catch (const AstExecutionCancelled&) { ++cancellations; return Result::Cancelled; }
    catch (const std::invalid_argument&) { ++unavailable; return Result::Unavailable; }
}
Ast outside_context(const Ast& source) {
    auto ast = source; Node root; root.kind = Kind::Concat;
    root.children = {source.root, source.nodes[expression(source)].children.front()};
    ast.root = ast.nodes.size(); ast.nodes.push_back(root); ast.capture_nodes[0] = ast.root; return ast;
}
void controls(Fixture& fixture) {
    context = "tight execution budget and fallback";
    AstMachineProgram program(fixture.ast);
    for (unsigned budget = 0; budget < 4; ++budget) for (const auto& input : std::vector<std::string>{"a;", "a+;"}) {
        auto options = bounded();
        options.prune = false;
        if (budget < 2) options.max_steps = budget + 2;
        else options.max_states = budget;
        OccurrenceStatistics derived, original;
        const auto got = control([&] { return program.full_match_candidate(chain_id, input, options, &derived); });
        options.normalized_exact_programs = false;
        const auto old = control([&] { return program.full_match(input, options, &original); });
        require(got == Result::ResourceLimit && old == Result::ResourceLimit, "exhaustion became Match/NoMatch/unavailability");
        require(derived.native_normalized_program_fallbacks == 1 && derived.native_acceleration_discarded_states > 0,
            "forced alternate did not discard its partial attempt and fall back exactly once");
        require(derived.selected_source_view == no_id && derived.normalized_exact_attempts == 0,
            "forced alternate state survived fallback");
    }
    std::atomic_bool cancelled{true}; OccurrenceStatistics stats;
    context = "supported cancellation and validation precedence";
    require(control([&] { return program.full_match_candidate(chain_id, "a;", bounded(), &stats, &cancelled); }) == Result::Cancelled,
        "ready forced-candidate cancellation failed to propagate");
    require(control([&] { return program.full_match_node(expression(fixture.ast), "a", bounded(), &stats, &cancelled); }) == Result::Cancelled,
        "nonroot cancellation failed to propagate");
    observed_original(stats);
    const std::vector<NodeId> none;
    require(control([&] { return program.full_match_any(none, "a;", bounded(), &stats, &cancelled); }) == Result::NoMatch,
        "empty-entry precedence changed");
    const auto shared = outside_context(fixture.ast); AstMachineProgram shared_program(shared);
    require(control([&] { return shared_program.full_match_candidate(chain_id, "a;a", bounded(), &stats, &cancelled); }) == Result::Unavailable,
        "unavailable preparation no longer precedes cancellation");
    SourceViewPortfolio shared_portfolio(shared); const auto* shared_view = shared_portfolio.prepare_view(4);
    require(shared_view && !shared_portfolio.prepare(chain_id) &&
        shared_portfolio.snapshot().views[4].candidates[static_cast<unsigned>(SourceRepresentation::Predictive)].status ==
            SourcePreparationStatus::AdmissionDeclined, "outside old-Alt use was certified predictive");
    for (const auto& input : std::vector<std::string>{"a;a+a", "a;a+"}) {
        context = "shared outside original definition " + input;
        const auto old_relations = relations(shared, input), new_relations = relations(shared_view->ast(), input);
        for (NodeId id = 0; id < shared.nodes.size(); ++id)
            require(old_relations[id] == new_relations[id], "outside-context old-node language changed");
        const bool truth = old_relations[shared.root][input.size()] != 0;
        auto off = bounded(); off.normalized_exact_programs = false;
        const auto old = control([&] { return shared_program.full_match(input, off, &stats); });
        const auto current = control([&] { return shared_program.full_match(input, bounded(), &stats); });
        require(old == current && current == (truth ? Result::Match : Result::NoMatch), "shared-definition fallback changed answer");
        require(stats.selected_source_view != 4, "unavailable shared predictor executed");
    }
    context = "changing options on prepared owner";
    auto off = bounded(); off.normalize_arithmetic_chains = false;
    require(control([&] { return program.full_match("a;", off, &stats); }) == Result::Match, "disabled option changed language");
    require(stats.selected_source_view != 4, "cached view ignored disabled arithmetic option");
    require(control([&] { return program.full_match("a;", bounded(), &stats); }) == Result::Match && stats.selected_source_view == 4,
        "reenabled owner failed to reuse arithmetic candidate");
    context = "aggregate preparation cap stays unavailable with original fallback";
    SourcePortfolioLimits cap; cap.max_candidate_attempts = 0; AstMachineProgram capped(fixture.ast, cap);
    require(control([&] { return capped.full_match_candidate(chain_id, "a;", bounded(), &stats); }) == Result::Unavailable,
        "portfolio cap was reported as NoMatch");
    require(control([&] { return capped.full_match("a;", bounded(), &stats); }) == Result::Match, "portfolio cap broke original fallback");
    require(stats.selected_source_view != 4, "capped candidate executed");
    context = "warm assistance exclusion";
    AtomicExclusions empty(fixture.ast.nodes.size());
    require(control([&] { return program.full_match_assisted("a;", bounded(), empty, &stats); }) == Result::Match,
        "assisted Boolean fallback changed language"); observed_original(stats);
    DeferredAssistance deferred;
    require(control([&] { return program.full_match_deferred("a;", bounded(), deferred, &stats); }) == Result::Match,
        "deferred Boolean fallback changed language"); observed_original(stats);
    context = "warm nonroot and multiple-entry exclusion";
    require(control([&] { return program.full_match_node(expression(fixture.ast), "a+a", bounded(), &stats); }) == Result::Match,
        "nonroot original Boolean changed"); observed_original(stats);
    const std::vector<NodeId> entries{expression(fixture.ast)};
    require(control([&] { return program.full_match_any(entries, "a+a", bounded(), &stats); }) == Result::Match,
        "multiple-entry original Boolean changed"); observed_original(stats);
}
}
int main() {
    try {
        Fixture arithmetic_fixture(false), transfer_fixture(true);
        context = "arithmetic graph preparation";
        boolean_cases(arithmetic_fixture, {"a;", "ab+(c*d);e;", "a+a+a;", "((z));", "a);", "a+;", "(a*b;", "A;"});
        context = "byte-renamed graph preparation";
        boolean_cases(transfer_fixture, {"x#", "xy/[x|y]#", "x/x|xy#", "[x/[y]]#", "x/#", "[x|y#", "z#", "x##"});
        observers(arithmetic_fixture); negative_schemas(arithmetic_fixture.ast); controls(arithmetic_fixture);
        require(calls == 128 && boolean_calls == 64 && observer_calls == 32 && control_calls == 32,
            "frozen pilot call allocation changed");
        std::cout << "test_arithmetic_chain passed checks=" << checks << " calls=" << calls
            << " boolean=" << boolean_calls << " observers=" << observer_calls << " controls=" << control_calls
            << " boolean_subject_match=" << boolean_subject_matches << " boolean_subject_no_match=" << boolean_subject_nonmatches
            << " control_match=" << control_matches << " control_no_match=" << control_nonmatches << " resource_limit=" << resource_limits
            << " cancellation=" << cancellations << " unavailable=" << unavailable << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "test_arithmetic_chain HARNESS_FAILURE calls=" << calls << " " << error.what() << '\n'; return 1;
    } catch (...) {
        std::cerr << "test_arithmetic_chain HARNESS_FAILURE calls=" << calls << " unexpected nonstandard exception\n"; return 1;
    }
}
