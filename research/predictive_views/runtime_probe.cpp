#include "ast_machine.hpp"
#include "predictive_scope_summary.hpp"
#include "regional_predictive.hpp"
#include "tests/helpers/relation_reference.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

// Experimental probe only. No summary is consulted before, or instead of,
// ordinary successful preparation. No production cache/admission path changes.
// The dense independent relation is restricted to the short validation inputs.
using namespace crank;
namespace d = crank::detail;
namespace ref = certified_reference;
namespace {
using Clock = std::chrono::steady_clock;
std::size_t checks = 0, oracle_queries = 0, native_queries = 0;
std::size_t regional_attempts = 0, regional_successes = 0;
std::size_t limit_cases = 0, cancelled_cases = 0, budget_cases = 0;
std::string context;
volatile std::uint64_t sink = 0;

void require(bool value, const std::string& why) {
    ++checks;
    if (!value) throw std::runtime_error(context + ": " + why);
}
std::uint64_t elapsed(Clock::time_point begin) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count();
}
OccurrenceMatchOptions options(bool regional = true) {
    OccurrenceMatchOptions out;
    // Same root accelerator controls as tests/test_regions.cpp. This exposes
    // the native regional bridge; public Auto end-to-end timing is not claimed.
    out.bounded_word_prediction = out.deterministic_programs = false;
    out.exact_end_spines = out.predictive_recursion = out.predictive_exact_rims = false;
    out.regional_execution = regional;
    out.max_states = 4'000'000;
    out.max_steps = 32'000'000;
    return out;
}
NodeId named_entry(const Ast& ast, const char* name) {
    for (std::size_t i = 1; i < ast.capture_names.size(); ++i)
        if (ast.capture_names[i] == name) return ast.capture_nodes.at(i);
    throw std::runtime_error("missing source capture");
}
bool same_payload(const d::PredictiveRegion& a, const d::PredictiveRegion& b) {
    if (a.entry != b.entry || a.root_child_count != b.root_child_count ||
        a.targets != b.targets || a.choice_index != b.choice_index ||
        a.choices.size() != b.choices.size()) return false;
    for (std::size_t i = 0; i < a.choices.size(); ++i)
        if (a.choices[i].byte != b.choices[i].byte || a.choices[i].empty != b.choices[i].empty)
            return false;
    return true;
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
bool shadow_admits(const d::PredictiveScopeSummary& summary, const std::bitset<256>& follow) {
    const auto prediction = summary.admission(follow);
    require(prediction.has_value(), "incomplete summary was interpreted as semantic rejection");
    return *prediction;
}

void cache_cases() {
    for (const bool unsafe : {false, true}) for (const bool exact_first : {false, true})
        for (const bool control_entry : {false, true}) {
        context = std::string("cache ") + (unsafe ? "unsafe-scope" : "dual-success") +
            (exact_first ? " exact-first" : "scoped-first") +
            (control_entry ? " control-entry" : "source-entry");
        const auto parsed = parse(unsafe
            ? R"((?(DEFINE)(?<S>a(?&S)|))(?&S)a;|(?&S);)"
            : R"((?(DEFINE)(?<S>a(?&S)b|))(?&S);|(?&S),)");
        const d::AstMachineProgram program(parsed);
        const auto& ast = program.ast();
        const auto analysis = program.shared_analysis();
        const auto& controls = program.control_targets();
        const auto source = named_entry(ast, "S");
        const auto entry = control_entry ? controls.at(source) : source;
        // Public internal accessor exposes only control_targets, not the
        // private equivalence targets. Passing controls twice deliberately
        // tests this real cache with no additional equivalent-node collapse.
        // Native program queries below still use its true private equivalents.
        d::RegionalPredictiveCache cache(ast, *analysis, controls, controls);
        require(cache.statistics().predictive_builds == 0,
            "observing an unused cache prepared certificates");
        const auto* contract = cache.contract(entry);
        require(contract && contract->bytes.any() && !contract->eof,
            "fixture did not expose a byte-only producer consumer");
        require(!cache.limit_reached(), "FOLLOW initialization limited");
        d::PredictiveRegion ordinary_exact(ast, *analysis, d::PredictiveRegion::Scope{entry, {}, true});
        d::PredictiveRegion ordinary_scoped(ast, *analysis,
            d::PredictiveRegion::Scope{entry, contract->bytes, contract->eof});
        require(ordinary_exact.certified && !ordinary_exact.limit_reached, "base exact guard failed");
        require(ordinary_scoped.certified == !unsafe && !ordinary_scoped.limit_reached,
            "unsafe-scope fixture did not selectively decline");
        const auto first = cache.certificate(entry, exact_first);
        const auto once = cache.statistics();
        require(once.predictive_builds == 1, "first request did not charge exactly one preparation");
        require(once.analysis_steps == (exact_first ? ordinary_exact.analysis_steps : ordinary_scoped.analysis_steps),
            "first constructor work not charged exactly");
        const auto second = cache.certificate(entry, !exact_first);
        const auto prepared = cache.statistics();
        const auto exact = exact_first ? first : second;
        const auto scoped = exact_first ? second : first;
        require(exact && bool(scoped) == !unsafe, "cache result differed from ordinary constructors");
        require(prepared.predictive_builds == 2 && prepared.spine_builds == 0,
            "exact/scoped preparation attempts lost their separate accounting");
        require(prepared.analysis_steps == ordinary_exact.analysis_steps + ordinary_scoped.analysis_steps,
            "exact/scoped constructor work was merged or omitted");
        require(prepared.retained_certificates == (unsafe ? 1U : 2U) &&
            prepared.declined == (unsafe ? 1U : 0U), "success and decline accounting changed");
        require(prepared.retained_bytes == retained_bytes(*exact) + (scoped ? retained_bytes(*scoped) : 0),
            "retained bytes did not charge separate certificates");
        require(!prepared.limit_declines && !prepared.allocation_declines, "unexpected preparation limit");
        if (scoped) {
            require(exact.get() != scoped.get(), "production cache unexpectedly aliased exact/scoped objects");
            require(same_payload(*exact, *scoped), "dual-success payloads differ");
        }
        d::PredictiveScopeSummaryCache<OccurrenceAnalysis> shadow(ast, *analysis);
        require(!shadow.summary(ordinary_scoped) || !unsafe, "failed preparation seeded a summary");
        const auto* summary = shadow.summary(*exact);
        require(summary && summary->complete && shadow_admits(*summary, {}), "successful exact guard did not seed summary");
        require(shadow_admits(*summary, contract->bytes) == !unsafe, "summary disagreed with ordinary scoped builder");
        if (unsafe) require(summary->forbidden[static_cast<unsigned char>('a')], "singleton unsafe witness absent");
        if (scoped) require(shadow.summary(*scoped) == summary, "semantic key did not reuse summary");
        require(shadow.builds == 1, "summary was not built once per immutable entry");
        for (unsigned i = 0; i < 4; ++i) {
            require(cache.certificate(entry, true) == exact && cache.certificate(entry, false) == scoped,
                "repeated request altered positive or permanently declined cache entry");
            require(same_statistics(cache.statistics(), prepared), "cache hit changed production preparation statistics");
        }
        require(same_statistics(cache.statistics(), prepared), "out-of-band summary changed production charges");
        std::cout << "CACHE unsafe=" << unsafe << " exact_first=" << exact_first
            << " control_entry=" << control_entry << " builds=" << prepared.predictive_builds
            << " work=" << prepared.analysis_steps << " follow_work=" << prepared.follow_steps
            << " retained=" << prepared.retained_certificates << " retained_bytes=" << prepared.retained_bytes
            << " declined=" << prepared.declined << " summary_builds=" << shadow.builds
            << " summary_work=" << shadow.analysis_steps << '\n';
    }
    context = "EOF and byte observer identity";
    d::RegionalConsumerContract bytes, with_eof;
    bytes.bytes.set(static_cast<unsigned char>(';'));
    bytes.bytes.set(255);
    with_eof = bytes; with_eof.eof = true;
    require(bytes != with_eof, "EOF contract identity collapsed");
    require(bytes.accepts(";", 0) && bytes.accepts(std::string(1, char(255)), 0), "byte contract rejected admitted byte");
    require(!bytes.accepts(";", 1) && !bytes.accepts("", 0), "byte-only contract acquired EOF permission");
    require(with_eof.accepts(";", 1) && with_eof.accepts("", 0), "EOF contract lost exit permission");
    require(!with_eof.accepts(";", 2) && !bytes.accepts("a", 0), "contract accepted an invalid endpoint");
}

struct Fixture {
    const char* name;
    const char* pattern;
    // Eight deliberately balanced words: four admitted and four rejected.
    // The independent relation verifies that count before any timed service.
    std::vector<std::string> inputs;
    bool require_regional;
};
std::vector<Fixture> fixtures() {
    return {
        {"safe-recursive-island", R"((?(DEFINE)(?<S>a(?&S)b|x))(?&S);|(?&S),)",
            {"x;", "axb;", "aaxbb,", "aaaxbbb;", "aaxb;", "aaxbbb,", "x", "y;"}, true},
        {"nullable-safe-island", R"((?(DEFINE)(?<S>a(?&S)b|))(?&S);|(?&S),)",
            {";", ",", "ab;", "aabb,", "a;", "abb,", "", "ba;"}, true},
        {"unsafe-follow-exact-only", R"((?(DEFINE)(?<S>a(?&S)|))(?&S)a;|(?&S);)",
            {";", "a;", "aa;", "aaaa;", "", "b;", "a", "aa,"}, false},
        {"left-recursive-decline", R"((?(DEFINE)(?<S>(?&S)a|b))(?&S);|(?&S),)",
            {"b;", "ba;", "baa,", "baaa;", "a;", "bb;", "ba", "bba,"}, false},
        {"shared-equivalent-observers", R"((?(DEFINE)(?<A>\((?&A)\)|a+)(?<B>\((?&B)\)|a+))(?:(?&A)x|(?&B)a))",
            {"aa", "aax", "(aa)a", "((a))x", "a", "(a)", "(aa)xq", "((a))"}, false},
        {"multiple-producer-publications", R"((?(DEFINE)(?<S>a(?&S)b|x))(?&S);(?&S),|(?&S),(?&S);)",
            {"x;x,", "x,x;", "aaxbb;axb,", "axb,x;", "x;", "x,x,", "axb;ab,", "aaxbb;x"}, true},
        {"mutual-typed-island", R"((?(DEFINE)(?<S>a(?&T)b|x)(?<T>c(?&S)d|y))(?&S);|(?&S),)",
            {"x;", "ayb,", "acxdb;", "acaybdb,", "ay;", "acxddb;", "acydb;", "x"}, true},
        {"binary-scope", R"((?(DEFINE)(?<S>[\x00\xff](?&S)a|x))(?&S)\xff|(?&S)\x00)",
            {std::string("x\xff", 2), std::string("x\0", 2), std::string("\0xa\xff", 4), std::string("\xffxa\0", 4),
             std::string("x", 1), std::string("\0x\xff", 3), std::string("\xffxaa\0", 5), std::string("y\xff", 2)}, true},
    };
}

struct Work {
    std::size_t queries = 0, matches = 0, steps = 0, states = 0;
    std::size_t discarded_steps = 0, regional_discarded_steps = 0, regional = 0, fallbacks = 0;
    void add(bool matched, const OccurrenceStatistics& s) {
        ++queries; matches += matched; steps += s.work_steps; states += s.states;
        // regional_discarded_steps is a SUBSET of the all-acceleration total;
        // publish it separately, never add it again when reporting total work.
        discarded_steps += s.native_acceleration_discarded_steps;
        regional_discarded_steps += s.regional_discarded_steps;
        regional += s.regional_attempts; fallbacks += s.native_regional_fallbacks;
    }
};

std::vector<bool> expected(const Ast& ast, const Fixture& fixture) {
    std::vector<bool> result;
    unsigned matches = 0;
    for (const auto& subject : fixture.inputs) {
        require(subject.size() <= 32, "dense oracle input exceeded bound");
        const auto relation = ref::language(ast, subject);
        ++oracle_queries;
        const bool value = relation.nodes[ast.root].at(0, subject.size());
        result.push_back(value); matches += value;
    }
    require(matches == 4 && result.size() == 8, "fixture ceased to be balanced 4 Match / 4 NoMatch");
    return result;
}

void observer_and_runtime_cases(const Fixture& fixture) {
    context = fixture.name;
    const d::AstMachineProgram program(parse(fixture.pattern));
    const auto& ast = program.ast();
    const auto truth = expected(ast, fixture);
    std::size_t this_attempts = 0;
    for (std::size_t i = 0; i < fixture.inputs.size(); ++i) {
        const auto& subject = fixture.inputs[i];
        context = std::string(fixture.name) + " input=" + ref::escaped(subject);
        OccurrenceStatistics stats, off_stats;
        require(program.full_match(subject, options(), &stats) == truth[i], "native Boolean disagreed with independent relation");
        require(program.full_match(subject, options(false), &off_stats) == truth[i], "regional-off baseline disagreed with relation");
        native_queries += 2;
        require(!stats.limit_reached && !off_stats.limit_reached && !stats.proof_nodes,
            "completed Boolean acquired ResourceLimit or proof history");
        require(!off_stats.regional_attempts, "disabled regional execution activated");
        this_attempts += stats.regional_attempts;
        regional_attempts += stats.regional_attempts; regional_successes += stats.regional_successes;
        const auto relation = ref::language(ast, subject); ++oracle_queries;
        for (std::size_t begin = 0; begin <= subject.size(); ++begin) {
            std::vector<std::size_t> ends;
            for (std::size_t end = begin; end <= subject.size(); ++end)
                if (relation.nodes[ast.root].at(begin, end)) ends.push_back(end);
            require(program.endpoints(subject, begin, options(), &stats) == ends,
                "endpoint observer changed relation");
            ++native_queries;
            require(!stats.regional_attempts, "full-root regional scope entered endpoint observer");
        }
        require(program.first_match(subject, options(), &stats) == program.first_match(subject, options(false)),
            "witness/capture identity changed");
        native_queries += 2;
        require(!stats.regional_attempts, "full-root regional scope entered witness observer");
        for (std::size_t slot = 1; slot < ast.capture_nodes.size(); ++slot) {
            const auto entry = ast.capture_nodes[slot];
            if (entry == no_id) continue;
            require(program.full_match_node(entry, subject, options(), &stats) == relation.nodes[entry].at(0, subject.size()),
                "source-specific full-span contract changed");
            ++native_queries;
            if (entry != ast.root) require(!stats.regional_attempts, "root consumer permission entered source query");
        }
    }
    if (fixture.require_regional) require(this_attempts > 0, "positive fixture never reached actual native regional route");
    std::cout << "RUNTIME fixture=" << fixture.name << " matches=4 rejects=4 regional_attempts=" << this_attempts << '\n';
}

void exceptional_cases() {
    context = "native ResourceLimit and precancellation";
    const d::AstMachineProgram program(parse(R"((?(DEFINE)(?<S>a(?&S)b|x))(?&S);|(?&S),)"));
    auto limited = options(false); limited.max_states = limited.max_steps = 0;
    OccurrenceStatistics stats;
    bool exhausted = false;
    try { (void)program.full_match("aaxbb;", limited, &stats); }
    catch (const ResourceLimit&) { exhausted = true; }
    require(exhausted && stats.limit_reached, "forced native exhaustion returned NoMatch");
    ++limit_cases;
    std::atomic_bool cancelled{true};
    bool caught = false;
    try { (void)program.full_match_node(program.ast().root, "aaxbb;", options(), &stats, &cancelled); }
    catch (const d::AstExecutionCancelled&) { caught = true; }
    require(caught && !stats.limit_reached, "precancellation returned Boolean or ResourceLimit");
    ++cancelled_cases;
    // Bounded quota grid retains the existing optional acceleration/fallback
    // contract: any successful regional-off run must still succeed with it on.
    for (const std::string subject : {"x;", "aaxbb;", "aaxb;", "aaxbb,"}) {
        const bool truth = ref::language(program.ast(), subject).nodes[program.ast().root].at(0, subject.size());
        ++oracle_queries;
        for (const std::size_t states : {0U, 1U, 4U, 16U, 64U, 512U})
            for (const std::size_t steps : {0U, 1U, 8U, 64U, 512U}) {
            auto off = options(false); off.max_states = states; off.max_steps = steps;
            auto on = off; on.regional_execution = true;
            std::optional<bool> baseline;
            try { baseline = program.full_match(subject, off); }
            catch (const ResourceLimit&) {}
            try {
                require(program.full_match(subject, on, &stats) == truth, "quota produced wrong Boolean");
                require(!stats.limit_reached, "successful fallback retained limit flag");
            } catch (const ResourceLimit&) {
                require(!baseline.has_value(), "optional regional attempt stole successful fallback allowance");
                require(stats.limit_reached, "quota exhaustion lost ResourceLimit flag");
            }
            require(stats.native_regional_fallbacks <= 1, "regional retry repeated");
            require(stats.regional_discarded_steps <= stats.native_acceleration_discarded_steps,
                "regional discarded work exceeds total discarded work");
            native_queries += 2; ++budget_cases;
        }
    }
}

void distribution(const char* metric, const char* fixture, std::vector<std::uint64_t> values, std::size_t divisor = 1) {
    std::sort(values.begin(), values.end());
    const auto quantile = [&](std::size_t numerator) { return double(values[(values.size() - 1) * numerator / 100]) / divisor; };
    std::cout << "TIMING metric=" << metric << " fixture=" << fixture << " samples=" << values.size()
        << " divisor=" << divisor << " ns_min=" << quantile(0) << " ns_median=" << quantile(50)
        << " ns_p90=" << quantile(90) << " ns_max=" << quantile(100) << '\n';
    std::cout << "RAW metric=" << metric << " fixture=" << fixture << " batch_ns=";
    for (const auto value : values) std::cout << value << ',';
    std::cout << '\n';
}

void measure(const Fixture& fixture, unsigned repetitions) {
    context = std::string("measure ") + fixture.name;
    const auto parsed = parse(fixture.pattern);
    const auto truth = expected(parsed, fixture);
    std::vector<std::uint64_t> cold, first;
    // Parse is deliberately outside these clocks. Every cold program receives
    // one of the eight balanced first queries, including adverse rejections.
    for (std::size_t i = 0; i < fixture.inputs.size(); ++i) {
        auto start = Clock::now();
        const d::AstMachineProgram program(parsed);
        cold.push_back(elapsed(start));
        start = Clock::now();
        const bool actual = program.full_match(fixture.inputs[i], options());
        first.push_back(elapsed(start));
        require(actual == truth[i], "cold first query disagreed with relation");
        ++native_queries;
    }
    const d::AstMachineProgram program(parsed);
    // Prime the actual native program over the whole balanced set. It already
    // owns its usual cache; a mask lookup is not a proposed replacement backend.
    for (std::size_t i = 0; i < fixture.inputs.size(); ++i) {
        require(program.full_match(fixture.inputs[i], options()) == truth[i], "warmup disagreed");
        ++native_queries;
    }
    const auto& ast = program.ast();
    const auto analysis = program.shared_analysis();
    const auto& controls = program.control_targets();
    d::RegionalPredictiveCache external(ast, *analysis, controls, controls);
    auto shadow = std::make_unique<d::PredictiveScopeSummaryCache<OccurrenceAnalysis>>(ast, *analysis);
    struct Prepared { std::shared_ptr<const d::PredictiveRegion> region; std::bitset<256> scope; bool admitted; };
    std::vector<Prepared> prepared;
    std::uint64_t external_preparation_ns = 0, summary_build_ns = 0;
    // Build shadow masks only after real ordinary exact constructor successes.
    // Timings are kept OUTSIDE cold-native and first-native service clocks.
    for (std::size_t slot = 1; slot < ast.capture_nodes.size(); ++slot) {
        const auto source = ast.capture_nodes[slot];
        if (source == no_id) continue;
        const auto entry = controls.at(source);
        auto start = Clock::now();
        const auto exact = external.certificate(entry, true);
        const auto scoped = external.certificate(entry, false);
        external_preparation_ns += elapsed(start);
        if (!exact) continue;
        const auto* contract = external.contract(entry);
        require(contract, "external ordinary prepared source has no contract");
        start = Clock::now();
        const auto* summary = shadow->summary(*exact);
        summary_build_ns += elapsed(start);
        if (!summary) continue; // Bounded experimental summary may decline.
        require(shadow_admits(*summary, contract->bytes) == bool(scoped), "shadow differed during measurement setup");
        prepared.push_back({exact, contract->bytes, bool(scoped)});
    }
    const auto production_before = program.regional_preparation_statistics();
    std::vector<std::uint64_t> native_batches, shadow_batches;
    Work native_work, shadow_work;
    const std::size_t queries_per_batch = fixture.inputs.size() * repetitions;
    for (unsigned batch = 0; batch < 8; ++batch) for (unsigned turn = 0; turn < 2; ++turn) {
        // Alternate AB/BA order to distribute adjacent machine drift. Both
        // modes call THE SAME unchanged cached native program for every word.
        const bool add_shadow = (turn ^ (batch & 1U)) != 0;
        auto& work = add_shadow ? shadow_work : native_work;
        std::size_t matched = 0, mask_checks = 0;
        const auto start = Clock::now();
        for (unsigned repetition = 0; repetition < repetitions; ++repetition)
            for (std::size_t j = 0; j < fixture.inputs.size(); ++j) {
            const auto i = (j + batch + repetition) % fixture.inputs.size();
            OccurrenceStatistics stats;
            const bool actual = program.full_match(fixture.inputs[i], options(), &stats);
            // Validation is outside timed batches; status distribution and
            // checksum expose gross accidental changes without timed throws.
            matched += actual; work.add(actual, stats);
            if (add_shadow) for (const auto& value : prepared) {
                const auto* summary = shadow->summary(*value.region);
                if (summary) {
                    const auto prediction = summary->admission(value.scope);
                    mask_checks += prediction.has_value() && *prediction == value.admitted;
                }
            }
        }
        const auto duration = elapsed(start);
        require(matched == 4U * repetitions, "timed query distribution changed");
        if (add_shadow) require(mask_checks == queries_per_batch * prepared.size(), "timed shadow check changed");
        sink = sink + matched + mask_checks;
        (add_shadow ? shadow_batches : native_batches).push_back(duration);
        native_queries += queries_per_batch;
    }
    require(same_statistics(program.regional_preparation_statistics(), production_before),
        "primed native preparation changed during warm measurements");
    require(native_work.matches == shadow_work.matches && native_work.steps == shadow_work.steps &&
        native_work.states == shadow_work.states && native_work.discarded_steps == shadow_work.discarded_steps &&
        native_work.regional == shadow_work.regional && native_work.fallbacks == shadow_work.fallbacks,
        "out-of-band shadow changed native work or query outcomes");
    distribution("cold_native_program_parse_excluded", fixture.name, cold);
    distribution("first_native_query", fixture.name, first);
    distribution("warm_native_query", fixture.name, native_batches, queries_per_batch);
    distribution("warm_native_query_plus_external_shadow", fixture.name, shadow_batches, queries_per_batch);
    const auto external_stats = external.statistics();
    std::cout << "MEASUREMENT fixture=" << fixture.name << " native_queries_per_mode=" << native_work.queries
        << " matches_per_mode=" << native_work.matches << " rejects_per_mode=" << native_work.queries - native_work.matches
        << " native_final_work_per_mode=" << native_work.steps << " native_discarded_work_per_mode=" << native_work.discarded_steps
        << " regional_discarded_subset=" << native_work.regional_discarded_steps
        << " regional_attempts_per_mode=" << native_work.regional << " regional_fallbacks_per_mode=" << native_work.fallbacks
        << " production_builds=" << production_before.predictive_builds << " production_analysis=" << production_before.analysis_steps
        << " production_retained_bytes=" << production_before.retained_bytes
        << " external_preparation_ns=" << external_preparation_ns << " external_predictive_builds=" << external_stats.predictive_builds
        << " external_work=" << external_stats.analysis_steps << " external_retained_bytes=" << external_stats.retained_bytes
        << " summary_build_ns=" << summary_build_ns << " summary_builds=" << shadow->builds
        << " summary_work=" << shadow->analysis_steps << " summary_cache_object_bytes=" << sizeof(*shadow)
        << " successful_summary_entries=" << prepared.size() << '\n';
}
} // namespace

