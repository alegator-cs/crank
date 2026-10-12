// Header-only research probe: no production runtime/admission replacement.
#include "predictive_scope_summary.hpp"
#include "regional_predictive.hpp"
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {
using namespace crank;
using crank::detail::PredictiveRegion;
using crank::detail::PredictiveScopeSummary;
using Bytes = std::bitset<256>;
using Clock = std::chrono::steady_clock;

void require(bool value, const std::string& reason) {
    if (!value) throw std::runtime_error(reason);
}
Bytes bytes(std::initializer_list<unsigned> values) {
    Bytes result;
    for (auto value : values) result.set(value);
    return result;
}
std::string hex(const Bytes& value) {
    std::ostringstream out;
    for (unsigned n = 0; n < 256; ++n)
        if (value[n]) out << std::hex << std::setw(2) << std::setfill('0') << n;
    return value.none() ? "empty" : out.str();
}
std::uint64_t elapsed(Clock::time_point begin) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count());
}

struct Builder {
    Ast ast;
    NodeId add(Kind kind, std::vector<NodeId> children = {}) {
        Node node;
        node.kind = kind;
        node.children = std::move(children);
        ast.nodes.push_back(std::move(node));
        return ast.nodes.size() - 1;
    }
    NodeId literal(unsigned value) {
        auto id = add(Kind::CharSet);
        ast.nodes[id].characters.set(value);
        return id;
    }
    NodeId capture(NodeId child, const char* name) {
        auto id = add(Kind::Capture, {child});
        ast.nodes[id].capture_id = ast.capture_names.size();
        ast.capture_names.emplace_back(name);
        ast.capture_nodes.push_back(id);
        return id;
    }
    NodeId call(NodeId target) {
        auto id = add(Kind::Call);
        ast.nodes[id].call_target = target;
        ast.has_calls = true;
        return id;
    }
    NodeId repeat(NodeId child, std::size_t lower, std::optional<std::size_t> upper) {
        auto id = add(Kind::Repeat, {child});
        ast.nodes[id].lower = lower;
        ast.nodes[id].upper = upper;
        return id;
    }
    Ast finish(NodeId root) {
        ast.root = root;
        ast.capture_nodes[0] = root;
        return std::move(ast);
    }
};

// Independent least-fixed-point nullable/FIRST computation. It does not call
// OccurrenceAnalysis and does not use Node::nullable, minimum_length or flags.
// These small fixtures have productive terminals/epsilon; an unproductive
// suffix would need an additional productivity fact and is outside this probe.
struct Facts {
    std::vector<OccurrenceBoundary> values;
    std::size_t passes = 0;
    explicit Facts(const Ast& ast) : values(ast.nodes.size()) {
        bool changed = true;
        while (changed) {
            require(++passes <= 4096, "independent facts did not converge");
            changed = false;
            auto previous = values; // Synchronous iteration, separate from production worklist.
            for (NodeId id = 0; id < ast.nodes.size(); ++id) {
                const auto& node = ast.nodes[id];
                OccurrenceBoundary b;
                switch (node.kind) {
                case Kind::Empty: case Kind::Define: b.nullable = true; break;
                case Kind::CharSet: b.first = node.characters; break;
                case Kind::Capture: b = previous.at(node.children.at(0)); break;
                case Kind::Call: b = previous.at(node.call_target); break;
                case Kind::Alt:
                    for (auto child : node.children) {
                        b.nullable = b.nullable || previous.at(child).nullable;
                        b.first |= previous.at(child).first;
                    }
                    break;
                case Kind::Concat:
                    b.nullable = true;
                    for (auto child : node.children) {
                        if (b.nullable) b.first |= previous.at(child).first;
                        b.nullable = b.nullable && previous.at(child).nullable;
                    }
                    break;
                case Kind::Repeat:
                    if (node.upper && !*node.upper) b.nullable = true;
                    else {
                        b.first = previous.at(node.children.at(0)).first;
                        b.nullable = !node.lower || previous.at(node.children.at(0)).nullable;
                    }
                    break;
                default: throw std::runtime_error("effectful fact fixture");
                }
                if (b != previous[id]) changed = true;
                values[id] = b;
            }
        }
    }
    const OccurrenceBoundary& boundary(NodeId id) const { return values.at(id); }
};

