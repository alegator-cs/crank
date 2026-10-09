#include "occurrence_analysis.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace {
std::size_t checks = 0, analyses = 0, summaries = 0, generated = 0;
constexpr auto maximum = std::numeric_limits<std::size_t>::max();

void require(bool condition, const std::string& context) {
    ++checks;
    if (!condition) throw std::runtime_error(context);
}

bool same(const crank::OccurrenceLength& a, const crank::OccurrenceLength& b) {
    return std::tie(a.productive, a.minimum, a.offset, a.modulus, a.residues) ==
           std::tie(b.productive, b.minimum, b.offset, b.modulus, b.residues);
}

template<class Function>
void out_of_range(Function query, const std::string& label) {
    bool caught = false;
    try { query(); }
    catch (const std::out_of_range&) { caught = true; }
    require(caught, label);
}

crank::NodeId add(crank::Ast& ast, crank::Kind kind,
                  std::vector<crank::NodeId> children = {}) {
    crank::Node node;
    node.kind = kind;
    node.children = std::move(children);
    const auto id = ast.nodes.size();
    ast.nodes.push_back(std::move(node));
    return id;
}

crank::NodeId character(crank::Ast& ast) {
    const auto id = add(ast, crank::Kind::CharSet);
    ast.nodes[id].characters.set(static_cast<unsigned char>('a'));
    return id;
}

crank::NodeId repeat(crank::Ast& ast, crank::NodeId child, std::size_t lower,
                     std::optional<std::size_t> upper) {
    const auto id = add(ast, crank::Kind::Repeat, {child});
    ast.nodes[id].lower = lower;
    ast.nodes[id].upper = upper;
    return id;
}

struct Fixture { std::string label; crank::Ast ast; };

// Huge cardinalities are represented in the AST, never expanded into input.
// This fixture deliberately makes the forward root fold differ from its
// right-to-left suffix fold on overflow. Reusing length(root) for suffix zero
// would alter a public field even when both summaries remain conservative.
crank::Ast overflow_fixture() {
    crank::Ast ast;
    const auto a = character(ast);
    const auto huge = repeat(ast, a, maximum, maximum);
    const auto three = repeat(ast, a, 3, 3);
    const auto odd = add(ast, crank::Kind::Alt, {a, three});
    const auto dead = add(ast, crank::Kind::Call);
    ast.nodes[dead].call_target = dead;
    const auto epsilon = add(ast, crank::Kind::Empty);
    const auto maybe = add(ast, crank::Kind::Alt, {a, epsilon});
    const auto huge_nullable = repeat(ast, maybe, maximum, maximum);
    const auto huge_range = repeat(ast, maybe, maximum - 1, maximum);
    const auto huge_empty = repeat(ast, epsilon, maximum, maximum);
    const auto twice_huge = repeat(ast, huge, 2, 2);
    const auto invalid = repeat(ast, maybe, maximum, maximum - 1);
    const auto zero_dead = repeat(ast, dead, 0, 0);
    const auto positive_dead = repeat(ast, dead, 1, {});
    const auto overflow = add(ast, crank::Kind::Concat, {huge, a, odd});
    add(ast, crank::Kind::Concat, {dead, huge, a});
    add(ast, crank::Kind::Concat, {huge, dead, a});
    add(ast, crank::Kind::Concat, {a, huge, odd, huge_range});
    add(ast, crank::Kind::Concat, {huge_nullable, huge_empty, zero_dead});
    add(ast, crank::Kind::Concat, {twice_huge, invalid, positive_dead});
    ast.root = overflow;
    return ast;
}

crank::Ast empty_and_unknown_fixture() {
    crank::Ast ast;
    const auto empty_concat = add(ast, crank::Kind::Concat);
    const auto empty_alt = add(ast, crank::Kind::Alt);
    const auto empty_set = add(ast, crank::Kind::CharSet);
    const auto empty_capture = add(ast, crank::Kind::Capture);
    const auto empty_repeat = add(ast, crank::Kind::Repeat);
    const auto missing_call = add(ast, crank::Kind::Call);
    const auto reference = add(ast, crank::Kind::Reference);
    const auto forward = add(ast, crank::Kind::Reference);
    ast.nodes[forward].forward = true;
    const auto a = character(ast);
    const auto dynamic_count = repeat(ast, a, 7, 7);
    ast.nodes[dynamic_count].use_count = 0;
    const auto dynamic_path = repeat(ast, a, 8, 8);
    ast.nodes[dynamic_path].count_path = 0;
    const auto binding = repeat(ast, a, 2, 4);
    ast.nodes[binding].bind_count = 0;
    const auto definition = add(ast, crank::Kind::Define, {empty_alt, missing_call});
    add(ast, crank::Kind::Concat, {empty_alt, empty_capture, a});
    add(ast, crank::Kind::Concat, {a, empty_set, missing_call});
    add(ast, crank::Kind::Concat, {empty_repeat, reference, forward});
    add(ast, crank::Kind::Concat, {binding, dynamic_count, dynamic_path});
    ast.root = add(ast, crank::Kind::Concat, {empty_concat, definition, a});
    return ast;
}

