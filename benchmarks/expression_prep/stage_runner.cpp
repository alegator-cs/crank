// Fresh grammar-source preparation, with overlapping component measurements.
// Timing includes each object's destruction, excludes pre-parsing for component stages.
#include "crank.hpp"
#include "assisted_matcher.hpp"
#include "ast_control.hpp"
#include "ast_equivalence.hpp"
#include "ast_machine.hpp"
#include "boolean_source_view.hpp"
#include "equality_matcher.hpp"
#include "fragment_matcher.hpp"
#include "occurrence_analysis.hpp"
#include "optimizations.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <ctime>
#include <functional>
#include <fstream>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>

namespace allocation_probe {
thread_local bool enabled = false;
thread_local std::uint64_t calls = 0, bytes = 0;
void record(std::size_t n) noexcept { if (enabled) { ++calls; bytes += n; } }
}

// Optional single-operation allocation pass. It is never enabled during timing.
#if defined(CRANK_EXPRESSION_ALLOCATIONS)
void* operator new(std::size_t n) {
    if (void* p = std::malloc(n ? n : 1)) { allocation_probe::record(n); return p; }
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
#if defined(__linux__)
void* operator new(std::size_t n, std::align_val_t a) {
    void* p = nullptr;
    if (posix_memalign(&p, static_cast<std::size_t>(a), n ? n : 1) != 0) throw std::bad_alloc();
    allocation_probe::record(n); return p;
}
void* operator new[](std::size_t n, std::align_val_t a) { return ::operator new(n, a); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
#endif
#endif

namespace {
std::uint64_t sink = 0;
std::size_t selected_stages = 0;
std::uint64_t cpu_ns() {
#if defined(CLOCK_PROCESS_CPUTIME_ID)
    timespec t{};
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t)) throw std::runtime_error("process CPU clock failed");
    return static_cast<std::uint64_t>(t.tv_sec) * 1'000'000'000 + t.tv_nsec;
#else
    return static_cast<std::uint64_t>(std::clock()) * 1'000'000'000 / CLOCKS_PER_SEC;
#endif
}
std::string quoted(std::string_view value) {
    std::string result = "\"";
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') { result += '\\'; result += c; }
        else if (c >= 32 && c < 127) result += c;
        else { result += "\\u00"; result += hex[c >> 4]; result += hex[c & 15]; }
    }
    return result + '"';
}
std::uint64_t text_hash(std::string_view text) {
    std::uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : text) { h ^= c; h *= 1099511628211ULL; }
    return h;
}
std::uint64_t ast_hash(const crank::Ast& ast) {
    std::uint64_t h = 1469598103934665603ULL;
    const auto add = [&](std::uint64_t x) { h ^= x; h *= 1099511628211ULL; };
    add(ast.nodes.size()); add(ast.root);
    for (const auto& n : ast.nodes) {
        add(static_cast<unsigned>(n.kind)); add(n.lower); add(n.upper.has_value()); add(n.upper.value_or(0));
        add(n.capture_id); add(n.call_target); add(n.bind_count); add(n.use_count); add(n.count_path);
        add(n.forward); add(n.nullable); add(n.minimum_length); add(n.stateful); add(n.children.size());
        for (const auto child : n.children) add(child);
        for (std::size_t c = 0; c < 256; ++c) add(n.characters[c]);
    }
    for (const auto& name : ast.capture_names) add(text_hash(name));
    for (const auto id : ast.capture_nodes) add(id);
    for (const auto& name : ast.count_names) add(text_hash(name));
    for (const auto id : ast.count_nodes) add(id);
    add(ast.count_paths.size());
    add(ast.has_references); add(ast.has_counts); add(ast.has_calls);
    add(ast.has_occurrence_paths); add(ast.has_extended);
    return h;
}
struct Fixture { std::string id, source; bool malformed = false; };
std::vector<Fixture> fixtures() {
    std::vector<Fixture> f = {
        {"empty", ""},
        {"literal", "hello"},
        {"identifier", "[A-Za-z_][A-Za-z_0-9]*"},
        {"record", R"((?:[A-Z]{4}:[0-9]{6}\n)+)"},
        {"balanced_nullable", "(?<S>a(?&S)b|)"},
        {"mutual_nullable", "(?(DEFINE)(?<A>a(?&B)|)(?<B>b(?&A)|))(?&A)"},
        {"acyclic_calls", "(?(DEFINE)(?<D>[0-9]+)(?<P>(?&D)(?:[.](?&D))?))(?&P)(?:,(?&P))*"},
        {"nested_nullable", "(?:(?:(?:a?|b?)*)?)*c?"},
        {"shared_count", "a{#n}b{=n}c{=n}"},
        {"integer_row", "(?:aaa){0,100}(?:bbbbb){0,100}"},
        {"recursive_count", "(?(DEFINE)(?<S>a(?&S)|b))(?&S){#n}"},
        {"backreference", R"(([ab]+):\1)"},
        {"affine", R"((a*)(b*)\2\1)"},
        {"json_source", R"CRANK((?(DEFINE)(?<W>[ \t\r\n]*)(?<V>(?&O)|(?&A)|(?&T)|(?&N)|true|false|null)(?<O>[{](?&W)(?:(?&T)(?&W):(?&W)(?&V)(?&W)(?:,(?&W)(?&T)(?&W):(?&W)(?&V)(?&W))*)?[}])(?<A>\[(?&W)(?:(?&V)(?&W)(?:,(?&W)(?&V)(?&W))*)?\])(?<T>"(?:[\x20-\x21\x23-\x5b\x5d-\x7e]|\\(?:["\\/bfnrt]|u[0-9A-Fa-f]{4}))*")(?<N>-?(?:0|[1-9][0-9]*)(?:[.][0-9]+)?(?:[eE][+-]?[0-9]+)?))(?&W)(?&V)(?&W))CRANK"}
    };
    f.push_back({"literal_64", std::string(64, 'a')});
    f.push_back({"literal_1024", std::string(1024, 'a')});
    f.push_back({"nested_groups_64", std::string(64, '(') + "a" + std::string(64, ')')});
    std::string alternatives = "(?:";
    for (unsigned i = 0; i < 128; ++i) { if (i) alternatives += '|'; alternatives += "branch" + std::to_string(i); }
    alternatives += ')'; f.push_back({"alternatives_128", std::move(alternatives)});
    std::string sequence;
    for (unsigned i = 0; i < 256; ++i) sequence += "[ab]";
    f.push_back({"concat_256", std::move(sequence)});
    std::string recursive = "(?(DEFINE)(?<S>";
    for (unsigned i = 0; i < 32; ++i) { if (i) recursive += '|'; recursive += "x" + std::to_string(i) + "(?&S)y"; }
    recursive += "|))(?&S)"; f.push_back({"recursive_alternatives_32", std::move(recursive)});
    f.push_back({"bad_unclosed_group", "(?:abc", true});
    f.push_back({"bad_class_range", "[z-a]", true});
    f.push_back({"bad_missing_call", "(?&missing)", true});
    f.push_back({"bad_missing_reference", R"(abc\9)", true});
    f.push_back({"bad_repeat_bounds", "a{5,2}", true});
    f.push_back({"bad_unproductive_count", "(?<S>(?&S))(?:a(?&S)){#n}", true});
    f.push_back({"bad_nesting_limit", std::string(257, '(') + "a" + std::string(257, ')'), true});
    return f;
}
struct Stage { std::string id; std::function<std::uint64_t()> operation; };
struct Options {
    std::size_t reps = 16, rounds = 3;
    std::uint64_t max_cpu_ns = 30'000'000'000ULL;
    bool allocations = false, list = false;
    std::string fixture, stage;
    std::unordered_map<std::string, std::size_t> plan;
};
std::size_t number(const char* value, std::size_t maximum) {
    std::size_t used = 0; const auto n = std::stoull(value, &used);
    if (used != std::string_view(value).size() || n == 0 || n > maximum) throw std::invalid_argument("numeric argument out of range");
    return n;
}
Options arguments(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const auto value = [&]() { if (++i == argc) throw std::invalid_argument("missing option value"); return argv[i]; };
        if (a == "--reps") o.reps = number(value(), 100000);
        else if (a == "--rounds") o.rounds = number(value(), 99);
        else if (a == "--max-cpu-ms") o.max_cpu_ns = number(value(), 600000) * 1'000'000ULL;
        else if (a == "--fixture") o.fixture = value();
        else if (a == "--stage") o.stage = value();
        else if (a == "--allocations") o.allocations = true;
        else if (a == "--plan") {
            std::ifstream input(value());
            if (!input) throw std::invalid_argument("cannot open repetition plan");
            std::string fixture, stage, reps;
            while (input >> fixture) {
                if (!(input >> stage >> reps)) throw std::invalid_argument("incomplete repetition plan row");
                if (!o.plan.emplace(fixture + '/' + stage, number(reps.c_str(), 100000)).second)
                    throw std::invalid_argument("duplicate repetition plan row");
            }
        }
        else if (a == "--list") o.list = true;
        else throw std::invalid_argument("unknown option: " + a);
    }
    return o;
}
std::vector<Stage> stages(const Fixture& f, const crank::Ast& ast,
                          const std::vector<crank::NodeId>& control) {
    using namespace crank;
    std::vector<Stage> s;
    // Only constant source storage is retained; every preparation is fresh.
    s.push_back({"parse_source", [&] { auto a = parse(f.source); return a.nodes.size() + a.root; }});
    s.push_back({"ast_copy", [&] { auto a = ast; return a.nodes.size() + a.root; }});
    s.push_back({"occurrence_analysis", [&] { OccurrenceAnalysis a(ast); return a.length(ast.root).minimum; }});
    s.push_back({"length_filter_integer_on", [&] { LengthFilter p(ast, true); return p.has_integer_row() + p.accepts(0); }});
    s.push_back({"length_filter_integer_off", [&] { LengthFilter p(ast, false); return p.has_integer_row() + p.accepts(0); }});
    s.push_back({"greedy_plan", [&] { auto p = make_greedy_plan(ast); return p.has_value(); }});
    s.push_back({"count_plan", [&] { auto p = make_count_plan(ast); return p.has_value(); }});
    s.push_back({"equality_plan", [&] { auto p = make_equality_plan(ast); return p.has_value(); }});
    s.push_back({"affine_plan", [&] { auto p = make_affine_plan(ast); return p.has_value(); }});
    s.push_back({"regular_eligibility", [&] { return RegularProgram::eligible(ast); }});
    if (RegularProgram::eligible(ast)) {
        s.push_back({"root_regular_program", [&] { RegularProgram p(ast); return p.construction_statistics().dfa_states; }});
        s.push_back({"root_regular_fragment", [&] { RegularFragment p(ast, ast.root); return p.compiled_statistics().dfa_states; }});
    }
    if (detail::AstMachineProgram::eligible(ast)) {
        s.push_back({"transparent_control", [&] { auto t = detail::pure_control_targets(ast); return t[ast.root]; }});
        s.push_back({"equivalence_quotient", [&] {
            detail::EquivalenceStatistics stats;
            auto t = detail::pure_ast_equivalent_targets(ast, control, &stats);
            return t[ast.root] + stats.merged_nodes + stats.work;
        }});
        s.push_back({"boolean_source_view", [&] { detail::BooleanSourceView p(ast); return p.compilation_steps() + p.ready(); }});
        s.push_back({"ast_machine_program", [&] { detail::AstMachineProgram p(ast); return p.ast().nodes.size(); }});
        s.push_back({"assisted_program_one_worker", [&] {
            AssistanceOptions options; options.workers = 1;
            AssistedProgram p(ast, options); return 1;
        }});
    }
    s.push_back({"occurrence_program", [&] { OccurrenceProgram p(ast); return p.supports_shared_calls(); }});
    s.push_back({"program_integer_on", [&] { Program p(f.source); return p.ast().nodes.size(); }});
    s.push_back({"program_integer_off", [&] {
        CompileOptions o; o.prepare_integer_row = false; Program p(f.source, o); return p.ast().nodes.size();
    }});
    return s;
}
void run(const Fixture& f, const Stage& stage, const Options& o, std::uint64_t start_cpu) {
    if (!o.stage.empty() && stage.id != o.stage) return;
    ++selected_stages;
    const auto planned = o.plan.find(f.id + '/' + stage.id);
    const auto reps = planned == o.plan.end() ? o.reps : planned->second;
    if (o.list) { std::cout << "{\"kind\":\"stage\",\"fixture\":" << quoted(f.id) << ",\"stage\":" << quoted(stage.id) << "}\n"; return; }
    // One explicit untimed warmup/preflight, never retained as a cache hit.
    try { sink ^= stage.operation(); }
    catch (const std::exception& e) {
        std::cout << "{\"kind\":\"declined\",\"fixture\":" << quoted(f.id) << ",\"stage\":" << quoted(stage.id)
                  << ",\"diagnostic\":" << quoted(e.what()) << "}\n"; return;
    }
    if (o.allocations) {
        allocation_probe::calls = allocation_probe::bytes = 0;
        allocation_probe::enabled = true;
        try { sink ^= stage.operation(); }
        catch (...) { allocation_probe::enabled = false; throw; }
        allocation_probe::enabled = false;
        std::cout << "{\"kind\":\"allocation\",\"fixture\":" << quoted(f.id) << ",\"stage\":" << quoted(stage.id)
                  << ",\"operations\":1,\"calls\":" << allocation_probe::calls << ",\"requested_bytes\":" << allocation_probe::bytes << "}\n";
    }
    for (std::size_t round = 0; round < o.rounds; ++round) {
        if (cpu_ns() - start_cpu >= o.max_cpu_ns) throw std::runtime_error("total process CPU measurement budget reached");
        std::uint64_t checksum = 0;
        const auto w0 = std::chrono::steady_clock::now(); const auto c0 = cpu_ns();
        for (std::size_t i = 0; i < reps; ++i) checksum += stage.operation();
        const auto elapsed = cpu_ns() - c0;
        const auto wall = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - w0).count();
        sink ^= checksum;
        std::cout << "{\"kind\":\"timing\",\"fixture\":" << quoted(f.id) << ",\"stage\":" << quoted(stage.id)
                  << ",\"round\":" << round << ",\"reps\":" << reps << ",\"cpu_ns\":" << elapsed
                  << ",\"wall_ns\":" << wall << ",\"checksum\":" << checksum << "}\n";
    }
}
} // namespace
int main(int argc, char** argv) {
    try {
        const auto o = arguments(argc, argv);
#if !defined(CRANK_EXPRESSION_ALLOCATIONS)
        if (o.allocations) throw std::invalid_argument("allocation pass requires CRANK_EXPRESSION_ALLOCATIONS build");
#endif
        const auto start = cpu_ns();
        std::cout << "{\"kind\":\"metadata\",\"schema\":1,\"scope\":\"grammar_source_preparation\","
                  << "\"timing_includes_destruction\":true,\"component_stages_overlap\":true,"
                  << "\"reps\":" << o.reps << ",\"rounds\":" << o.rounds << ",\"clock\":\"process_cpu\"}\n";
        for (const auto& f : fixtures()) {
            if (!o.fixture.empty() && f.id != o.fixture) continue;
            if (f.malformed) {
                std::string diagnostic;
                try { (void)crank::parse(f.source); throw std::runtime_error("malformed fixture was unexpectedly accepted: " + f.id); }
                catch (const std::invalid_argument& e) { diagnostic = e.what(); }
                std::cout << "{\"kind\":\"diagnostic\",\"fixture\":" << quoted(f.id) << ",\"source\":" << quoted(f.source)
                          << ",\"message\":" << quoted(diagnostic) << "}\n";
                Stage parse_bad{"parse_source_invalid", [&] {
                    try { (void)crank::parse(f.source); }
                    catch (const std::invalid_argument& e) { return text_hash(e.what()); }
                    throw std::runtime_error("unexpected malformed acceptance");
                }};
                Stage program_bad{"program_source_invalid", [&] {
                    try { crank::Program p(f.source); }
                    catch (const std::invalid_argument& e) { return text_hash(e.what()); }
                    throw std::runtime_error("unexpected malformed acceptance");
                }};
                run(f, parse_bad, o, start); run(f, program_bad, o, start); continue;
            }
            const auto ast = crank::parse(f.source);
            std::cout << "{\"kind\":\"fixture\",\"fixture\":" << quoted(f.id) << ",\"source\":" << quoted(f.source)
                      << ",\"source_bytes\":" << f.source.size() << ",\"nodes\":" << ast.nodes.size() << ",\"ast_hash\":" << ast_hash(ast) << "}\n";
            const auto control = crank::detail::AstMachineProgram::eligible(ast)
                ? crank::detail::pure_control_targets(ast) : std::vector<crank::NodeId>{};
            for (const auto& stage : stages(f, ast, control)) run(f, stage, o, start);
        }
        if (selected_stages == 0) throw std::invalid_argument("no preparation stage matched filters");
        std::cout << "{\"kind\":\"complete\",\"sink\":" << sink << "}\n";
    } catch (const std::exception& e) {
        std::cerr << "expression_prep: " << e.what() << '\n'; return 1;
    }
}