void validate(const Ast& ast) {
    require(ast.root < ast.nodes.size() && !ast.capture_names.empty(), "invalid root");
    for (const auto& n : ast.nodes) {
        require(n.kind != Kind::Reference && n.bind_count == no_id &&
                n.use_count == no_id && n.count_path == no_id, "effectful fixture");
        for (auto child : n.children) require(child < ast.nodes.size(), "invalid structural edge");
        if (n.kind == Kind::Capture)
            require(n.children.size() == 1 && n.capture_id != no_id &&
                    n.capture_id < ast.capture_names.size(), "invalid capture");
        if (n.kind == Kind::Call)
            require(n.call_target < ast.nodes.size() &&
                    (n.call_target == ast.root || ast.nodes[n.call_target].kind == Kind::Capture),
                    "invalid call target");
        if (n.kind == Kind::Repeat)
            require(n.children.size() == 1 && (!n.upper || *n.upper >= n.lower), "invalid repeat");
    }
}

enum class Tail { Star, Bounded3, Required2, Inactive0, Optional1, OptionalFixed2,
                  FixedOptional2, PrefixHazard, SuffixShield, InternalConflict,
                  OverlappingFirst, ZeroCycle };
struct Fixture {
    std::string name;
    Ast ast;
    Bytes expected_forbidden;
    bool expected_base = true;
    unsigned x = 0, y = 0;
};
Fixture fixture(std::string name, Tail tail, unsigned a, unsigned z, unsigned x,
                unsigned y, unsigned q, bool balanced_recursion = false) {
    Builder b;
    const auto empty = b.add(Kind::Empty);
    const auto ca = b.literal(a), cz = b.literal(z), cx = b.literal(x);
    const auto cy = b.literal(y), cq = b.literal(q);
    const auto A = b.capture(empty, "A");
    const auto recurse = b.call(A);
    auto spine = balanced_recursion
        ? b.add(Kind::Concat, {cq, recurse, ca, b.call(A)})
        : b.add(Kind::Concat, {ca, recurse});
    if (tail == Tail::ZeroCycle) spine = recurse;
    const auto base = tail == Tail::OverlappingFirst ? ca : empty;
    b.ast.nodes[A].children = {b.add(Kind::Alt, {spine, base})};
    const auto initial_A = b.call(A);
    const auto optional_x = b.capture(b.add(Kind::Alt, {cx, empty}), "X");
    NodeId remainder = no_id;
    Bytes forbidden;
    bool certified = true;
    switch (tail) {
    case Tail::Star: remainder = b.repeat(cy, 0, {}); forbidden = bytes({x, y}); break;
    case Tail::Bounded3: remainder = b.repeat(cy, 0, 3); forbidden = bytes({x, y}); break;
    case Tail::Required2: remainder = b.repeat(cy, 2, 2); break;
    case Tail::Inactive0: remainder = b.repeat(cy, 0, 0); forbidden = bytes({x}); break;
    case Tail::Optional1: remainder = b.repeat(cy, 0, 1); forbidden = bytes({x, y}); break;
    case Tail::OptionalFixed2:
        remainder = b.repeat(b.repeat(cy, 2, 2), 0, 1); forbidden = bytes({x, y}); break;
    case Tail::FixedOptional2:
        remainder = b.repeat(b.repeat(cy, 0, 1), 2, 2); certified = false; break;
    case Tail::PrefixHazard:
        remainder = b.add(Kind::Concat, {cq, optional_x}); forbidden = bytes({x}); break;
    case Tail::SuffixShield:
        remainder = b.add(Kind::Concat, {optional_x, cq}); break;
    case Tail::InternalConflict: remainder = cx; certified = false; break;
    case Tail::OverlappingFirst: case Tail::ZeroCycle:
        remainder = b.repeat(cy, 0, {}); certified = false; break;
    }
    const auto root = tail == Tail::PrefixHazard || tail == Tail::SuffixShield
        ? b.add(Kind::Concat, {initial_A, cz, remainder})
        : b.add(Kind::Concat, {initial_A, cz, optional_x, remainder});
    return {std::move(name), b.finish(root), forbidden, certified, x, y};
}
std::vector<Fixture> fixtures() {
    std::vector<Fixture> result;
    for (unsigned relabel = 0; relabel < 3; ++relabel) {
        // ASCII, NUL/high-bit, and disjoint high-bit relabelings test the full byte domain.
        const std::array<unsigned, 5> alphabet = relabel == 0
            ? std::array<unsigned, 5>{'a', 'z', 'x', 'y', 'q'}
            : relabel == 1 ? std::array<unsigned, 5>{0, 255, 128, 129, 254}
                          : std::array<unsigned, 5>{17, 18, 200, 201, 202};
        const std::vector<std::pair<const char*, Tail>> kinds{
            {"star", Tail::Star}, {"required2", Tail::Required2},
            {"inactive0", Tail::Inactive0}, {"optional1", Tail::Optional1},
            {"optional_fixed2", Tail::OptionalFixed2}, {"fixed_optional2_decline", Tail::FixedOptional2},
            {"prefix_hazard", Tail::PrefixHazard}, {"suffix_shield", Tail::SuffixShield},
            {"internal_conflict_decline", Tail::InternalConflict},
            {"overlapping_first_decline", Tail::OverlappingFirst}, {"zero_cycle_decline", Tail::ZeroCycle}};
        for (const auto& [name, kind] : kinds)
            result.push_back(fixture(std::string("continuity_") + name + "_" + std::to_string(relabel), kind,
                alphabet[0], alphabet[1], alphabet[2], alphabet[3], alphabet[4]));
    }
    // Independent NEW transfer family: L='[' L ']' L|epsilon;
    // E=L '|' Capture(u|epsilon) v{0,3}. Recursive call continuations differ
    // from the unary continuity grammar, and the terminal labels are disjoint.
    // ']' and '|' shield L's '[' hazard; outer nullable tails expose u/v.
    result.push_back(fixture("independent_balanced_bounded3_0", Tail::Bounded3,
        ']', '|', 'u', 'v', '[', true));
    result.push_back(fixture("independent_balanced_required2_0", Tail::Required2,
        ']', '|', 'u', 'v', '[', true));
    result.push_back(fixture("independent_balanced_inactive0_0", Tail::Inactive0,
        ']', '|', 'u', 'v', '[', true));
    return result;
}

