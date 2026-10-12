// Source-only fault/authority probe until the coordinated compiler slot is granted.
// Link with existing regex_ast.cpp and occurrence_analysis.cpp; no matcher needed.
#include "predictive_payload_view.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#ifdef _WIN32
#include <malloc.h>
#endif

namespace allocation_probe {
struct Trace {
    static constexpr std::size_t capacity = 512;
    std::array<std::size_t, capacity> sizes{};
    std::size_t count = 0;
    bool overflow = false;
};
struct Plan {
    Trace trace;
    const Trace* expected = nullptr;
    std::size_t fail_at = std::numeric_limits<std::size_t>::max();
    bool fired = false, prefix_mismatch = false;
};
thread_local Plan* active = nullptr;
void observe(std::size_t size) {
    if (!active) return;
    auto& p = *active;
    const auto ordinal = p.trace.count++;
    if (ordinal < Trace::capacity) p.trace.sizes[ordinal] = size;
    else p.trace.overflow = true;
    // A calibrated semantic phase is a size-labelled prefix in this concrete
    // implementation, never an assertion that kth allocation means the same
    // thing in a different representation or compiler/library.
    if (!p.fired && p.expected && ordinal <= p.fail_at &&
        (ordinal >= p.expected->count || p.expected->sizes[ordinal] != size))
        p.prefix_mismatch = true;
    if (!p.fired && ordinal == p.fail_at) {
        p.fired = true;
        throw std::bad_alloc{}; // One-shot rejection; cleanup may allocate normally.
    }
}
struct Guard {
    Plan* previous;
    explicit Guard(Plan& p) : previous(active) { active = &p; }
    ~Guard() { active = previous; }
};
void* ordinary(std::size_t size) {
    observe(size);
    if (auto* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc{};
}
void* aligned(std::size_t size, std::size_t alignment) {
    observe(size);
#ifdef _WIN32
    if (auto* p = _aligned_malloc(size ? size : 1, alignment)) return p;
#else
    const auto effective = size ? size : 1;
    if (effective > std::numeric_limits<std::size_t>::max() - (alignment - 1)) throw std::bad_alloc{};
    const auto rounded = (effective + alignment - 1) / alignment * alignment;
    if (auto* p = std::aligned_alloc(alignment, rounded)) return p;
#endif
    throw std::bad_alloc{};
}
void aligned_free(void* p) noexcept {
#ifdef _WIN32
    _aligned_free(p);
#else
    std::free(p);
#endif
}
} // namespace allocation_probe

// These replacements are confined to this standalone executable. The fixed
// recorder, hook and exception objects perform no user heap allocation.
void* operator new(std::size_t n) { return allocation_probe::ordinary(n); }
void* operator new[](std::size_t n) { return allocation_probe::ordinary(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void* operator new(std::size_t n, std::align_val_t a) { return allocation_probe::aligned(n, static_cast<std::size_t>(a)); }
void* operator new[](std::size_t n, std::align_val_t a) { return allocation_probe::aligned(n, static_cast<std::size_t>(a)); }
void operator delete(void* p, std::align_val_t) noexcept { allocation_probe::aligned_free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { allocation_probe::aligned_free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { allocation_probe::aligned_free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { allocation_probe::aligned_free(p); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { try { return ::operator new(n); } catch (...) { return nullptr; } }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { try { return ::operator new[](n); } catch (...) { return nullptr; } }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    try { return ::operator new(n, a); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    try { return ::operator new[](n, a); } catch (...) { return nullptr; }
}
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { allocation_probe::aligned_free(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { allocation_probe::aligned_free(p); }

namespace {
using namespace crank;
namespace d = crank::detail;
namespace ap = allocation_probe;
using Statistics = d::RegionalPreparationStatistics;
using Record = std::shared_ptr<const d::PredictiveRegion>;
std::size_t checks = 0, fault_cases = 0, boundary_cases = 0, warm_queries = 0;
std::string context;
void require(bool value, const char* reason) {
    ++checks;
    if (!value) throw std::runtime_error(context + ": " + reason);
}

struct Fixture {
    Ast ast;
    NodeId source = no_id;
    std::vector<NodeId> aliases;
};
struct Builder {
    Ast ast;
    NodeId add(Kind kind, std::vector<NodeId> children = {}) {
        Node n; n.kind = kind; n.children = std::move(children);
        ast.nodes.push_back(std::move(n)); return ast.nodes.size() - 1;
    }
    NodeId byte(unsigned c) {
        const auto id = add(Kind::CharSet); ast.nodes[id].characters.set(c); return id;
    }
    NodeId capture(NodeId child) {
        const auto id = add(Kind::Capture, {child});
        ast.nodes[id].capture_id = ast.capture_names.size();
        ast.capture_names.emplace_back("S"); ast.capture_nodes.push_back(id); return id;
    }
    NodeId call(NodeId capture) {
        const auto id = add(Kind::Call); ast.nodes[id].call_target = capture;
        ast.has_calls = true; return id;
    }
};
Fixture fixture(std::size_t n = 23, std::size_t aliases = 2) {
    Builder b;
    const auto empty = b.add(Kind::Empty);
    const auto a = b.byte('a'), z = b.byte('b'), separator = b.byte(';');
    const auto source = b.capture(empty);
    const auto recur = b.call(source);
    const auto spine = b.add(Kind::Concat, {a, recur, z});
    const auto choice = b.add(Kind::Alt, {spine, empty});
    b.ast.nodes[source].children = {choice};
    std::vector<NodeId> entries, branches;
    for (std::size_t i = 0; i < aliases; ++i) {
        const auto call = b.call(source); entries.push_back(call);
        // The required suffix supplies a byte-only, safe closed consumer.
        branches.push_back(b.add(Kind::Concat, {call, separator}));
    }
    b.ast.root = b.add(Kind::Alt, std::move(branches));
    b.ast.capture_nodes[0] = b.ast.root;
    require(b.ast.nodes.size() <= n, "fixture padding too small");
    b.ast.nodes.resize(n); // Valid unreachable Empty nodes enlarge full arrays.
    for (const auto& node : b.ast.nodes) {
        require(node.bind_count == no_id && node.use_count == no_id && node.count_path == no_id,
                "fixture has effects");
        for (auto child : node.children) require(child < n, "invalid structural edge");
        if (node.kind == Kind::Capture)
            require(node.children.size() == 1 && node.capture_id < b.ast.capture_names.size(), "invalid Capture");
        if (node.kind == Kind::Call)
            require(node.call_target < n && b.ast.nodes[node.call_target].kind == Kind::Capture,
                    "Call must target a valid named Capture");
    }
    return {std::move(b.ast), source, std::move(entries)};
}
std::vector<NodeId> identity(const Ast& ast) {
    std::vector<NodeId> result(ast.nodes.size());
    for (NodeId id = 0; id < result.size(); ++id) result[id] = id;
    return result;
}
bool same_statistics(const Statistics& a, const Statistics& b) {
    return a.follow_steps == b.follow_steps && a.predictive_builds == b.predictive_builds &&
        a.spine_builds == b.spine_builds && a.retained_certificates == b.retained_certificates &&
        a.retained_bytes == b.retained_bytes && a.analysis_steps == b.analysis_steps &&
        a.declined == b.declined && a.limit_declines == b.limit_declines &&
        a.allocation_declines == b.allocation_declines;
}
std::size_t reservation(const d::PredictiveRegion& p) {
    return sizeof(d::PredictiveRegion) + p.targets.capacity() * sizeof(NodeId) +
        p.choice_index.capacity() * sizeof(std::size_t) +
        p.choices.capacity() * sizeof(d::PredictiveRegion::Choice);
}
bool same_tables(const d::PredictiveRegion& a, const d::PredictiveRegion& b) {
    if (a.entry != b.entry || a.root_child_count != b.root_child_count ||
        a.targets != b.targets || a.choice_index != b.choice_index || a.choices.size() != b.choices.size())
        return false;
    for (std::size_t i = 0; i < a.choices.size(); ++i)
        if (a.choices[i].byte != b.choices[i].byte || a.choices[i].empty != b.choices[i].empty) return false;
    return true;
}

enum class Mode { Native, ViewDisabled, ViewEnabled };
const char* mode_name(Mode mode) {
    return mode == Mode::Native ? "native_cache" : mode == Mode::ViewDisabled ? "view_disabled" : "view_enabled";
}
struct Slot {
    Record record, payload;
    bool exact = false;
    d::RegionalConsumerContract consumer;
    const d::RegionalPredictiveCache* source_owner = nullptr;
    explicit operator bool() const { return bool(record); }
};
struct Owner {
    d::RegionalPredictiveCache& cache;
    Mode mode;
    d::PredictivePayloadViewOwner views;
    Owner(d::RegionalPredictiveCache& c, Mode m) : cache(c), mode(m), views(c, m == Mode::ViewEnabled) {}
    Slot certificate(NodeId entry, bool exact) {
        if (mode == Mode::Native) {
            auto record = cache.certificate(entry, exact);
            if (!record) return {};
            return {record, record, exact, exact ? d::RegionalConsumerContract{{}, true} : *cache.contract(entry), &cache};
        }
        auto view = views.certificate(entry, exact);
        if (!view) return {};
        return {view.record(), view.payload(), view.exact(), view.consumer(), view.source_owner()};
    }
};

struct Direct {
    std::optional<d::PredictiveRegion> record;
    ap::Trace trace;
};
Direct direct(const Fixture& f, const OccurrenceAnalysis& analysis, NodeId entry,
              const d::RegionalConsumerContract& c, bool exact,
              const ap::Trace* expected = nullptr, std::size_t fail_at = no_id) {
    Direct out;
    ap::Plan plan; plan.expected = expected; plan.fail_at = fail_at;
    {
        ap::Guard guard(plan);
        out.record.emplace(f.ast, analysis,
            d::PredictiveRegion::Scope{entry, exact ? std::bitset<256>{} : c.bytes, exact || c.eof});
    }
    require(!plan.trace.overflow && !plan.prefix_mismatch, "direct phase allocation prefix changed");
    if (fail_at != no_id) require(plan.fired, "direct phase did not inject failure");
    out.trace = plan.trace; return out;
}
struct Calibration {
    Direct direct_record;
    ap::Trace native_trace;
    std::array<std::size_t, 4> positions{}; // owner, FOLLOW, table-index, targets
};
Calibration calibrate(const Fixture& f, const OccurrenceAnalysis& analysis,
                      const std::vector<NodeId>& controls, NodeId entry, bool exact) {
    d::RegionalPredictiveCache cache(f.ast, analysis, controls, controls);
    const auto* c = cache.contract(entry);
    require(c && c->bytes.any() && !c->eof, "calibration needs a safe byte-only consumer");
    const auto contract = *c;
    Calibration out;
    out.direct_record = direct(f, analysis, entry, contract, exact);
    require(out.direct_record.record->certified && !out.direct_record.record->limit_reached,
            "direct calibration certificate unavailable");
    ap::Plan plan;
    Record prepared;
    {
        ap::Guard guard(plan); prepared = cache.certificate(entry, exact);
    }
    require(prepared && !plan.trace.overflow, "native calibration unavailable");
    out.native_trace = plan.trace;
    require(out.native_trace.count == out.direct_record.trace.count + 1,
            "native allocation stream is not owner plus direct constructor");
    require(out.native_trace.sizes[0] >= sizeof(d::PredictiveRegion), "first allocation is not owner storage");
    for (std::size_t i = 0; i < out.direct_record.trace.count; ++i)
        require(out.native_trace.sizes[i + 1] == out.direct_record.trace.sizes[i],
                "actual cache/direct constructor allocation streams differ");
    const auto follow_size = f.ast.nodes.size() * sizeof(std::bitset<256>);
    const auto index_size = f.ast.nodes.size() * sizeof(NodeId);
    std::size_t follow = no_id, table = no_id, target = no_id;
    for (std::size_t i = 0; i < out.direct_record.trace.count; ++i) {
        const auto size = out.direct_record.trace.sizes[i];
        if (size == follow_size) {
            require(follow == no_id, "FOLLOW size is ambiguous on this library/fixture"); follow = i;
        }
        if (follow != no_id && i > follow && size == index_size) {
            if (table == no_id) table = i;
            else { require(target == no_id, "table/target size is ambiguous"); target = i; }
        }
    }
    require(follow != no_id && table != no_id && target != no_id && follow < table && table < target,
            "could not calibrate source-labelled FOLLOW/table-index/target phases");
    out.positions = {0, follow + 1, table + 1, target + 1};
    // Verify work at each internal label via the actual direct constructor's
    // catch, not by reading a partially constructed object from operator new.
    for (unsigned phase = 1; phase < 4; ++phase) {
        auto failed = direct(f, analysis, entry, contract, exact,
            &out.direct_record.trace, out.positions[phase] - 1);
        require(!failed.record->certified && failed.record->limit_reached &&
                failed.record->targets.empty() && failed.record->choice_index.empty() && failed.record->choices.empty(),
                "internal catch did not clear executable payload and report limit");
        require((phase == 1) == (failed.record->analysis_steps == 0),
                "FOLLOW/late table phase partial-work distinction was not established");
    }
    return out;
}

void header(std::ostream& out) {
    out << "category,mode,failing_slot,order,phase,request,node,available,shared,allocation_count,"
           "injected_ordinal,injected_size,predictive_builds,spine_builds,analysis_steps,follow_steps,"
           "retained_certificates,retained_bytes,declined,limit_declines,allocation_declines\n";
}
void row(std::ostream& out, const char* category, Mode mode, bool failing_exact, bool failure_first,
         const char* phase, const char* request, NodeId entry, const Slot& slot, const Statistics& s,
         std::size_t allocation_count = 0, std::size_t injected = no_id, std::size_t injected_size = 0) {
    out << category << ',' << mode_name(mode) << ',' << (failing_exact ? "exact" : "scoped") << ','
        << (failure_first ? "failure_first" : "sibling_first") << ',' << phase << ',' << request << ','
        << entry << ',' << bool(slot) << ',' << (slot && slot.record.get() != slot.payload.get()) << ','
        << allocation_count << ',';
    if (injected != no_id) out << injected;
    else out << -1;
    out << ',' << injected_size << ',' << s.predictive_builds << ',' << s.spine_builds << ','
        << s.analysis_steps << ',' << s.follow_steps << ',' << s.retained_certificates << ',' << s.retained_bytes
        << ',' << s.declined << ',' << s.limit_declines << ',' << s.allocation_declines << '\n';
}
void identity_checks(const Slot& slot, d::RegionalPredictiveCache& cache, NodeId entry, bool exact) {
    require(bool(slot) && slot.payload && slot.record->certified && !slot.record->limit_reached, "invalid admitted slot");
    require(slot.source_owner == &cache && slot.exact == exact && slot.record->entry == entry &&
            slot.record->root_child_count == no_id, "view source/root/observer identity mismatch");
    require(slot.consumer == (exact ? d::RegionalConsumerContract{{}, true} : *cache.contract(entry)),
            "view EOF/closed-consumer identity mismatch");
    require(same_tables(*slot.record, *slot.payload), "read backing is not equal to original tables");
}

void fault_traces(std::ostream& out) {
    auto f = fixture();
    OccurrenceAnalysis analysis(f.ast);
    const auto controls = identity(f.ast);
    const auto entry = f.source;
    context = "phase calibration";
    const auto exact = calibrate(f, analysis, controls, entry, true);
    const auto scoped = calibrate(f, analysis, controls, entry, false);
    const std::array<const char*, 4> phase_names{"outer_make_shared", "internal_follow_zero_work", "internal_table_index", "internal_targets"};
    for (Mode mode : {Mode::Native, Mode::ViewDisabled, Mode::ViewEnabled})
        for (bool failing_exact : {true, false}) for (bool failure_first : {true, false})
            for (unsigned phase = 0; phase < phase_names.size(); ++phase) {
        context = std::string(mode_name(mode)) + " " + phase_names[phase] +
            (failing_exact ? " exact-fail" : " scoped-fail") + (failure_first ? " fail-first" : " sibling-first");
        ++fault_cases;
        d::RegionalPredictiveCache cache(f.ast, analysis, controls, controls);
        Owner owner(cache, mode);
        const auto* cp = cache.contract(entry); require(cp != nullptr, "cache initialization failed");
        const auto contract = *cp;
        const auto initial = cache.statistics();
        require(!cache.limit_reached() && initial.follow_steps > 0 && !initial.predictive_builds,
                "injection did not start after clean initialization");
        const auto& calibration = failing_exact ? exact : scoped;
        const auto& sibling_reference = failing_exact ? scoped.direct_record : exact.direct_record;
        std::size_t partial_work = 0;
        if (phase) {
            auto replay = direct(f, analysis, entry, contract, failing_exact,
                &calibration.direct_record.trace, calibration.positions[phase] - 1);
            partial_work = replay.record->analysis_steps;
        }
        Slot sibling;
        if (!failure_first) {
            sibling = owner.certificate(entry, !failing_exact);
            identity_checks(sibling, cache, entry, !failing_exact);
        }
        const auto before = cache.statistics();
        ap::Plan plan;
        plan.expected = &calibration.native_trace;
        plan.fail_at = calibration.positions[phase];
        Slot failed;
        { ap::Guard guard(plan); failed = owner.certificate(entry, failing_exact); }
        require(plan.fired && !plan.prefix_mismatch && !plan.trace.overflow, "native phase injection was not calibrated");
        require(!failed && !failed.payload, "failed observer acquired authority from a sibling");
        const auto after = cache.statistics();
        require(after.predictive_builds == before.predictive_builds + 1 && after.spine_builds == before.spine_builds,
                "failed attempt did not charge exactly one predictive build");
        require(after.analysis_steps == before.analysis_steps + partial_work, "partial constructor work was lost or invented");
        require(after.declined == before.declined + 1 && after.allocation_declines == before.allocation_declines + (phase == 0) &&
                after.limit_declines == before.limit_declines + (phase != 0), "outer/internal failure counters were conflated");
        require(after.retained_bytes == before.retained_bytes && after.retained_certificates == before.retained_certificates &&
                after.follow_steps == before.follow_steps, "failed constructor changed retention/initial FOLLOW accounting");
        row(out, "fault", mode, failing_exact, failure_first, phase_names[phase], "injected", entry, failed, after,
            plan.trace.count, plan.fail_at, calibration.native_trace.sizes[plan.fail_at]);
        if (failure_first) {
            sibling = owner.certificate(entry, !failing_exact);
            identity_checks(sibling, cache, entry, !failing_exact);
        }
        const auto complete = cache.statistics();
        require(complete.predictive_builds == 2 && complete.retained_certificates == 1 && complete.declined == 1,
                "independent sibling preparation was suppressed or duplicated");
        require(complete.analysis_steps == partial_work + sibling_reference.record->analysis_steps &&
                complete.retained_bytes == reservation(*sibling_reference.record), "sibling preparation charge differs from reference");
        row(out, "fault", mode, failing_exact, failure_first, phase_names[phase], "sibling_success", entry, sibling, complete);
        for (unsigned retry = 0; retry < 3; ++retry) {
            ap::Plan warm; warm.fail_at = 0; // Any allocation would make the warm assertion fail.
            Slot again, positive;
            { ap::Guard guard(warm); again = owner.certificate(entry, failing_exact); positive = owner.certificate(entry, !failing_exact); }
            ++warm_queries;
            require(!again && positive.record == sibling.record && positive.payload == sibling.payload,
                    "terminal tombstone or positive sibling changed on warm requery");
            require(warm.trace.count == 0 && !warm.fired, "terminal slot requery performed an allocation");
            require(same_statistics(cache.statistics(), complete), "terminal requery changed production counters");
        }
        row(out, "fault", mode, failing_exact, failure_first, phase_names[phase], "tombstone_requery", entry, {}, cache.statistics());
    }
}

void dual_success(std::ostream& out) {
    auto f = fixture(); OccurrenceAnalysis analysis(f.ast); const auto controls = identity(f.ast);
    for (Mode mode : {Mode::Native, Mode::ViewDisabled, Mode::ViewEnabled}) for (bool exact_first : {true, false}) {
        context = std::string(mode_name(mode)) + " dual success";
        d::RegionalPredictiveCache cache(f.ast, analysis, controls, controls); Owner owner(cache, mode);
        require(cache.contract(f.source) != nullptr, "dual-success initialization");
        const auto first = owner.certificate(f.source, exact_first);
        const auto second = owner.certificate(f.source, !exact_first);
        identity_checks(first, cache, f.source, exact_first);
        identity_checks(second, cache, f.source, !exact_first);
        require(first.record.get() != second.record.get() && same_tables(*first.record, *second.record),
                "original records were aliased or tables unequal");
        require((first.payload == second.payload) == (mode == Mode::ViewEnabled), "opt-in table-read identity incorrect");
        const auto s = cache.statistics();
        require(s.predictive_builds == 2 && s.retained_certificates == 2 && s.retained_bytes ==
                reservation(*first.record) + reservation(*second.record) && !s.declined,
                "shared read discounted original objects or reservations");
        row(out, "dual_success", mode, !exact_first, false, "none", "first", f.source, first, s);
        row(out, "dual_success", mode, !exact_first, false, "none", "second", f.source, second, s);
    }
}

void actual_retention_boundary(std::ostream& out) {
    // Actual production limit from pinned regional_predictive.hpp, not a
    // reduced-policy seam. Both gates and retention run inside the real class.
    constexpr std::size_t cap = 8 * 1024 * 1024;
    auto f = fixture(4096, 130);
    OccurrenceAnalysis analysis(f.ast);
    const auto controls = identity(f.ast);
    const auto entry = f.aliases.back();
    d::RegionalPredictiveCache calibration(f.ast, analysis, controls, controls);
    const auto* c = calibration.contract(entry); require(c && c->bytes.any() && !c->eof, "boundary safe consumer missing");
    const auto exact = direct(f, analysis, entry, *c, true);
    const auto scoped = direct(f, analysis, entry, *c, false);
    require(exact.record->certified && scoped.record->certified && same_tables(*exact.record, *scoped.record),
            "boundary independent constructors not successful/equal");
    const auto b = reservation(*exact.record);
    require(b == reservation(*scoped.record) && b < cap && cap / b + 1 < 128,
            "boundary fixture cannot reach retention before preparation gate");
    const auto prefills = cap / b - 1;
    require(prefills < f.aliases.size() - 1, "boundary fixture has too few distinct slots");
    for (Mode mode : {Mode::Native, Mode::ViewDisabled, Mode::ViewEnabled}) for (bool exact_first : {true, false}) {
        context = std::string(mode_name(mode)) + " actual 8MiB retention boundary";
        ++boundary_cases;
        d::RegionalPredictiveCache cache(f.ast, analysis, controls, controls); Owner owner(cache, mode);
        require(cache.contract(entry) != nullptr && !cache.limit_reached(), "boundary cache initialization");
        std::size_t work = 0;
        for (std::size_t i = 0; i < prefills; ++i) {
            const auto fill = owner.certificate(f.aliases[i], true);
            require(bool(fill) && reservation(*fill.record) == b, "prefill declined or changed reservation");
            work += fill.record->analysis_steps;
        }
        const auto before = cache.statistics();
        const auto remaining = cap - before.retained_bytes;
        require(before.retained_bytes == prefills * b && before.predictive_builds == prefills &&
                before.retained_certificates == prefills && b <= remaining && remaining < 2 * b,
                "actual owner did not reach the intended near-cap state");
        require(before.analysis_steps == work && !before.declined, "prefill work/decline accounting");
        const auto first = owner.certificate(entry, exact_first);
        require(bool(first), "first boundary observer should fit");
        identity_checks(first, cache, entry, exact_first);
        const auto once = cache.statistics();
        const auto second = owner.certificate(entry, !exact_first);
        require(!second && !second.payload, "second boundary observer revived via shared table pool");
        const auto after = cache.statistics();
        require(after.predictive_builds == prefills + 2 && after.retained_certificates == prefills + 1 &&
                after.retained_bytes == (prefills + 1) * b && after.declined == 1 && after.limit_declines == 1 &&
                after.allocation_declines == 0, "actual retention failure counters differ");
        require(after.analysis_steps == work + exact.record->analysis_steps + scoped.record->analysis_steps,
                "failed retain lost independently successful constructor work");
        require(once.retained_bytes == after.retained_bytes && after.retained_bytes < cap && cap - after.retained_bytes < b,
                "actual remaining budget would unexpectedly fit original second reservation");
        row(out, "actual_retention", mode, !exact_first, false, "full_original_reservation", "first_fits", entry, first, once);
        row(out, "actual_retention", mode, !exact_first, false, "full_original_reservation", "second_declines", entry, second, after);
        // The zero-charge alternative is algebraic only. No unsafe sharing
        // implementation is installed or exercised by this assertion.
        const bool hypothetical_zero_charge_fits = std::size_t{0} <= cap - once.retained_bytes;
        require(hypothetical_zero_charge_fits, "discount counterexample premise");
        for (unsigned retry = 0; retry < 3; ++retry) {
            ap::Plan warm; warm.fail_at = 0; Slot declined, retained;
            { ap::Guard guard(warm); declined = owner.certificate(entry, !exact_first); retained = owner.certificate(entry, exact_first); }
            ++warm_queries;
            require(!declined && retained.record == first.record && retained.payload == first.payload &&
                    warm.trace.count == 0 && !warm.fired, "retention tombstone changed on requery");
            require(same_statistics(cache.statistics(), after), "retention requery changed charges");
        }
        row(out, "actual_retention", mode, !exact_first, false, "full_original_reservation", "tombstone_requery", entry, {}, cache.statistics());
    }
}
} // namespace

int main(int argc, char** argv) {
    try {
        std::ofstream file;
        if (argc == 3 && std::string(argv[1]) == "--out") {
            file.open(argv[2]); require(file.good(), "could not open CSV");
        } else require(argc == 1, "usage: slot_authority_probe [--out FILE]");
        std::ostream& out = file.is_open() ? file : std::cout;
        header(out);
        dual_success(out);
        fault_traces(out);
        actual_retention_boundary(out);
        require(out.good(), "CSV write failed");
        std::cerr << "PASS checks=" << checks << " fault_cases=" << fault_cases << " actual_boundary_cases="
            << boundary_cases << " warm_slot_pair_queries=" << warm_queries << '\n';
        std::cerr << "ACTUAL CACHE ALLOCATION/RETENTION PROBE; NO MATCHER, CONCURRENCY OR PERFORMANCE CLAIM\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL " << e.what() << '\n'; return 1;
    }
}
