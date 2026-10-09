#include "ast_machine.hpp"
#include "tests/helpers/test_progress.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

// Deliberately independent denotational reference evaluator. It knows only the
// public AST syntax and a finite Boolean relation on input positions. It uses
// no Crank analysis, request keys, scheduler, continuations, proofs, summaries,
// or length/nullability metadata. The dense relation is appropriate here only
// because oracle inputs are short; this is not an execution backend.
namespace {
using namespace crank;
crank_test::Progress progress("test_relation_oracle");
std::uint64_t diagnostic_seed = UINT64_C(0x6f726967696e7331);
unsigned diagnostic_maximum = 4;
std::size_t diagnostic_random_count = 40;

struct Matrix {
    std::size_t width;
    std::vector<unsigned char> bits;
    explicit Matrix(std::size_t n) : width(n), bits(n * n) {}
    unsigned char& at(std::size_t i, std::size_t j) { return bits[i * width + j]; }
    bool at(std::size_t i, std::size_t j) const { return bits[i * width + j] != 0; }
    bool operator==(const Matrix&) const = default;
};

Matrix identity(std::size_t n) {
    Matrix result(n);
    for (std::size_t i = 0; i < n; ++i) result.at(i, i) = 1;
    return result;
}
void unite(Matrix& into, const Matrix& from) {
    for (std::size_t i = 0; i < into.bits.size(); ++i) into.bits[i] |= from.bits[i];
}
Matrix compose(const Matrix& left, const Matrix& right) {
    Matrix result(left.width);
    for (std::size_t i = 0; i < left.width; ++i)
        for (std::size_t k = 0; k < left.width; ++k)
            if (left.at(i, k))
                for (std::size_t j = 0; j < left.width; ++j)
                    result.at(i, j) |= right.at(k, j);
    return result;
}
Matrix closure(Matrix result) {
    for (std::size_t i = 0; i < result.width; ++i) result.at(i, i) = 1;
    for (std::size_t k = 0; k < result.width; ++k)
        for (std::size_t i = 0; i < result.width; ++i)
            if (result.at(i, k))
                for (std::size_t j = 0; j < result.width; ++j)
                    result.at(i, j) |= result.at(k, j);
    return result;
}

// Binary exponentiation plus a geometric relation sum. Returns (R^n,
// I | R | ... | R^(n-1)); finite repeats never need a count-sized loop.
std::pair<Matrix, Matrix> powers(const Matrix& relation, std::size_t n) {
    if (!n) return {identity(relation.width), Matrix(relation.width)};
    auto [half, sum] = powers(relation, n / 2);
    auto square = compose(half, half);
    auto tail = compose(half, sum);
    unite(sum, tail);
    if (n % 2) {
        unite(sum, square);
        square = compose(square, relation);
    }
    return {std::move(square), std::move(sum)};
}

struct Language {
    std::vector<Matrix> nodes;
    std::size_t rounds = 0;
};
Language language(const Ast& ast, const std::string& input) {
    const auto width = input.size() + 1;
    Language result{std::vector<Matrix>(ast.nodes.size(), Matrix(width))};
    for (;;) {
        auto next = result.nodes;
        for (NodeId id = 0; id < ast.nodes.size(); ++id) {
            const auto& node = ast.nodes[id];
            Matrix value(width);
            switch (node.kind) {
            case Kind::Empty: case Kind::Define: value = identity(width); break;
            case Kind::CharSet:
                for (std::size_t i = 0; i < input.size(); ++i)
                    if (node.characters[static_cast<unsigned char>(input[i])])
                        value.at(i, i + 1) = 1;
                break;
            case Kind::Capture: value = result.nodes.at(node.children.at(0)); break;
            case Kind::Call: value = result.nodes.at(node.call_target); break;
            case Kind::Alt:
                for (auto child : node.children) unite(value, result.nodes.at(child));
                break;
            case Kind::Concat:
                value = identity(width);
                for (auto child : node.children) value = compose(value, result.nodes.at(child));
                break;
            case Kind::Repeat: {
                const auto& child = result.nodes.at(node.children.at(0));
                auto lower = powers(child, node.lower).first;
                if (node.upper) {
                    const auto difference = *node.upper - node.lower;
                    auto [last, sum] = powers(child, difference);
                    unite(sum, last); // Inclusive upper, without overflowing upper+1.
                    value = compose(lower, sum);
                } else value = compose(lower, closure(child));
                break;
            }
            default: throw std::logic_error("oracle received an effectful node");
            }
            unite(next[id], value);
        }
        ++result.rounds;
        if (next == result.nodes) return result;
        result.nodes = std::move(next);
        // Every strict iteration adds at least one of finitely many facts.
        if (result.rounds > ast.nodes.size() * width * width + 1)
            throw std::logic_error("least-fixed-point convergence invariant");
    }
}

std::string escaped(std::string_view value) {
    std::ostringstream out;
    for (unsigned char c : value) {
        if (c >= 32 && c < 127) out << c;
        else out << "\\x" << std::hex << std::setw(2) << std::setfill('0') << unsigned(c);
    }
    return out.str();
}

struct Random {
    std::uint64_t state;
    std::uint64_t next() {
        state ^= state >> 12; state ^= state << 25; state ^= state >> 27;
        return state * UINT64_C(2685821657736338717);
    }
    std::size_t choose(std::size_t bound) { return next() % bound; }
};

NodeId add(Ast& ast, Kind kind, std::vector<NodeId> children = {}) {
    Node n; n.kind = kind; n.children = std::move(children);
    ast.nodes.push_back(std::move(n)); return ast.nodes.size() - 1;
}
NodeId byte(Ast& ast, unsigned char value) {
    const auto id = add(ast, Kind::CharSet); ast.nodes[id].characters.set(value); return id;
}
NodeId repeat(Ast& ast, NodeId child, std::size_t lower, std::optional<std::size_t> upper) {
    const auto id = add(ast, Kind::Repeat, {child});
    ast.nodes[id].lower = lower; ast.nodes[id].upper = upper; return id;
}
NodeId call(Ast& ast, NodeId target) {
    const auto id = add(ast, Kind::Call); ast.nodes[id].call_target = target;
    ast.has_calls = true; return id;
}

struct Fixture {
    std::string name;
    Ast ast;
    std::vector<std::string> inputs;
    std::string expression{};
};
std::vector<std::string> words(std::string alphabet, unsigned maximum) {
    std::vector<std::string> all{std::string{}}, layer = all;
    for (unsigned length = 0; length < maximum; ++length) {
        std::vector<std::string> next;
        for (const auto& prefix : layer) for (const auto ch : alphabet) next.push_back(prefix + ch);
        all.insert(all.end(), next.begin(), next.end()); layer = std::move(next);
    }
    return all;
}

NodeId expression(Ast& ast, const std::vector<NodeId>& rules, Random& random, unsigned depth) {
    const auto kind = random.choose(depth ? 9 : 4);
    if (kind == 0) return add(ast, Kind::Empty);
    if (kind == 1) return byte(ast, 'a');
    if (kind == 2) return byte(ast, 'b');
    if (kind == 3) return call(ast, rules[random.choose(rules.size())]);
    if (kind == 4 || kind == 5) {
        std::vector<NodeId> children;
        const auto count = 2 + random.choose(2);
        for (std::size_t i = 0; i < count; ++i) children.push_back(expression(ast, rules, random, depth - 1));
        return add(ast, kind == 4 ? Kind::Concat : Kind::Alt, std::move(children));
    }
    if (kind == 6) {
        const auto id = add(ast, Kind::CharSet); ast.nodes[id].characters.set('a');
        ast.nodes[id].characters.set('b'); return id;
    }
    const auto child = expression(ast, rules, random, depth - 1);
    const auto lower = random.choose(3);
    return repeat(ast, child, lower, kind == 7 ? std::optional<std::size_t>{lower + random.choose(3)} : std::nullopt);
}

Fixture random_fixture(Random& random, unsigned ordinal, unsigned maximum) {
    Ast ast; std::vector<NodeId> rules;
    const auto count = 2 + random.choose(3);
    for (std::size_t i = 0; i < count; ++i) {
        const auto id = add(ast, Kind::Capture);
        ast.nodes[id].capture_id = ast.capture_names.size();
        ast.capture_names.push_back("R" + std::to_string(i));
        ast.capture_nodes.push_back(id); rules.push_back(id);
    }
    for (const auto rule : rules) {
        const auto child = expression(ast, rules, random, 2 + random.choose(2));
        ast.nodes[rule].children = {child};
    }
    // Several callers of the same rules, with different suffixes and origins.
    const auto first = call(ast, rules[random.choose(count)]);
    const auto second = call(ast, rules[random.choose(count)]);
    const auto third = call(ast, rules[random.choose(count)]);
    const auto left = add(ast, Kind::Concat, {byte(ast, 'a'), first, byte(ast, 'a')});
    const auto right = add(ast, Kind::Concat, {second, byte(ast, 'b')});
    ast.root = add(ast, Kind::Alt, {left, right, third}); ast.capture_nodes[0] = ast.root;
    return {"random_" + std::to_string(ordinal), std::move(ast), words("ab", maximum)};
}

std::vector<Fixture> fixtures(unsigned maximum) {
    std::vector<Fixture> result;
    auto add_pattern = [&](std::string name, std::string pattern, std::string alphabet,
                           std::vector<std::string> extras = {}) {
        auto input = words(alphabet, std::min(maximum, 3U));
        input.insert(input.end(), extras.begin(), extras.end());
        std::sort(input.begin(), input.end()); input.erase(std::unique(input.begin(), input.end()), input.end());
        auto parsed = progress.call("parse", progress.enabled()
            ? "fixture=" + name + " expression=" + escaped(pattern) + " input=<fixture generation> profile=<parse> states=<not applicable> steps=<not applicable>"
            : std::string{}, [&] { return parse(pattern); });
        result.push_back({std::move(name), std::move(parsed), std::move(input),
                          progress.enabled() ? std::move(pattern) : std::string{}});
    };
    // X is observed from two origins by two different suffix consumers.
    // Unioning X(0)={1} and X(1)={2} and sending {1,2} to both is unsound:
    // aab would then spuriously pass the X b branch. Duplicate messages are
    // irrelevant; the (origin, consumer) correlation is not.
    add_pattern("origin_consumer_correlation", "(?(DEFINE)(?<X>a))a(?&X)c|(?&X)b", "abc", {"aab", "aac", "ab"});
    add_pattern("recursive_origin_correlation", "(?(DEFINE)(?<X>a(?&X)|a))a(?&X)c|(?&X)b", "abc", {"aaac", "aaaab", "aacb"});
    add_pattern("nonrectangular_end_control", "(?(DEFINE)(?<X>a?|ab))a(?&X)bc|(?&X)ba|aa(?&X)c", "abc", {"aabc", "aaba", "aaabc", "aabbc"});
    add_pattern("exact_and_open_demand", "(?(DEFINE)(?<X>(?&X)a|a|))(?&X)b|(?&X)ab|a(?&X)c", "abc", {"aaab", "aaac", "aaaa", "ba"});
    add_pattern("same_origin_different_demands", "(?(DEFINE)(?<X>a(?&X)|))(?&X)(?&X)b|(?&X)bc", "abc", {"aaab", "aabc", "aaabc", "aabca"});
    add_pattern("nullable_replay_after_result", "(?(DEFINE)(?<A>(?&B)|)(?<B>(?&A)|a))(?&A)(?&B)b|(?&B)c", "abc");
    add_pattern("unproductive_mutual_cycle", "(?(DEFINE)(?<A>(?&B))(?<B>(?&A)))(?&A)|a", "ab");
    add_pattern("nullable_catalan", "(?<S>(?&S)(?&S)|a|)", "ab", {"aaaaaa", "aaaaab"});
    add_pattern("nonnullable_catalan", "(?<S>(?&S)(?&S)|a)", "ab", {"aaaaaa", "aaaaab"});
    add_pattern("two_distinct_recursive_suffixes", "(?(DEFINE)(?<A>a(?&A)b|)(?<B>a(?&B)c|))(?&A)c|(?&B)b", "abc", {"aabbc", "aaccb", "aabcb", "aacbc"});
    add_pattern("local_regular_multiple_endpoints", "(?<S>a(?:ab|a)*b(?&S)c|d)", "abcd", {"aabdc", "aaabbdc", "ababdc", "aabdcc"});
    add_pattern("local_regular_mixed_alternation", "(?<S>ab|ac|a(?&S)c|)", "abc", {"aabcc", "aaaccc", "abcc"});
    add_pattern("local_regular_nullable_regions", "(?<S>(?:ab?){0,3}(?&S)(?:ba|b)|)", "ab", {"abb", "ababa", "aaabbb"});
    add_pattern("count_above_control_word", "(?:a?|ab?){63,66}b", "ab", {"aaaab"});
    add_pattern("unbounded_count_above_control_word", "(?:a?|b?){65,}", "ab", {"abbaab"});
    std::string wide;
    for (unsigned i = 0; i < 70; ++i) wide += i % 3 ? "a?" : "(?:a?|b?)";
    add_pattern("concat_above_control_word", wide + "b", "ab", {"aaaab", "aaaba"});
    add_pattern("binary_terminals", "(?:[\\x00\\xff]?a)*", std::string("a\0\xff", 3));
    // Above the 16-subscriber promotion threshold, including caller-word and
    // input-position word boundaries. Only exact-interval sampling is reduced
    // on these longer subjects; every unrestricted start row is still checked.
    std::vector<std::string> dense_inputs;
    for (std::size_t n : {24, 32, 64, 65}) {
        dense_inputs.push_back(std::string(n, 'a'));
        dense_inputs.push_back(std::string(n - 1, 'a') + 'b');
    }
    result.push_back({"dense_catalan_words", progress.call("parse", progress.enabled()
        ? "fixture=dense_catalan_words expression=(?<S>(?&S)(?&S)|a) input=<fixture generation> profile=<parse> states=<not applicable> steps=<not applicable>"
        : std::string{}, [&] { return parse("(?<S>(?&S)(?&S)|a)"); }), dense_inputs,
        progress.enabled() ? "(?<S>(?&S)(?&S)|a)" : std::string{}});
    result.push_back({"dense_nullable_catalan_words", progress.call("parse", progress.enabled()
        ? "fixture=dense_nullable_catalan_words expression=(?<S>(?&S)(?&S)|a|) input=<fixture generation> profile=<parse> states=<not applicable> steps=<not applicable>"
        : std::string{}, [&] { return parse("(?<S>(?&S)(?&S)|a|)"); }), dense_inputs,
        progress.enabled() ? "(?<S>(?&S)(?&S)|a|)" : std::string{}});
    result.push_back({"dense_catalan_distinct_suffix_controls",
                      progress.call("parse", progress.enabled()
                          ? "fixture=dense_catalan_distinct_suffix_controls expression=(?(DEFINE)(?<S>(?&S)(?&S)|a))(?&S)a|a(?&S)|(?&S)aa|a(?&S)a input=<fixture generation> profile=<parse> states=<not applicable> steps=<not applicable>"
                          : std::string{}, [&] { return parse("(?(DEFINE)(?<S>(?&S)(?&S)|a))(?&S)a|a(?&S)|(?&S)aa|a(?&S)a"); }),
                      dense_inputs, progress.enabled()
                          ? "(?(DEFINE)(?<S>(?&S)(?&S)|a))(?&S)a|a(?&S)|(?&S)aa|a(?&S)a" : std::string{}});
    return result;
}

struct Profile { std::string name; OccurrenceMatchOptions options; };
template<class Options>
void disable_new_quotients(Options& options) {
    if constexpr (requires { options.frontier_indexes; }) options.frontier_indexes = false;
    if constexpr (requires { options.aggregate_returns; }) options.aggregate_returns = false;
    if constexpr (requires { options.aggregate_suffix_returns; }) options.aggregate_suffix_returns = false;
    if constexpr (requires { options.local_regular_regions; }) options.local_regular_regions = false;
    if constexpr (requires { options.regular_cursor_frontiers; }) options.regular_cursor_frontiers = false;
}
template<class Options>
void append_new_profiles(std::vector<Profile>& result, Options current) {
    constexpr bool frontier = requires { current.frontier_indexes; };
    constexpr bool aggregate = requires { current.aggregate_returns; };
    constexpr bool suffix = requires { current.aggregate_suffix_returns; };
    constexpr bool local = requires { current.local_regular_regions; };
    constexpr bool cursor = requires { current.regular_cursor_frontiers; };
    constexpr unsigned bits = unsigned(frontier) + unsigned(aggregate) + unsigned(suffix) + unsigned(local) + unsigned(cursor);
    if constexpr (bits != 0) {
        for (unsigned mask = 0; mask < (1U << bits); ++mask) {
            auto option = current; unsigned bit = 0;
            if constexpr (frontier) option.frontier_indexes = (mask >> bit++) & 1U;
            if constexpr (aggregate) option.aggregate_returns = (mask >> bit++) & 1U;
            if constexpr (suffix) option.aggregate_suffix_returns = (mask >> bit++) & 1U;
            if constexpr (local) option.local_regular_regions = (mask >> bit++) & 1U;
            if constexpr (cursor) option.regular_cursor_frontiers = (mask >> bit++) & 1U;
            result.push_back({"new_quotient_combination_" + std::to_string(mask), option});
        }
    }
}
std::vector<Profile> profiles() {
    OccurrenceMatchOptions baseline;
    baseline.max_states = 2'000'000; baseline.max_steps = 8'000'000;
    baseline.prune = false; baseline.compress_returns = false;
    baseline.collapse_control = false; baseline.coalesce_returns = false;
    baseline.inline_terminals = false; baseline.merge_equivalent_nodes = false;
    disable_new_quotients(baseline);
    OccurrenceMatchOptions current;
    current.max_states = baseline.max_states; current.max_steps = baseline.max_steps;
    std::vector<Profile> result{{"materialized", baseline}, {"current", current}};
    auto option = current; option.prune = false; result.push_back({"unpruned", option});
    option = current; option.coalesce_returns = false; result.push_back({"uncoalesced", option});
    option = current; option.collapse_control = false; result.push_back({"uncollapsed", option});
    option = current; option.inline_terminals = false; result.push_back({"terminal_cells", option});
    option = current; option.merge_equivalent_nodes = false; result.push_back({"distinct_nodes", option});
    append_new_profiles(result, current);
    return result;
}

std::size_t checks = 0, languages = 0, greatest_rounds = 0, native_queries = 0;
std::size_t accepts = 0, rejects = 0, endpoint_pairs = 0;
std::size_t aggregate_word_tasks = 0, suffix_word_tasks = 0, regular_region_requests = 0;
std::string context;
void require(bool condition, std::string_view what) {
    ++checks;
    if (!condition) throw std::runtime_error(context + ": " + std::string(what));
}

std::vector<std::size_t> endpoints(const Matrix& matrix, std::size_t start) {
    std::vector<std::size_t> result;
    for (auto end = start; end < matrix.width; ++end) if (matrix.at(start, end)) result.push_back(end);
    return result;
}

template<class Statistics>
void record_activation(const Statistics& statistics) {
    if constexpr (requires { statistics.aggregate_word_tasks; })
        aggregate_word_tasks += statistics.aggregate_word_tasks;
    if constexpr (requires { statistics.suffix_word_tasks; })
        suffix_word_tasks += statistics.suffix_word_tasks;
    if constexpr (requires { statistics.regular_region_requests; })
        regular_region_requests += statistics.regular_region_requests;
}

template<class Options>
void require_activation(const Options&) {
    if constexpr (requires(Options value) { value.aggregate_returns; })
        require(aggregate_word_tasks > 0, "dense transparent aggregate was never exercised");
    if constexpr (requires(Options value) { value.aggregate_suffix_returns; })
        require(suffix_word_tasks > 0, "dense suffix aggregate was never exercised");
    if constexpr (requires(Options value) { value.local_regular_regions; })
        require(regular_region_requests > 0, "local regular region was never exercised");
}

std::string progress_detail(const Fixture& fixture, const std::string& input,
                            std::size_t input_index, std::string_view profile,
                            const OccurrenceMatchOptions* options, NodeId node,
                            std::size_t begin, std::size_t end) {
    if (!progress.enabled()) return {};
    std::ostringstream out;
    out << "fixture=" << fixture.name;
    if (!fixture.expression.empty()) out << " expression=" << escaped(fixture.expression);
    else out << " expression=<generated AST> seed=" << diagnostic_seed
             << " random_count=" << diagnostic_random_count << " generation_maximum=" << diagnostic_maximum;
    out << " input_index=";
    if (input_index == no_id) out << "<none>"; else out << input_index;
    out << " input=" << escaped(input) << " input_bytes=" << input.size()
        << " profile=" << profile << " ast_nodes=" << fixture.ast.nodes.size()
        << " node=";
    if (node == no_id) out << "<union>"; else out << node;
    out << " origin=" << begin << " interval=[" << begin << ',' << end << "]";
    if (options) out << " states=" << options->max_states << " steps=" << options->max_steps
                     << " max_occurrences=" << options->max_occurrences
                     << " max_repeat=" << options->max_repeat
                     << " max_call_depth=" << options->max_call_depth;
    else out << " states=<not applicable> steps=<not applicable>";
    return out.str();
}

std::string entry_detail(const std::vector<NodeId>& entries) {
    if (!progress.enabled()) return {};
    std::ostringstream out;
    out << " entries=[";
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (i) out << ',';
        out << entries[i];
    }
    out << ']';
    return out.str();
}