bool same_payload(const PredictiveRegion& a, const PredictiveRegion& b) {
    if (a.targets != b.targets || a.choice_index != b.choice_index || a.choices.size() != b.choices.size())
        return false;
    for (std::size_t i = 0; i < a.choices.size(); ++i)
        if (a.choices[i].byte != b.choices[i].byte || a.choices[i].empty != b.choices[i].empty)
            return false;
    return true;
}
struct Totals {
    std::size_t fixtures = 0, constructors = 0, singleton = 0, subsets = 0;
    std::size_t comparisons = 0, unavailable = 0, payloads = 0, declines = 0, eof_checks = 0;
};
void csv_header(std::ostream& out) {
    out << "category,fixture,mode,eof,scope_bytes,expected,certified,limited,complete,payload_equal,"
           "production_steps,summary_steps,iterations,elapsed_ns,checksum,accepted_count,declined_count\n";
}
void row(std::ostream& out, const std::string& category, const std::string& name,
         const std::string& mode, bool eof, const Bytes& scope, int expected,
         int certified, int limited, int complete, int payload,
         std::size_t production_steps, std::size_t summary_steps,
         std::size_t iterations = 1, std::uint64_t ns = 0, std::uint64_t checksum = 0,
         std::size_t accepted = 0, std::size_t declined = 0) {
    out << category << ',' << name << ',' << mode << ',' << eof << ',' << hex(scope) << ','
        << expected << ',' << certified << ',' << limited << ',' << complete << ',' << payload
        << ',' << production_steps << ',' << summary_steps << ',' << iterations << ',' << ns << ','
        << checksum << ',' << accepted << ',' << declined << '\n';
}

