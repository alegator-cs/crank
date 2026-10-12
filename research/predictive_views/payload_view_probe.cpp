#include "ast_machine.hpp"
#include "predictive_payload_view.hpp"
#include "tests/helpers/relation_reference.hpp"

#include <iostream>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

// External post-admission table reader only. No native executor consumes this
// view and no production constructor, admission, retention, or charge is cut.
using namespace crank;
namespace d = crank::detail;
namespace ref = certified_reference;
namespace {
std::size_t checks = 0, cache_cases = 0, interpreter_queries = 0;
std::size_t interpreter_matches = 0, interpreter_no_matches = 0;
std::size_t optional_hook_calls = 0, throw_on_hook_call = 0;
std::string context;

void require(bool value, const char* why) {
    ++checks;
    if (!value) throw std::runtime_error(context + ": " + why);
}
NodeId named_entry(const Ast& ast, const char* name) {
    for (std::size_t i = 1; i < ast.capture_names.size(); ++i)
        if (ast.capture_names[i] == name) return ast.capture_nodes.at(i);
    throw std::runtime_error("missing source capture");
}
bool same_statistics(const d::RegionalPreparationStatistics& a,
                     const d::RegionalPreparationStatistics& b) {
    return a.follow_steps == b.follow_steps && a.predictive_builds == b.predictive_builds &&
        a.spine_builds == b.spine_builds && a.retained_certificates == b.retained_certificates &&
        a.retained_bytes == b.retained_bytes && a.analysis_steps == b.analysis_steps &&
        a.declined == b.declined && a.limit_declines == b.limit_declines &&
        a.allocation_declines == b.allocation_declines;
}
std::size_t retained_bytes(const d::PredictiveRegion& p) {
    return sizeof(p) + p.targets.capacity() * sizeof(NodeId) +
        p.choices.capacity() * sizeof(d::PredictiveRegion::Choice) +
        p.choice_index.capacity() * sizeof(std::size_t);
}
bool same_tables(const d::PredictiveRegion& a, const d::PredictiveRegion& b) {
    if (a.entry != b.entry || a.root_child_count != b.root_child_count ||
        a.targets != b.targets || a.choice_index != b.choice_index ||
        a.choices.size() != b.choices.size()) return false;
    for (std::size_t i = 0; i < a.choices.size(); ++i)
        if (a.choices[i].byte != b.choices[i].byte || a.choices[i].empty != b.choices[i].empty)
            return false;
    return true;
}
void optional_stage(const d::PredictiveRegion&) {
    if (++optional_hook_calls == throw_on_hook_call) throw std::bad_alloc{};
}

static_assert(std::is_const_v<std::remove_reference_t<
    decltype(std::declval<const d::PredictivePayloadView&>().targets())>>);
static_assert(std::is_const_v<std::remove_reference_t<
    decltype(std::declval<const d::PredictivePayloadView&>().choices())>>);
static_assert(std::is_const_v<std::remove_reference_t<
    decltype(std::declval<const d::PredictivePayloadView&>().choice_index())>>);

void inspect_view(const d::PredictivePayloadView& view,
                  const d::RegionalPredictiveCache& cache, NodeId entry, bool exact) {
    require(bool(view), "admitted view absent");
    require(view.source_owner() == &cache, "view changed source owner");
    require(view.entry() == entry && view.record()->entry == entry && view.payload()->entry == entry,
        "selected backing changed the requested entry");
    require(view.root_child_count() == view.record()->root_child_count &&
        view.root_child_count() == view.payload()->root_child_count && view.root_child_count() == no_id,
        "record root metadata changed");
    require(view.exact() == exact, "exact observer identity changed");
    const auto expected = exact ? d::RegionalConsumerContract{{}, true} : *cache.contract(entry);
    require(view.consumer() == expected, "consumer bytes/EOF identity changed");
    require(&view.targets() == &view.payload()->targets &&
        &view.choices() == &view.payload()->choices &&
        &view.choice_index() == &view.payload()->choice_index,
        "accessor did not read the selected immutable backing");
    require(same_tables(*view.record(), *view.payload()), "sharing selected unequal execution tables");
    require(cache.certificate(entry, exact).get() == view.record().get(),
        "view replaced the cache authority record");
}

// Small independently written recursive table reader. It deliberately has no
// native agenda, endpoint publication, accounting, cancellation, or fallback.
// Bounds are assertions on this probe, never a native Match/NoMatch policy.
class ViewInterpreter {
    const Ast& ast_;
    const OccurrenceAnalysis& analysis_;
    const d::PredictivePayloadView& view_;
    std::string_view subject_;
    std::size_t boundary_, steps_ = 0;