void check_fixture(const Fixture& fixture, const std::vector<Profile>& modes) {
    const auto& ast = fixture.ast;
    auto program = progress.call("AstMachineProgram setup", progress.enabled()
        ? progress_detail(fixture, {}, no_id, "<setup>", nullptr, ast.root, 0, 0)
            + " preparation=constructor_defaults"
        : std::string{}, [&] { return detail::AstMachineProgram(ast); }, true);
    std::vector<NodeId> entries{ast.root};
    for (std::size_t i = 1; i < ast.capture_nodes.size(); ++i)
        if (ast.capture_nodes[i] != no_id) entries.push_back(ast.capture_nodes[i]);
    // Reverse order and duplicate entries deliberately: a quotient must not
    // erase an accepting seed when it aliases another seed or internal node.
    std::reverse(entries.begin(), entries.end()); entries.push_back(entries.front());
    std::size_t input_index = 0;
    for (const auto& input : fixture.inputs) {
        auto case_scope = progress.scope("case", progress.enabled()
            ? progress_detail(fixture, input, input_index, "<all profiles>", &modes.front().options, ast.root, 0, input.size())
                + " profile_count=" + std::to_string(modes.size())
            : std::string{});
        const auto reference = progress.call("independent language", progress.enabled()
            ? progress_detail(fixture, input, input_index, "<independent reference>", nullptr, ast.root, 0, input.size())
                + " reference=finite_relation_least_fixed_point"
                + " reference_round_guard=" + std::to_string(ast.nodes.size() * (input.size() + 1) * (input.size() + 1) + 1)
            : std::string{}, [&] { return language(ast, input); }, input.size() > 8); ++languages;
        greatest_rounds = std::max(greatest_rounds, reference.rounds);
        const auto& root = reference.nodes[ast.root];
        const bool full = root.at(0, input.size());
        if (full) ++accepts; else ++rejects;
        context = fixture.name + " input=" + escaped(input) + " materialized witness";
        const auto witness = progress.call("first_match materialized witness", progress.enabled()
            ? progress_detail(fixture, input, input_index, modes.front().name, &modes.front().options, ast.root, 0, input.size())
            : std::string{}, [&] { return program.first_match(input, modes.front().options); }, input.size() > 8); ++native_queries;
        require(bool(witness) == full, "witness existence differs from relation");
        for (const auto& mode : modes) {
            context = fixture.name + " input=" + escaped(input) + " profile=" + mode.name;
            OccurrenceStatistics statistics;
            require(progress.call("full_match root exact demand", progress.enabled()
                ? progress_detail(fixture, input, input_index, mode.name, &mode.options, ast.root, 0, input.size())
                : std::string{}, [&] { return program.full_match(input, mode.options, &statistics); },
                input.size() > 8 && mode.name == "materialized") == full, "root exact demand"); ++native_queries;
            record_activation(statistics);
            for (std::size_t start = 0; start <= input.size(); ++start) {
                const auto expected = endpoints(root, start);
                endpoint_pairs += expected.size();
                require(progress.call("endpoints unrestricted root row", progress.enabled()
                    ? progress_detail(fixture, input, input_index, mode.name, &mode.options, ast.root, start, input.size())
                    : std::string{}, [&] { return program.endpoints(input, start, mode.options, &statistics); }) == expected,
                        "unrestricted root endpoint row start=" + std::to_string(start)); ++native_queries;
                record_activation(statistics);
            }
            require(progress.call("first_match witness identity", progress.enabled()
                ? progress_detail(fixture, input, input_index, mode.name, &mode.options, ast.root, 0, input.size())
                : std::string{}, [&] { return program.first_match(input, mode.options); }) == witness, "witness identity changed"); ++native_queries;
            bool union_expected = false;
            for (const auto entry : entries) union_expected |= reference.nodes[entry].at(0, input.size());
            require(progress.call("full_match_any accepting entry union", progress.enabled()
                ? progress_detail(fixture, input, input_index, mode.name, &mode.options, no_id, 0, input.size()) + entry_detail(entries)
                : std::string{}, [&] { return program.full_match_any(entries, input, mode.options, nullptr); }) == union_expected,
                    "multiple accepting entry union"); ++native_queries;
            require(!progress.call("full_match_any empty seed union", progress.enabled()
                ? progress_detail(fixture, input, input_index, mode.name, &mode.options, no_id, 0, input.size()) + " entries=[]"
                : std::string{}, [&] { return program.full_match_any({}, input, mode.options, nullptr); }), "empty seed union"); ++native_queries;
            // Each prefix is an exact demand while the root query above is
            // unrestricted. Original call targets survive this node entry.
            std::vector<std::size_t> exact_ends;
            for (std::size_t end = 0; end <= input.size(); ++end)
                if (input.size() <= 8 || end <= 1 || end == input.size() / 2 || end + 1 >= input.size())
                    exact_ends.push_back(end);
            for (const auto entry : entries) {
                for (const auto end : exact_ends) {
                    require(progress.call("full_match_node exact prefix", progress.enabled()
                        ? progress_detail(fixture, input, input_index, mode.name, &mode.options, entry, 0, end)
                        : std::string{}, [&] { return program.full_match_node(entry, std::string_view(input).substr(0, end), mode.options, nullptr); })
                                == reference.nodes[entry].at(0, end), "entry exact prefix demand"); ++native_queries;
                }
            }
            // Nonzero-origin exact interval demands check the correlation the
            // row API does not expose for arbitrary entries. Sampling every
            // eighth subject bounds cost independently of its grammar shape.
            if ((input_index % 8) == 0) {
                for (const auto entry : entries)
                    for (std::size_t begin = 1; begin <= input.size(); ++begin)
                        for (auto end = begin; end <= input.size(); ++end) {
                            if (input.size() > 8 && (begin != 1 && begin != input.size() / 2)) continue;
                            if (input.size() > 8 && (end != begin && end != begin + 1 && end != input.size())) continue;
                            require(progress.call("full_match_node exact nonzero interval", progress.enabled()
                                ? progress_detail(fixture, input, input_index, mode.name, &mode.options, entry, begin, end)
                                : std::string{}, [&] { return program.full_match_node(entry, std::string_view(input).substr(begin, end - begin),
                                                           mode.options, nullptr); })
                                        == reference.nodes[entry].at(begin, end), "entry exact nonzero interval"); ++native_queries;
                        }
            }
        }
        ++input_index;
    }
}
} // namespace