int main(int argc, char** argv) {
    try {
        bool measurements = true;
        unsigned repetitions = 32;
        for (int i = 1; i < argc; ++i) {
            const std::string argument = argv[i];
            if (argument == "--check-only") measurements = false;
            else if (argument == "--repetitions" && i + 1 < argc) {
                repetitions = unsigned(std::stoul(argv[++i]));
                if (!repetitions || repetitions > 1024) throw std::runtime_error("repetitions must be 1..1024");
            } else throw std::runtime_error("usage: runtime_probe [--check-only] [--repetitions 1..1024]");
        }
        std::cout << "CONTRACT experimental=shadow_only production_admission=unchanged gpu=none oracle_max_input=32\n";
        cache_cases();
        const auto all = fixtures();
        for (const auto& fixture : all) observer_and_runtime_cases(fixture);
        exceptional_cases();
        require(regional_attempts > 0 && regional_successes > 0, "no actual regional success was observed");
        if (measurements) for (const auto& fixture : all) measure(fixture, repetitions);
        std::cout << "{\"status\":\"passed\",\"checks\":" << checks << ",\"oracle_queries\":" << oracle_queries
            << ",\"native_queries\":" << native_queries << ",\"regional_attempts\":" << regional_attempts
            << ",\"regional_successes\":" << regional_successes << ",\"forced_resource_limits\":" << limit_cases
            << ",\"precancellations\":" << cancelled_cases << ",\"budget_cases\":" << budget_cases
            << ",\"measurement_fixtures\":" << (measurements ? all.size() : 0) << ",\"sink\":" << sink << "}\n";
    } catch (const std::exception& error) {
        std::cerr << "RUNTIME PROBE FAILURE: " << error.what() << '\n'; return 1;
    }
}