void scopes(Fixture& f, std::ostream& out, Totals& totals) {
    validate(f.ast);
    Facts facts(f.ast);
    PredictiveRegion base(f.ast, facts, PredictiveRegion::Scope{f.ast.root, {}, true});
    ++totals.constructors;
    PredictiveScopeSummary summary(f.ast, facts, base);
    ++totals.fixtures;
    row(out, "base", f.name, "exact", true, {}, f.expected_base, base.certified,
        base.limit_reached, summary.complete, -1, base.analysis_steps, summary.analysis_steps);
    if (base.limit_reached) { ++totals.unavailable; return; }
    require(base.certified == f.expected_base, f.name + ": base certification");
    require(summary.complete == f.expected_base, f.name + ": summary availability");
    if (summary.complete) {
        require(summary.forbidden == f.expected_forbidden, f.name + ": analytically expected forbidden mask");
        require(summary.entry == base.entry && summary.root_child_count == base.root_child_count,
                f.name + ": source identity");
    } else ++totals.declines;
    const auto verify = [&](const Bytes& scope, bool eof, const char* category) {
        PredictiveRegion candidate(f.ast, facts, PredictiveRegion::Scope{f.ast.root, scope, eof});
        ++totals.constructors;
        const auto expected = f.expected_base && (scope & f.expected_forbidden).none();
        int payload = -1;
        if (candidate.limit_reached) ++totals.unavailable;
        else {
            ++totals.comparisons;
            require(candidate.certified == expected, f.name + ": independent scoped expectation");
            const auto prediction = summary.admission(scope);
            require(prediction.has_value() == summary.complete, f.name + ": unavailable prediction identity");
            if (prediction)
                require(*prediction == candidate.certified, f.name + ": mask/production disagreement");
            if (candidate.certified) {
                payload = same_payload(base, candidate);
                ++totals.payloads;
                require(payload == 1, f.name + ": exact/scoped execution payload differs");
            }
        }
        row(out, category, f.name, "scoped", eof, scope, expected, candidate.certified,
            candidate.limit_reached, summary.complete, payload, candidate.analysis_steps, summary.analysis_steps);
    };
    for (unsigned byte = 0; byte < 256; ++byte) for (bool eof : {false, true}) {
        verify(bytes({byte}), eof, "singleton"); ++totals.singleton;
    }
    Bytes full; full.set();
    const std::vector<Bytes> subsets{{}, full, ~f.expected_forbidden, f.expected_forbidden,
        bytes({f.x, f.y}), bytes({0, 127, 128, 255}), bytes({0, 1, 2, 253, 254, 255})};
    for (const auto& subset : subsets) for (bool eof : {false, true}) {
        verify(subset, eof, "subset"); ++totals.subsets;
    }
}

void contracts(std::ostream& out, Totals& totals) {
    using crank::detail::RegionalConsumerContract;
    for (bool eof : {false, true}) {
        RegionalConsumerContract c{bytes({0, 'x', 255}), eof};
        std::string subject;
        subject.push_back('\0'); subject.push_back('x'); subject.push_back(static_cast<char>(255));
        const std::vector<std::pair<std::size_t, bool>> checks{{0, true}, {1, true}, {2, true}, {3, eof}, {4, false}};
        for (auto [end, expected] : checks) {
            require(c.accepts(subject, end) == expected, "physical EOF/byte consumer contract"); ++totals.eof_checks;
        }
        require(c.accepts({}, 0) == eof, "empty subject EOF contract"); ++totals.eof_checks;
        RegionalConsumerContract exact{{}, true};
        require(!exact.accepts(subject, 1), "interior endpoint is not physical EOF"); ++totals.eof_checks;
        // Exact virtual boundary is represented by a bounded subject view. The
        // same underlying interior position is EOF only in that exact view.
        require(exact.accepts(std::string_view(subject).substr(0, 1), 1), "virtual exact boundary"); ++totals.eof_checks;
        row(out, "contract", "physical_and_virtual_eof", "accepts", eof, c.bytes,
            1, 1, 0, 1, -1, 0, 0, checks.size() + 3);
    }
}