    bool node(NodeId id, std::size_t& position, std::size_t depth) {
        require(++steps_ <= 4096 && depth <= 64, "external interpreter bound exceeded");
        require(id < view_.targets().size(), "table target index out of range");
        id = view_.targets().at(id);
        require(id < ast_.nodes.size(), "table selected invalid AST node");
        const auto& n = ast_.nodes.at(id);
        switch (n.kind) {
        case Kind::Empty: case Kind::Define: return true;
        case Kind::CharSet:
            if (position >= boundary_ || !n.characters[static_cast<unsigned char>(subject_[position])]) return false;
            ++position; return true;
        case Kind::Capture: return node(n.children.at(0), position, depth + 1);
        case Kind::Call: return node(n.call_target, position, depth + 1);
        case Kind::Alt: {
            const auto index = view_.choice_index().at(id);
            require(index < view_.choices().size(), "alternative choice index out of range");
            const auto& choice = view_.choices().at(index);
            auto child = position < boundary_ ? choice.byte[static_cast<unsigned char>(subject_[position])] : no_id;
            if (child == no_id) child = choice.empty;
            return child != no_id && node(child, position, depth + 1);
        }
        case Kind::Concat: {
            const auto count = id == ast_.root && view_.root_child_count() != no_id
                ? view_.root_child_count() : n.children.size();
            require(count <= n.children.size(), "root child count exceeds syntax");
            for (std::size_t i = 0; i < count; ++i)
                if (!node(n.children[i], position, depth + 1)) return false;
            return true;
        }
        case Kind::Repeat: {
            std::size_t count = 0;
            const auto child = n.children.at(0);
            while (!n.upper || count < *n.upper) {
                const bool starts = position < boundary_ &&
                    analysis_.boundary(child).first[static_cast<unsigned char>(subject_[position])];
                if (count >= n.lower && !starts) break;
                const auto before = position;
                if (!node(child, position, depth + 1)) return false;
                require(position > before, "certified repeat body made no progress");
                ++count;
            }
            return count >= n.lower;
        }
        default: throw std::runtime_error("effectful external interpreter node");
        }
    }
public:
    ViewInterpreter(const Ast& ast, const OccurrenceAnalysis& analysis,
                    const d::PredictivePayloadView& view, std::string_view subject, std::size_t boundary)
        : ast_(ast), analysis_(analysis), view_(view), subject_(subject), boundary_(boundary) {}
    std::optional<std::size_t> run() {
        require(bool(view_) && boundary_ <= subject_.size() && subject_.size() <= 16,
            "invalid bounded external view query");
        std::size_t position = 0;
        if (!node(view_.entry(), position, 0)) return std::nullopt;
        if (view_.exact()) return position == boundary_ ? std::optional{position} : std::nullopt;
        return view_.consumer().accepts(subject_, position) ? std::optional{position} : std::nullopt;
    }
};

void compare_query(const Ast& ast, const OccurrenceAnalysis& analysis,
                   const d::PredictivePayloadView& view, const std::string& subject,
                   std::size_t boundary, bool expected_match) {
    const auto relation = ref::language(ast, subject);
    std::optional<std::size_t> expected;
    if (view.exact()) {
        if (relation.nodes.at(view.entry()).at(0, boundary)) expected = boundary;
    } else {
        for (std::size_t end = 0; end <= subject.size(); ++end)
            if (relation.nodes.at(view.entry()).at(0, end) && view.consumer().accepts(subject, end)) {
                require(!expected, "scope oracle admitted multiple endpoints in deterministic fixture");
                expected = end;
            }
    }
    require(bool(expected) == expected_match, "balanced oracle fixture was mislabeled");
    const auto actual = ViewInterpreter(ast, analysis, view, subject, boundary).run();
    require(actual == expected, "external table-view decision or endpoint differs from dense relation");
    ++interpreter_queries;
    if (actual) ++interpreter_matches; else ++interpreter_no_matches;
}
void interpreter_cases(const Ast& ast, const OccurrenceAnalysis& analysis,
                       const d::PredictivePayloadView& exact, const d::PredictivePayloadView& scoped) {
    // Four matches and four rejections per observer, before any timing exists.
    const std::vector<std::pair<std::string, bool>> exact_inputs{
        {"", true}, {"ab", true}, {"aabb", true}, {"aaabbb", true},
        {"a", false}, {"abb", false}, {"ba", false}, {"aab", false}};
    const std::vector<std::pair<std::string, bool>> scoped_inputs{
        {";", true}, {"ab;", true}, {"aabb,", true}, {"aaabbb;", true},
        {"a;", false}, {"aa;", false}, {"x;", false}, {"aab,", false}};
    for (const auto& [subject, matches] : exact_inputs)
        compare_query(ast, analysis, exact, subject, subject.size(), matches);
    for (const auto& [subject, matches] : scoped_inputs)
        compare_query(ast, analysis, scoped, subject, subject.size(), matches);
    // A virtual exact end can precede a real byte; the scoped consumer remains
    // a physical-byte/EOF predicate and cannot substitute for that observer.
    compare_query(ast, analysis, exact, "ab;", 2, true);
    compare_query(ast, analysis, scoped, "ab", 2, false);
    require(!scoped.consumer().accepts("ab", 2), "byte-only scope acquired physical EOF permission");
}

d::RegionalPreparationStatistics safe_case(bool sharing, bool exact_first, bool control_entry) {
    context = std::string("safe ") + (sharing ? "enabled" : "disabled") +
        (exact_first ? " exact-first" : "scoped-first") + (control_entry ? " control" : " source");
    const d::AstMachineProgram program(parse(R"((?(DEFINE)(?<S>a(?&S)b|))(?&S);|(?&S),)"));
    const auto& ast = program.ast();
    const auto facts = program.shared_analysis();
    const auto& controls = program.control_targets();
    const auto source = named_entry(ast, "S");
    const auto entry = control_entry ? controls.at(source) : source;
    d::RegionalPredictiveCache cache(ast, *facts, controls, controls);
    d::PredictivePayloadViewOwner views(cache, sharing);
    require(!d::PredictivePayloadView{}, "default view unexpectedly admitted");
    require(!cache.statistics().predictive_builds, "unused owner prepared an authority slot");
    const auto* contract = cache.contract(entry);
    require(contract && contract->bytes.any() && !contract->eof, "safe fixture consumer missing");
    const d::PredictiveRegion ordinary_exact(ast, *facts, d::PredictiveRegion::Scope{entry, {}, true});
    const d::PredictiveRegion ordinary_scoped(ast, *facts,
        d::PredictiveRegion::Scope{entry, contract->bytes, contract->eof});
    require(ordinary_exact.certified && ordinary_scoped.certified, "safe ordinary constructor declined");
    const auto first = views.certificate(entry, exact_first);
    const auto after_first = cache.statistics();
    require(after_first.predictive_builds == 1 && after_first.retained_certificates == 1 &&
        after_first.analysis_steps == (exact_first ? ordinary_exact.analysis_steps : ordinary_scoped.analysis_steps),
        "first admission charge changed");
    require(first.payload() == first.record() && !first.tables_shared(), "first table reader unexpectedly pooled");
    const auto second = views.certificate(entry, !exact_first);
    const auto exact = exact_first ? first : second;
    const auto scoped = exact_first ? second : first;
    inspect_view(exact, cache, entry, true);
    inspect_view(scoped, cache, entry, false);
    require(exact.record() != scoped.record(), "original exact/scoped records aliased");
    require(same_tables(*exact.record(), *scoped.record()), "safe exact/scoped tables differ");
    require(second.tables_shared() == sharing, "opt-in flag did not control canonical table reads");
    require(sharing ? exact.payload() == scoped.payload() : exact.payload() != scoped.payload(),
        "payload identity does not follow opt-in setting");
    require(exact.consumer() != scoped.consumer(), "exact/scoped consumer identities collapsed");
    const auto prepared = cache.statistics();
    require(prepared.predictive_builds == 2 && prepared.spine_builds == 0 &&
        prepared.retained_certificates == 2 && prepared.declined == 0 &&
        prepared.limit_declines == 0 && prepared.allocation_declines == 0,
        "sharing changed admission outcome counters");
    require(prepared.analysis_steps == ordinary_exact.analysis_steps + ordinary_scoped.analysis_steps,
        "sharing omitted or discounted constructor work");
    require(prepared.retained_bytes == retained_bytes(*exact.record()) + retained_bytes(*scoped.record()),
        "sharing discounted original vector capacities or certificate size");
    for (unsigned i = 0; i < 3; ++i) {
        const auto again_exact = views.certificate(entry, true);
        const auto again_scoped = views.certificate(entry, false);
        require(again_exact.record() == exact.record() && again_scoped.record() == scoped.record() &&
            again_exact.payload() == exact.payload() && again_scoped.payload() == scoped.payload(),
            "view hit changed record or backing identity");
        require(same_statistics(cache.statistics(), prepared), "view hit changed preparation statistics");
    }
    interpreter_cases(ast, *facts, exact, scoped);
    require(same_statistics(cache.statistics(), prepared), "external reader changed production statistics");
    // Source/control entries can have equivalent languages, but must retain
    // their distinct entry metadata and cannot share across that key.
    const auto other_entry = control_entry ? source : controls.at(source);
    require(other_entry != entry, "fixture did not retain distinct source/control entry metadata");
    const auto other = views.certificate(other_entry, true);
    inspect_view(other, cache, other_entry, true);
    require(other.payload() != exact.payload() && !other.tables_shared(), "pool crossed entry metadata");
    const auto with_other = cache.statistics();
    require(with_other.predictive_builds == 3 && with_other.retained_certificates == 3 &&
        with_other.retained_bytes == prepared.retained_bytes + retained_bytes(*other.record()),
        "different entry's full reservation was omitted");
    ++cache_cases;
    return prepared;
}

void unsafe_cases() {
    for (bool sharing : {false, true}) for (bool exact_first : {false, true}) {
        context = std::string("unsafe ") + (sharing ? "enabled" : "disabled") +
            (exact_first ? " exact-first" : "scoped-first");
        const d::AstMachineProgram program(parse(R"((?(DEFINE)(?<S>a(?&S)|))(?&S)a;|(?&S);)"));
        const auto& ast = program.ast(); const auto facts = program.shared_analysis();
        const auto& controls = program.control_targets(); const auto entry = named_entry(ast, "S");
        d::RegionalPredictiveCache cache(ast, *facts, controls, controls);
        d::PredictivePayloadViewOwner views(cache, sharing);
        const auto first = views.certificate(entry, exact_first);
        const auto second = views.certificate(entry, !exact_first);
        const auto exact = exact_first ? first : second;
        const auto scoped = exact_first ? second : first;
        require(bool(exact) && !scoped, "semantic scoped decline revived from exact sibling");
        require(exact.payload() == exact.record() && !exact.tables_shared(), "failed sibling entered canonical pool");
        const auto prepared = cache.statistics();
        require(prepared.predictive_builds == 2 && prepared.retained_certificates == 1 &&
            prepared.declined == 1 && prepared.limit_declines == 0 && prepared.allocation_declines == 0 &&
            prepared.retained_bytes == retained_bytes(*exact.record()), "unsafe authority charges changed");
        for (unsigned i = 0; i < 3; ++i) {
            require(!views.certificate(entry, false) && views.certificate(entry, true).record() == exact.record(),
                "failed authority slot or admitted exact slot changed on view hit");
            require(same_statistics(cache.statistics(), prepared), "declined view hit added work");
        }
        ++cache_cases;
    }
}

void owner_isolation() {
    context = "same source, different authority owners";
    const d::AstMachineProgram program(parse(R"((?(DEFINE)(?<S>a(?&S)b|))(?&S);|(?&S),)"));
    const auto& ast = program.ast(); const auto facts = program.shared_analysis();
    const auto& controls = program.control_targets(); const auto entry = named_entry(ast, "S");
    d::RegionalPredictiveCache left(ast, *facts, controls, controls), right(ast, *facts, controls, controls);
    d::PredictivePayloadViewOwner left_views(left, true), right_views(right, true);
    const auto l_exact = left_views.certificate(entry, true), l_scoped = left_views.certificate(entry, false);
    const auto r_scoped = right_views.certificate(entry, false), r_exact = right_views.certificate(entry, true);
    require(l_exact.payload() == l_scoped.payload() && r_exact.payload() == r_scoped.payload(),
        "same-owner safe pair did not share");
    require(l_exact.source_owner() == &left && r_exact.source_owner() == &right &&
        l_exact.payload() != r_exact.payload() && l_exact.record() != r_exact.record(),
        "pool crossed authority owner even with identical immutable inputs");
    require(same_tables(*l_exact.payload(), *r_exact.payload()) &&
        same_statistics(left.statistics(), right.statistics()), "independent owners prepared unequal controls");
}

void optional_failure() {
    context = "optional phase bad_alloc after independent admission";
    const d::AstMachineProgram program(parse(R"((?(DEFINE)(?<S>a(?&S)b|))(?&S);|(?&S),)"));
    const auto& ast = program.ast(); const auto facts = program.shared_analysis();
    const auto& controls = program.control_targets(); const auto entry = named_entry(ast, "S");
    d::RegionalPredictiveCache cache(ast, *facts, controls, controls);
    optional_hook_calls = 0; throw_on_hook_call = 2;
    d::PredictivePayloadViewOwner views(cache, true, optional_stage);
    const auto first = views.certificate(entry, true);
    const auto before_second = cache.statistics();
    const auto interrupted = views.certificate(entry, false);
    const auto after_second = cache.statistics();
    require(optional_hook_calls == 2 && first && interrupted && !interrupted.tables_shared() &&
        interrupted.payload() == interrupted.record(), "optional allocation seam lost independent admitted view");
    require(after_second.predictive_builds == before_second.predictive_builds + 1 &&
        after_second.retained_certificates == before_second.retained_certificates + 1 &&
        after_second.retained_bytes == before_second.retained_bytes + retained_bytes(*interrupted.record()) &&
        after_second.declined == 0 && after_second.limit_declines == 0 && after_second.allocation_declines == 0,
        "optional stage failure changed production admission accounting");
    inspect_view(interrupted, cache, entry, false);
    const auto retry = views.certificate(entry, false);
    require(optional_hook_calls == 3 && retry.record() == interrupted.record() &&
        retry.tables_shared() && retry.payload() == first.payload(), "optional retry did not use same admitted authority");
    require(interrupted.payload() == interrupted.record() && !interrupted.tables_shared(),
        "optional retry mutated an already-returned independent view");
    require(same_statistics(cache.statistics(), after_second), "optional retry discounted or recharged production work");
    d::RegionalPredictiveCache disabled_cache(ast, *facts, controls, controls);
    optional_hook_calls = 0; throw_on_hook_call = 1;
    d::PredictivePayloadViewOwner disabled(disabled_cache, false, optional_stage);
    const auto independent_exact = disabled.certificate(entry, true);
    const auto independent_scoped = disabled.certificate(entry, false);
    require(independent_exact && independent_scoped && optional_hook_calls == 0 &&
        !independent_exact.tables_shared() && !independent_scoped.tables_shared(),
        "disabled opt-in still invoked optional sharing stage");
    require(same_statistics(disabled_cache.statistics(), after_second),
        "optional failure did not preserve disabled-control statistics");
}
} // namespace

int main() {
    try {
        for (bool exact_first : {false, true}) for (bool control_entry : {false, true}) {
            const auto disabled = safe_case(false, exact_first, control_entry);
            const auto enabled = safe_case(true, exact_first, control_entry);
            require(same_statistics(disabled, enabled), "enabled/disabled preparation statistics differ");
        }
        unsafe_cases(); owner_isolation(); optional_failure();
        std::cout << "{\"status\":\"passed\",\"checks\":" << checks
            << ",\"cache_cases\":" << cache_cases << ",\"external_interpreter_queries\":" << interpreter_queries
            << ",\"external_interpreter_matches\":" << interpreter_matches
            << ",\"external_interpreter_no_matches\":" << interpreter_no_matches
            << ",\"native_view_queries\":0,\"timing_measurements\":0}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n'; return 1;
    }
}