std::uint32_t state = 0x6c7e139bU;
std::uint32_t random_value() {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

// Structural children precede their parents, but call targets may point
// forward into the later DAG and so create direct/mutual recursion. Dead
// strongly connected components, shared children and Define boundaries all
// occur without requiring a parser or duplicating the analysis equations.
crank::Ast generated_ast(unsigned serial) {
    crank::Ast ast;
    add(ast, crank::Kind::Empty);
    character(ast);
    add(ast, crank::Kind::CharSet);
    const auto call = add(ast, crank::Kind::Call);
    for (unsigned i = 0; i < 52; ++i) {
        const auto previous = ast.nodes.size();
        const auto child = random_value() % previous;
        const auto other = random_value() % previous;
        switch (random_value() % 9) {
        case 0: {
            std::vector<crank::NodeId> children;
            const auto count = random_value() % 8;
            for (unsigned j = 0; j < count; ++j)
                children.push_back(random_value() % previous);
            add(ast, crank::Kind::Concat, std::move(children));
            break;
        }
        case 1: add(ast, crank::Kind::Alt, {child, other}); break;
        case 2: repeat(ast, child, random_value() % 3, {}); break;
        case 3: {
            const auto lower = random_value() % 4;
            repeat(ast, child, lower, lower + random_value() % 4);
            break;
        }
        case 4: add(ast, crank::Kind::Capture, {child}); break;
        case 5: {
            const auto id = add(ast, crank::Kind::Call);
            ast.nodes[id].call_target = (i % 3 == 0) ? id : child;
            break;
        }
        case 6: add(ast, crank::Kind::Define, {child, other}); break;
        case 7: add(ast, crank::Kind::Concat, {child, other, child}); break;
        case 8: add(ast, crank::Kind::Alt, {call, child, other}); break;
        }
    }
    ast.nodes[call].call_target = serial % 3 == 0 ? call : ast.nodes.size() - 1;
    ast.root = add(ast, crank::Kind::Concat,
                   {ast.nodes.size() - 1, 1, ast.nodes.size() - 2});
    return ast;
}

std::vector<Fixture> fixtures() {
    std::vector<Fixture> result;
    const std::vector<std::string> patterns{
        "", "abc", "(?:aaaa|a|)(?:bbbb|b|)(?:cc|)",
        "(?<S>aa(?&S)bb||a)", "(?<S>(?&S)aaaa||a)",
        "(?<S>(?&S)(?&S)|a|)", "(?<S>(?&S){0,2}aaaa|)",
        "(?<S>(?&S))", "(?(DEFINE)(?<A>(?&B))(?<B>(?&A)))(?&A)",
        "(?(DEFINE)(?<A>(?&B)|)(?<B>(?&A)|aaaa))(?&A)",
        "(?(DEFINE)(?<A>aa(?&B)||a)(?<B>(?&A)bb))(?&A)",
        "(?(DEFINE)(?<unused>(?&unused)))a",
        "(?:a{63}|a{64})(?:aa|aaaa)(?:b{0,2})",
        "(?:a{64}|a{65}){2}(?:bb|b)",
        "(?<x>aa|a)\\k<x>(?:bb)*",
        "a*(?:aaaa){#n:1,3}b{=n}",
        "a*(?:aaaa){#n:1,3}b{=#0.@count}",
        "(?(DEFINE)(?<S>a(?&S)b|))(?&S)c{=S[0].@height}",
        "x(?(DEFINE)(?<S>aa(?&S)bb|cc))(?&S)d{=S[0].@height}ee{=S[0].@height}",
        "(?(DEFINE)(?<S>a(?&S)b(?&S)|ab|))(?&S)c{=S[0].@height}",
        "(?(DEFINE)(?<S>a(?&S)b|a|))(?&S)c{=S[0].@height}",
        "(?(DEFINE)(?<S>a(?&T)|)(?<T>b(?&S)))(?&S)c{=S[0].@height}"
    };
    for (std::size_t i = 0; i < patterns.size(); ++i)
        result.push_back({"parsed_" + std::to_string(i), crank::parse(patterns[i])});
    result.push_back({"overflow", overflow_fixture()});
    result.push_back({"empty_unknown", empty_and_unknown_fixture()});
    for (unsigned i = 0; i < 40; ++i) {
        result.push_back({"generated_" + std::to_string(i), generated_ast(i)});
        ++generated;
    }
    // Wide and nested concatenations distinguish per-node metadata from
    // boundary offsets and expose off-by-one mistakes at arena boundaries.
    crank::Ast wide;
    const auto a = character(wide);
    const auto odd = add(wide, crank::Kind::Alt, {a, repeat(wide, a, 3, 3)});
    std::vector<crank::NodeId> children(257, a);
    for (std::size_t i = 0; i < children.size(); i += 5) children[i] = odd;
    const auto row = add(wide, crank::Kind::Concat, std::move(children));
    const auto empty = add(wide, crank::Kind::Concat);
    const auto unary = add(wide, crank::Kind::Concat, {row});
    wide.root = add(wide, crank::Kind::Concat, {empty, unary, row, empty});
    result.push_back({"wide_nested", std::move(wide)});
    return result;
}

struct HeldSummary {
    const crank::OccurrenceLength* address;
    crank::OccurrenceLength value;
};

void inspect(const Fixture& fixture, const crank::OccurrenceAnalysis& analysis) {
    ++analyses;
    std::vector<HeldSummary> held;
    std::vector<std::pair<const std::vector<std::size_t>*, std::vector<std::size_t>>> minima;
    for (std::size_t id = 0; id < fixture.ast.nodes.size(); ++id) {
        const auto& node = fixture.ast.nodes[id];
        const auto& summary = analysis.length(id);
        held.push_back({&summary, summary});
        const auto& suffix = analysis.suffix_minimum(id);
        minima.emplace_back(&suffix, suffix);
        if (node.kind != crank::Kind::Concat) {
            require(suffix.empty(), fixture.label + ": non-concat has no boundaries");
            out_of_range([&] { (void)analysis.suffix_length(id, 0); },
                         fixture.label + ": non-concat suffix is out of range");
            continue;
        }
        require(suffix.size() == node.children.size() + 1,
                fixture.label + ": every concat boundary including epsilon is present");
        for (std::size_t boundary = 0; boundary < suffix.size(); ++boundary) {
            const auto& value = analysis.suffix_length(id, boundary);
            held.push_back({&value, value});
            require(value.minimum == suffix[boundary], fixture.label + ": legacy minima agree");
            if (boundary == node.children.size())
                require(same(value, {true, 0, 0, 0, 1}), fixture.label + ": final suffix is epsilon");
            if (boundary + 1 == node.children.size())
                require(same(value, analysis.length(node.children.back())),
                        fixture.label + ": last child summary is unchanged by epsilon");
        }
        out_of_range([&] { (void)analysis.suffix_length(id, suffix.size()); },
                     fixture.label + ": next boundary is out of range");
        out_of_range([&] { (void)analysis.suffix_length(id, maximum); },
                     fixture.label + ": huge boundary cannot wrap into the arena");
    }
    // Saved references must not be transient scratch slots reused by another
    // const query. Comparing after all queries catches such representation
    // changes without asserting that equal immutable summaries cannot alias.
    for (const auto& item : held)
        require(same(*item.address, item.value), fixture.label + ": query reference remains stable");
    for (const auto& item : minima)
        require(*item.first == item.second, fixture.label + ": minimum vector remains stable");
    summaries += held.size();
    out_of_range([&] { (void)analysis.length(fixture.ast.nodes.size()); }, "node bound");
    out_of_range([&] { (void)analysis.suffix_minimum(maximum); }, "minimum node bound");
    out_of_range([&] { (void)analysis.suffix_length(maximum, 0); }, "suffix node bound");
}

void known_properties(const std::vector<Fixture>& all) {
    const auto find = [&](std::string_view label) -> const crank::Ast& {
        for (const auto& fixture : all) if (fixture.label == label) return fixture.ast;
        throw std::runtime_error("missing fixture");
    };
    {
        const auto& ast = find("overflow");
        const crank::OccurrenceAnalysis analysis(ast);
        const auto& root = analysis.length(ast.root);
        const auto& suffix = analysis.suffix_length(ast.root, 0);
        require(root.minimum == maximum && suffix.minimum == maximum,
                "overflow saturates minima in both fold orders");
        require(root.modulus == 1 && suffix.modulus == 2 && suffix.offset == 1,
                "overflow preserves existing distinct forward and backward congruence fields");
    }
    for (const auto label : {"parsed_17", "parsed_18"}) {
        const crank::OccurrenceAnalysis analysis(find(label));
        require(analysis.height_equation().has_value(), "positive height certificate fixture");
    }
    for (const auto label : {"parsed_19", "parsed_20", "parsed_21"}) {
        const crank::OccurrenceAnalysis analysis(find(label));
        require(!analysis.height_equation(), "nonlinear/unequal-base/mutual height remains uncertified");
    }
    const crank::OccurrenceAnalysis empty(crank::Ast{});
    require(!empty.height_equation(), "empty AST has no height equation");
    out_of_range([&] { (void)empty.length(0); }, "empty node access");
    out_of_range([&] { (void)empty.suffix_minimum(0); }, "empty minimum access");
    out_of_range([&] { (void)empty.suffix_length(0, 0); }, "empty suffix access");
}

void ownership(const Fixture& fixture) {
    const crank::OccurrenceAnalysis expected(fixture.ast);
    const auto compare = [&](const crank::OccurrenceAnalysis& actual) {
        for (std::size_t id = 0; id < fixture.ast.nodes.size(); ++id) {
            require(same(actual.length(id), expected.length(id)),
                    fixture.label + ": copy/move preserves node fields");
            const auto& minima = expected.suffix_minimum(id);
            require(actual.suffix_minimum(id) == minima,
                    fixture.label + ": copy/move preserves all minima");
            for (std::size_t boundary = 0; boundary < minima.size(); ++boundary)
                require(same(actual.suffix_length(id, boundary),
                             expected.suffix_length(id, boundary)),
                        fixture.label + ": copy/move preserves suffix fields");
        }
        const auto& wanted = expected.height_equation();
        const auto& found = actual.height_equation();
        require(wanted.has_value() == found.has_value(),
                fixture.label + ": copy/move preserves certificate presence");
        if (wanted && found)
            require(std::tie(wanted->capture_id, wanted->offset, wanted->coefficient,
                             wanted->count_paths) ==
                    std::tie(found->capture_id, found->offset, found->coefficient,
                             found->count_paths),
                    fixture.label + ": copy/move preserves certificate fields");
    };
    std::optional<crank::OccurrenceAnalysis> survivor;
    {
        auto source = fixture.ast;
        crank::OccurrenceAnalysis original(source);
        crank::OccurrenceAnalysis copy(original);
        crank::OccurrenceAnalysis moved(std::move(copy));
        crank::OccurrenceAnalysis assigned(crank::parse("z"));
        assigned = original;
        crank::OccurrenceAnalysis move_assigned(crank::parse("z"));
        move_assigned = std::move(assigned);
        survivor.emplace(std::move(move_assigned));
        source = crank::Ast{};
        compare(moved);
        compare(*survivor);
        inspect(fixture, moved);
    }
    compare(*survivor);
    inspect(fixture, *survivor);
}

void write_length(const crank::OccurrenceLength& length) {
    std::cout << ' ' << length.productive << ' ' << length.minimum << ' '
              << length.offset << ' ' << length.modulus << ' ' << length.residues;
}

// This is deliberately a complete field transcript rather than a hash or
// reimplementation of the arithmetic equations. Build the same test source
// against the frozen old analysis and candidate, then compare stdout exactly.
void record(const Fixture& fixture, const crank::OccurrenceAnalysis& analysis) {
    std::cout << "fixture " << fixture.label << ' ' << fixture.ast.nodes.size()
              << ' ' << fixture.ast.root << '\n';
    for (std::size_t id = 0; id < fixture.ast.nodes.size(); ++id) {
        std::cout << "node " << id << ' ' << static_cast<int>(fixture.ast.nodes[id].kind);
        write_length(analysis.length(id));
        std::cout << '\n';
        const auto& minima = analysis.suffix_minimum(id);
        std::cout << "minima " << id << ' ' << minima.size();
        for (const auto minimum : minima) std::cout << ' ' << minimum;
        std::cout << '\n';
        for (std::size_t boundary = 0; boundary < minima.size(); ++boundary) {
            std::cout << "suffix " << id << ' ' << boundary;
            write_length(analysis.suffix_length(id, boundary));
            std::cout << '\n';
        }
    }
    const auto& equation = analysis.height_equation();
    std::cout << "height " << equation.has_value();
    if (equation) {
        std::cout << ' ' << equation->capture_id << ' ' << equation->offset
                  << ' ' << equation->coefficient << ' ' << equation->count_paths.size();
        for (const auto path : equation->count_paths) std::cout << ' ' << path;
    }
    std::cout << '\n';
}
} // namespace

int main(int argc, char** argv) {
    try {
        const bool records = argc == 2 && std::string_view(argv[1]) == "--summary-record";
        if (argc != 1 && !records) throw std::runtime_error("usage: test_analysis [--summary-record]");
        const auto all = fixtures();
        known_properties(all);
        for (const auto& fixture : all) {
            const crank::OccurrenceAnalysis analysis(fixture.ast);
            inspect(fixture, analysis);
            if (records) record(fixture, analysis);
        }
        ownership(all.back());
        ownership(all[17]);
        std::cout << "analysis checks=" << checks << " analyses=" << analyses
                  << " summaries=" << summaries << " fixtures=" << all.size()
                  << " generated=" << generated << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "analysis test failure: " << error.what() << '\n';
        return 1;
    }
}