void truncation_and_limits(std::ostream& out, Totals& totals) {
    auto f = fixture("trimmed_root", Tail::Star, 'a', 'z', 'x', 'y', 'q');
    Facts facts(f.ast);
    PredictiveRegion base(f.ast, facts, std::size_t{3});
    PredictiveScopeSummary summary(f.ast, facts, base);
    require(base.certified && summary.complete && summary.root_child_count == 3,
            "fixed trimmed-root identity");
    require(summary.forbidden == bytes({'x'}), "trimmed root has different forbidden mask");
    crank::detail::PredictiveScopeSummaryCache<Facts> cache(f.ast, facts);
    const auto* trimmed = cache.summary(base);
    PredictiveRegion full(f.ast, facts);
    const auto* full_summary = cache.summary(full);
    PredictiveRegion safe_scoped(f.ast, facts, PredictiveRegion::Scope{f.ast.root, bytes({'q'}), false});
    totals.constructors += 3; // Trimmed base, full base, and safe scoped cache-key reference.
    const auto* scoped_summary = cache.summary(safe_scoped);
    require(trimmed && full_summary && full_summary == scoped_summary && trimmed != full_summary,
            "cache fixed root count and exact/scoped key identities");
    require(trimmed->forbidden == bytes({'x'}) && full_summary->forbidden == bytes({'x', 'y'}) && cache.builds == 2,
            "cache preserves per-source mask and avoids duplicate summary work");
    const auto cache_work = cache.analysis_steps;
    require(cache.summary(full) == full_summary && cache.builds == 2 && cache.analysis_steps == cache_work,
            "warm cache must not add preparation work");
    row(out, "cache", "fixed_source_keys", "exact_scoped_and_trimmed", false, {},
        1, 1, 0, 1, -1, 0, cache.analysis_steps, cache.builds);
    auto clone = f.ast;
    clone.nodes[clone.root].children.resize(3);
    Facts clone_facts(clone);
    for (unsigned byte = 0; byte < 256; ++byte) for (bool eof : {false, true}) {
        auto scope = bytes({byte});
        PredictiveRegion candidate(clone, clone_facts, PredictiveRegion::Scope{clone.root, scope, eof});
        ++totals.constructors; ++totals.singleton;
        if (candidate.limit_reached) ++totals.unavailable;
        else {
            ++totals.comparisons;
            const auto prediction = summary.admission(scope);
            require(prediction.has_value() && *prediction == candidate.certified, "trimmed-root reference disagreement");
            if (candidate.certified) {
                require(same_payload(base, candidate), "trimmed-root payload disagreement"); ++totals.payloads;
            }
        }
        row(out, "singleton", f.name, "fixed_root_count_3", eof, scope, byte != 'x',
            candidate.certified, candidate.limit_reached, summary.complete,
            candidate.certified ? 1 : -1, candidate.analysis_steps, summary.analysis_steps);
    }
    auto oversized = f.ast;
    oversized.nodes.resize(4097);
    Facts large_facts(oversized);
    PredictiveRegion limited(oversized, large_facts);
    PredictiveScopeSummary unavailable(oversized, large_facts, limited);
    ++totals.constructors; ++totals.unavailable;
    require(limited.limit_reached && !limited.certified && !unavailable.complete,
            "node-cap failure must remain unavailable");
    require(!unavailable.admission({}).has_value(), "incomplete summary returned semantic admission");
    row(out, "resource", "4097_nodes", "unavailable", true, {}, -1,
        limited.certified, limited.limit_reached, unavailable.complete, -1,
        limited.analysis_steps, unavailable.analysis_steps);
}

// Equal numbers of safe and unsafe scopes, matched cardinalities and EOF bits.
// The deterministic generator samples a broad byte domain, including NUL/high
// bits. It is a preparation workload; it is not a distribution of subjects.
struct Seed { Bytes scope; bool eof; bool safe; };
std::vector<Seed> balanced(const Bytes& forbidden, std::size_t cardinality) {
    std::vector<unsigned> safe, unsafe;
    for (unsigned byte = 0; byte < 256; ++byte)
        (forbidden[byte] ? unsafe : safe).push_back(byte);
    require(!unsafe.empty() && safe.size() >= cardinality, "balanced scope fixture");
    std::vector<Seed> seeds;
    for (unsigned n = 0; n < 32; ++n) {
        Bytes good;
        // Stride 31 is coprime to the safe-domain sizes of these fixtures.
        for (std::size_t k = 0; good.count() < cardinality; ++k)
            good.set(safe[(n * 17 + k * 31) % safe.size()]);
        auto bad = good;
        for (unsigned byte = 0; byte < 256; ++byte) if (bad[byte]) { bad.reset(byte); break; }
        bad.set(unsafe[n % unsafe.size()]);
        seeds.push_back({good, static_cast<bool>(n & 1), true});
        seeds.push_back({bad, static_cast<bool>(n & 1), false});
    }
    return seeds;
}

