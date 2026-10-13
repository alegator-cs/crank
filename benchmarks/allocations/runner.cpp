#include "crank.hpp"
#include "ast_machine.hpp"
#include "assisted_matcher.hpp"
#include "ast_control.hpp"
#include "ast_equivalence.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <new>
#include <thread>

// Executable-only replacement C++ allocation functions. No engine budgets are
// used. Requested payload bytes exclude this probe's headers and allocator
// metadata. All thread allocations are attributed by allocating thread.
namespace probe {
thread_local bool foreground = false;
std::atomic<bool> enabled{false};
std::atomic<long long> fail{-1};
struct Counters { std::atomic<std::size_t> calls{0}, bytes{0}, live{0}, peak{0}; } c[2];
std::atomic<std::size_t> live{0}, peak{0};
struct Header { void* raw; std::size_t bytes, thread; };
void maximum(std::atomic<std::size_t>& p, std::size_t n) {
    auto old = p.load(std::memory_order_relaxed);
    while (old < n && !p.compare_exchange_weak(old, n, std::memory_order_relaxed)) {}
}
void* allocate(std::size_t n, std::size_t alignment) {
    if (enabled.load(std::memory_order_relaxed)) {
        auto ordinal = fail.load(std::memory_order_relaxed);
        if (foreground && ordinal >= 0 && fail.fetch_sub(1) == 0) throw std::bad_alloc{};
    }
    alignment = std::max(alignment, alignof(Header));
    if (n > SIZE_MAX - alignment - sizeof(Header)) throw std::bad_alloc{};
    void* raw = std::malloc(std::max<std::size_t>(n, 1) + alignment + sizeof(Header));
    if (!raw) throw std::bad_alloc{};
    auto address = (reinterpret_cast<std::uintptr_t>(raw) + sizeof(Header) + alignment - 1) & ~(alignment - 1);
    auto* h = reinterpret_cast<Header*>(address) - 1;
    const auto t = foreground ? 0u : 1u;
    *h = {raw, n, t};
    maximum(peak, live.fetch_add(n, std::memory_order_relaxed) + n);
    maximum(c[t].peak, c[t].live.fetch_add(n, std::memory_order_relaxed) + n);
    if (enabled.load(std::memory_order_relaxed)) {
        c[t].calls.fetch_add(1, std::memory_order_relaxed);
        c[t].bytes.fetch_add(n, std::memory_order_relaxed);
    }
    return reinterpret_cast<void*>(address);
}
void release(void* p) noexcept {
    if (!p) return;
    auto* h = static_cast<Header*>(p) - 1;
    live.fetch_sub(h->bytes, std::memory_order_relaxed);
    c[h->thread].live.fetch_sub(h->bytes, std::memory_order_relaxed);
    std::free(h->raw);
}
}
#ifdef CRANK_ALLOC_PROBE
void* operator new(std::size_t n) { return probe::allocate(n, alignof(std::max_align_t)); }
void* operator new[](std::size_t n) { return ::operator new(n); }
void* operator new(std::size_t n, std::align_val_t a) { return probe::allocate(n, static_cast<std::size_t>(a)); }
void* operator new[](std::size_t n, std::align_val_t a) { return ::operator new(n,a); }
void operator delete(void* p) noexcept { probe::release(p); }
void operator delete[](void* p) noexcept { probe::release(p); }
void operator delete(void* p, std::size_t) noexcept { probe::release(p); }
void operator delete[](void* p, std::size_t) noexcept { probe::release(p); }
void operator delete(void* p, std::align_val_t) noexcept { probe::release(p); }
void operator delete[](void* p, std::align_val_t) noexcept { probe::release(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { probe::release(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { probe::release(p); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { try { return ::operator new(n); } catch (...) { return nullptr; } }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { try { return ::operator new(n); } catch (...) { return nullptr; } }
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept { try { return ::operator new(n,a); } catch (...) { return nullptr; } }
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept { try { return ::operator new(n,a); } catch (...) { return nullptr; } }
void operator delete(void* p, const std::nothrow_t&) noexcept { probe::release(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { probe::release(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { probe::release(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { probe::release(p); }
#endif
namespace {
using Clock = std::chrono::steady_clock;
std::uint64_t ns(Clock::duration d) { return std::chrono::duration_cast<std::chrono::nanoseconds>(d).count(); }
std::string read(const char* path) { std::ifstream f(path,std::ios::binary); if (!f) throw std::runtime_error("input missing"); return {std::istreambuf_iterator<char>(f),{}}; }
}
int main(int argc, char** argv) {
    probe::foreground = true;
    if (argc < 6) return 2;
    const auto pattern = read(argv[1]), subject = read(argv[2]);
    const std::string phase = argv[3];
    const auto reps = std::stoul(argv[4]);
    const bool allocations = std::string(argv[5]) == "probe";
    auto ast = crank::parse(pattern);
    auto control = crank::detail::pure_control_targets(ast);
    std::unique_ptr<crank::Program> program;
    std::unique_ptr<crank::detail::AstMachineProgram> native;
    std::unique_ptr<crank::AssistedProgram> assisted;
    if (phase == "first_auto" || phase == "warm_auto" || phase == "first_witness" || phase == "warm_witness") program = std::make_unique<crank::Program>(pattern);
    if (phase == "first_native" || phase == "warm_native" || phase == "first_core" || phase == "warm_core") native = std::make_unique<crank::detail::AstMachineProgram>(ast);
    crank::AssistanceOptions ao; ao.workers=4;
    if (phase == "first_assisted" || phase == "warm_assisted") assisted = std::make_unique<crank::AssistedProgram>(ast,ao);
    std::size_t verdict = 0, helpers = 0, decisions = 0, native_steps = 0;
    auto query = [&]() -> std::size_t {
        if (phase == "parse") { auto a=crank::parse(pattern); return a.nodes.size(); }
        if (phase == "prepare") { crank::Program p(pattern); return p.ast().nodes.size(); }
        if (phase == "equivalence") { auto t=crank::detail::pure_ast_equivalent_targets(ast,control); return t.size(); }
        if (phase == "regular") { crank::RegularProgram p(ast); return p.construction_statistics().dfa_states; }
        if (phase == "prepare_native") { crank::detail::AstMachineProgram p(ast); return p.ast().nodes.size(); }
        if (phase == "prepare_assisted") { crank::AssistedProgram p(ast,ao); p.wait_idle(); return 1; }
        // Fresh owned input storage is part of service cost on every query.
        std::string input(subject.data(),subject.size());
        if (program) {
            if (phase == "first_witness" || phase == "warm_witness") return program->first_match(input).has_value();
            return program->full_match(input);
        }
        if (native) {
            crank::OccurrenceMatchOptions o;
            if (phase == "first_core" || phase == "warm_core") {
                o.predictive_recursion=false; o.bounded_word_prediction=false;
                o.deterministic_programs=false; o.normalized_exact_programs=false;
                o.exact_end_spines=false; o.predictive_exact_rims=false;
                o.regional_execution=false; o.local_regular_regions=false;
            }
            crank::OccurrenceStatistics s; auto v=native->full_match(input,o,&s); native_steps=s.work_steps; return v;
        }
        if (assisted) { crank::AssistedStatistics s; auto v=assisted->full_match(input,{},&s); helpers+=s.helpers_admitted; decisions=static_cast<std::size_t>(s.decision); native_steps=s.matching.work_steps; return v; }
        throw std::runtime_error("unknown phase");
    };
    if (phase.rfind("warm_",0)==0) { query(); if (assisted) assisted->wait_idle(); helpers=0; }
    const auto initial_pool=assisted ? assisted->pool_statistics() : crank::AssistancePoolStatistics{};
    const auto start_live = probe::live.load();
    for (auto& c:probe::c) { c.calls=0; c.bytes=0; c.peak=c.live.load(); }
    probe::peak=start_live;
    if (argc>6) probe::fail=std::stoll(argv[6]);
    probe::enabled=allocations;
    std::uint64_t wall=0, drain=0;
    std::size_t failures=0;
    for (std::size_t i=0;i<reps;++i) {
        auto begin=Clock::now();
        try { verdict=query(); } catch (const std::bad_alloc&) { ++failures; } catch (const crank::ResourceLimit&) { verdict=2; }
        wall+=ns(Clock::now()-begin);
        if (assisted) { begin=Clock::now(); assisted->wait_idle(); drain+=ns(Clock::now()-begin); }
    }
    probe::enabled=false;
    const auto end_live=probe::live.load(), peak_live=probe::peak.load();
    const auto pool=assisted ? assisted->pool_statistics() : crank::AssistancePoolStatistics{};
    std::cout << "{\"phase\":\"" << phase << "\",\"reps\":" << reps << ",\"verdict\":" << verdict << ",\"failures\":" << failures
        << ",\"wall_ns\":" << wall << ",\"drain_ns\":" << drain << ",\"main_calls\":" << probe::c[0].calls << ",\"main_bytes\":" << probe::c[0].bytes
        << ",\"helper_calls\":" << probe::c[1].calls << ",\"helper_bytes\":" << probe::c[1].bytes << ",\"start_live\":" << start_live << ",\"peak_live\":" << peak_live
        << ",\"peak_extra\":" << peak_live-start_live << ",\"end_live\":" << end_live << ",\"helpers_admitted\":" << helpers << ",\"decision\":" << decisions
        << ",\"native_steps\":" << native_steps << ",\"tasks_finished\":" << pool.tasks_finished-initial_pool.tasks_finished << "}\n";
}