int main(int argc, char** argv) {
    try {
        std::size_t random_count = 40;
        unsigned maximum = 4;
        std::uint64_t seed = UINT64_C(0x6f726967696e7331);
        for (int arg = 1; arg < argc; ++arg) {
            const std::string option = argv[arg];
            if (arg + 1 >= argc) throw std::invalid_argument("options require a value");
            if (option == "--random") random_count = std::stoull(argv[++arg]);
            else if (option == "--length") maximum = static_cast<unsigned>(std::stoul(argv[++arg]));
            else if (option == "--seed") seed = std::stoull(argv[++arg], nullptr, 0);
            else throw std::invalid_argument("unknown option: " + option);
        }
        if (maximum > 6 || random_count > 1000 || !seed) throw std::invalid_argument("bounded oracle limits");
        diagnostic_seed = seed; diagnostic_maximum = maximum; diagnostic_random_count = random_count;
        auto cases = fixtures(maximum);
        Random random{seed};
        for (std::size_t i = 0; i < random_count; ++i) cases.push_back(random_fixture(random, i, maximum));
        const auto modes = profiles();
        for (const auto& fixture : cases) check_fixture(fixture, modes);
        context = "feature coverage"; require_activation(OccurrenceMatchOptions{});
        std::cout << "{\"status\":\"passed\",\"seed\":" << seed
                  << ",\"random_grammars\":" << random_count
                  << ",\"fixtures\":" << cases.size() << ",\"profiles\":" << modes.size()
                  << ",\"checks\":" << checks << ",\"native_queries\":" << native_queries
                  << ",\"oracle_languages\":" << languages << ",\"accept_subjects\":" << accepts
                  << ",\"reject_subjects\":" << rejects << ",\"endpoint_pairs\":" << endpoint_pairs
                  << ",\"maximum_fixed_point_rounds\":" << greatest_rounds
                  << ",\"aggregate_word_tasks\":" << aggregate_word_tasks
                  << ",\"suffix_word_tasks\":" << suffix_word_tasks
                  << ",\"regular_region_requests\":" << regular_region_requests << ",\"profile_names\":[";
        for (std::size_t i = 0; i < modes.size(); ++i) {
            if (i) std::cout << ',';
            std::cout << '\"' << modes[i].name << '\"';
        }
        std::cout << "]}\n";
    } catch (const std::exception& error) {
        std::cerr << "RELATION ORACLE FAILURE: " << context << ": " << error.what() << '\n'; return 1;
    }
}