void timing(const std::vector<Fixture>& all, std::ostream& out,
            std::size_t iterations, std::size_t trials) {
    for (const auto& f : all) {
        if (!f.expected_base || f.expected_forbidden.none() || f.name.back() != '0') continue;
        Facts facts(f.ast); // Facts are held fixed and excluded from preparation timing.
        for (std::size_t cardinality : {1, 2, 4, 8, 16}) {
            const auto seeds = balanced(f.expected_forbidden, cardinality);
            for (const bool exact_first : {true, false}) for (std::size_t trial = 0; trial < trials; ++trial) {
                const auto order = std::string(exact_first ? "exact_first" : "scoped_first") +
                    "_k" + std::to_string(cardinality) + "_trial" + std::to_string(trial);
                std::uint64_t checksum = 0;
                std::size_t production_steps = 0;
                auto start = Clock::now();
                // Cold first pair, both warm-up orders. There is no source or
                // constructor memo in PredictiveRegion itself.
                if (exact_first) {
                    PredictiveRegion exact(f.ast, facts, PredictiveRegion::Scope{f.ast.root, {}, true});
                    PredictiveRegion scoped(f.ast, facts, PredictiveRegion::Scope{f.ast.root, seeds[0].scope, seeds[0].eof});
                    require(exact.certified && scoped.certified, "cold safe preparation");
                    checksum += exact.targets.size() + scoped.targets.size();
                    production_steps += exact.analysis_steps + scoped.analysis_steps;
                } else {
                    PredictiveRegion scoped(f.ast, facts, PredictiveRegion::Scope{f.ast.root, seeds[0].scope, seeds[0].eof});
                    PredictiveRegion exact(f.ast, facts, PredictiveRegion::Scope{f.ast.root, {}, true});
                    require(exact.certified && scoped.certified, "cold safe preparation");
                    checksum += exact.targets.size() + scoped.targets.size();
                    production_steps += exact.analysis_steps + scoped.analysis_steps;
                }
                row(out, "microbenchmark_cold", f.name, order + "_production_pair", false,
                    seeds[0].scope, 1, 1, 0, -1, -1, production_steps, 0, 2, elapsed(start), checksum);
                start = Clock::now();
                PredictiveRegion base(f.ast, facts, PredictiveRegion::Scope{f.ast.root, {}, true});
                const auto base_ns = elapsed(start);
                start = Clock::now();
                PredictiveScopeSummary summary(f.ast, facts, base);
                const auto summary_ns = elapsed(start);
                require(base.certified && summary.complete, "shadow preparation availability");
                crank::detail::PredictiveScopeSummaryCache<Facts> cache(f.ast, facts);
                const auto* cached_summary = cache.summary(base);
                require(cached_summary && cache.builds == 1, "shadow cache preparation");
                row(out, "microbenchmark_cold", f.name, order + "_exact_base", true, {},
                    1, 1, 0, -1, -1, base.analysis_steps, 0, 1, base_ns, base.targets.size());
                row(out, "microbenchmark_cold", f.name, order + "_additional_summary", true, {},
                    1, 1, 0, 1, -1, 0, summary.analysis_steps, 1, summary_ns, summary.forbidden.count());
                // Compare completed production semantic outcomes before timing
                // warm lookup; resource failures stay separate and abort timing.
                for (const auto& seed : seeds) {
                    PredictiveRegion reference(f.ast, facts, PredictiveRegion::Scope{f.ast.root, seed.scope, seed.eof});
                    require(!reference.limit_reached, "timing production scope unavailable");
                    const auto prediction = summary.admission(seed.scope);
                    require(reference.certified == seed.safe && prediction.has_value() && *prediction == seed.safe,
                            "balanced distribution semantic mismatch");
                }
                const auto production_loop = [&] {
                    std::uint64_t sum = 0; std::size_t steps = 0, certified = 0, declined = 0;
                    auto begin = Clock::now();
                    for (std::size_t n = 0; n < iterations; ++n) {
                        const auto& seed = seeds[n % seeds.size()];
                        PredictiveRegion reference(f.ast, facts, PredictiveRegion::Scope{f.ast.root, seed.scope, seed.eof});
                        require(!reference.limit_reached, "warm production scope unavailable");
                        sum += reference.certified ? reference.targets.size() + reference.choices.size() : 1;
                        steps += reference.analysis_steps;
                        certified += reference.certified; declined += !reference.certified;
                    }
                    const auto ns = elapsed(begin);
                    row(out, "microbenchmark_warm", f.name, order + "_repeated_production",
                        false, {}, -1, -1, 0, -1, -1, steps, 0, iterations, ns, sum, certified, declined);
                };
                const auto mask_loop = [&] {
                    std::uint64_t sum = 0; std::size_t accepted = 0;
                    auto begin = Clock::now();
                    for (std::size_t n = 0; n < iterations; ++n) {
                        const auto& seed = seeds[n % seeds.size()];
                        const auto prediction = summary.admission(seed.scope);
                        const bool admitted = prediction.has_value() && *prediction;
                        accepted += admitted; sum += admitted ? base.targets.size() + base.choices.size() : 1;
                    }
                    const auto ns = elapsed(begin);
                    row(out, "microbenchmark_warm", f.name, order + "_shadow_mask_lookup",
                        false, {}, -1, -1, 0, 1, -1, 0, 0, iterations, ns, sum, accepted, iterations - accepted);
                };
                const auto cached_loop = [&] {
                    std::uint64_t sum = 0; std::size_t accepted = 0;
                    auto begin = Clock::now();
                    for (std::size_t n = 0; n < iterations; ++n) {
                        const auto& seed = seeds[n % seeds.size()];
                        const auto* found = cache.summary(base);
                        const auto prediction = found ? found->admission(seed.scope) : std::optional<bool>{};
                        const bool admitted = prediction.has_value() && *prediction;
                        accepted += admitted; sum += admitted ? base.targets.size() + base.choices.size() : 1;
                    }
                    const auto ns = elapsed(begin);
                    require(cache.builds == 1, "warm lookup repeated preparation");
                    row(out, "microbenchmark_warm", f.name, order + "_cached_shadow_lookup",
                        false, {}, -1, -1, 0, 1, -1, 0, 0, iterations, ns, sum, accepted, iterations - accepted);
                };
                // Balance measured loop order as well as first constructor order.
                if (exact_first) { production_loop(); mask_loop(); }
                else { mask_loop(); production_loop(); }
                cached_loop();
            }
        }
    }
}
} // namespace

int main(int argc, char** argv) {
    try {
        std::string filename;
        bool benchmark = false;
        std::size_t iterations = 1024, trials = 5;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--timing") benchmark = true;
            else if (arg == "--out" && i + 1 < argc) filename = argv[++i];
            else if (arg == "--iterations" && i + 1 < argc) iterations = std::stoull(argv[++i]);
            else if (arg == "--trials" && i + 1 < argc) trials = std::stoull(argv[++i]);
            else throw std::runtime_error("usage: transfer_scope [--out FILE] [--timing] [--iterations N] [--trials N]");
        }
        require(iterations > 0 && iterations % 64 == 0, "iterations must be a positive multiple of 64 for exact balance");
        require(trials > 0, "trials must be positive");
        std::ofstream file;
        if (!filename.empty()) { file.open(filename); require(file.good(), "cannot open CSV output"); }
        std::ostream& output = filename.empty() ? std::cout : file;
        csv_header(output);
        auto all = fixtures();
        Totals totals;
        for (auto& f : all) scopes(f, output, totals);
        contracts(output, totals);
        truncation_and_limits(output, totals);
        if (benchmark) timing(all, output, iterations, trials);
        require(output.good(), "CSV output failed");
        std::cerr << "PASS fixtures=" << totals.fixtures << " constructors=" << totals.constructors
            << " singleton=" << totals.singleton << " subsets=" << totals.subsets
            << " completed_comparisons=" << totals.comparisons << " unavailable=" << totals.unavailable
            << " payload_equal=" << totals.payloads << " base_declines=" << totals.declines
            << " eof_checks=" << totals.eof_checks << '\n';
        std::cerr << "HEADER-ONLY PREPARATION MICROBENCHMARK; NO MATCHER EXECUTION OR END-TO-END GAIN CLAIM\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
