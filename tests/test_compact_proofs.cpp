#include "ast_machine.hpp"
#include "occurrence_analysis.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
std::size_t checks = 0, analyses = 0;
void require(bool value, const std::string& context) {
    ++checks;
    if (!value) throw std::runtime_error(context);
}
using Lengths = std::set<std::size_t>;
Lengths product(const Lengths& a, const Lengths& b) {
    Lengths result;
    for (const auto x : a) for (const auto y : b)
        if (x + y < 64) result.insert(x + y);
    return result;
}

// Integer enumeration is deliberately independent of shifts, rotations,
// powers and the production worklist. Lengths are nonnegative, so truncation
// cannot remove a derivation that later yields a shorter result.
std::vector<Lengths> oracle(const crank::Ast& ast) {
    std::vector<Lengths> lengths(ast.nodes.size());
    Lengths any;
    for (std::size_t n = 0; n < 64; ++n) any.insert(n);
    bool changed;
    do {
        changed = false;
        for (std::size_t id = 0; id < ast.nodes.size(); ++id) {
            const auto& node = ast.nodes[id];
            Lengths next;
            switch (node.kind) {
            case crank::Kind::Empty: case crank::Kind::Define: next.insert(0); break;
            case crank::Kind::CharSet:
                if (node.characters.any()) next.insert(1);
                break;
            case crank::Kind::Reference: next = any; break;
            case crank::Kind::Call:
                next = node.call_target < lengths.size() ? lengths[node.call_target] : any;
                break;
            case crank::Kind::Capture:
                next = node.children.empty() ? Lengths{0} : lengths[node.children.front()];
                break;
            case crank::Kind::Alt:
                for (const auto child : node.children)
                    next.insert(lengths[child].begin(), lengths[child].end());
                break;
            case crank::Kind::Concat:
                next.insert(0);
                for (const auto child : node.children) next = product(next, lengths[child]);
                break;
            case crank::Kind::Repeat: {
                if (node.children.empty()) { next = any; break; }
                const bool dynamic = node.use_count != crank::no_id || node.count_path != crank::no_id;
                const auto lower = dynamic ? 0 : node.lower;
                const auto upper = dynamic ? std::optional<std::size_t>{} : node.upper;
                if (upper && *upper < lower) break;
                if (lower > 64 || (upper && *upper > 64))
                    throw std::runtime_error("huge repeat is checked separately");
                Lengths power{0};
                for (std::size_t count = 0;; ++count) {
                    if (count >= lower) next.insert(power.begin(), power.end());
                    if (upper && count >= *upper) break;
                    const auto following = product(power, lengths[node.children.front()]);
                    if (following.empty()) break;
                    if (!upper && count >= lower && following == power) break;
                    power = following;
                }
                break;
            }
            }
            const auto before = lengths[id].size();
            lengths[id].insert(next.begin(), next.end());
            changed |= lengths[id].size() != before;
        }
    } while (changed);
    return lengths;
}

void check_pattern(const std::string& pattern) {
    const auto ast = crank::parse(pattern);
    const crank::OccurrenceAnalysis analysis(ast);
    const auto expected = oracle(ast);
    ++analyses;
    for (std::size_t id = 0; id < ast.nodes.size(); ++id) {
        const auto& value = analysis.length(id);
        for (std::size_t n = 0; n < 64; ++n) {
            const auto bit = (value.short_lengths & (std::uint64_t{1} << n)) != 0;
            require((value.productive && bit) == expected[id].contains(n),
                    pattern + ": exact bounded abstraction at node " + std::to_string(id));
            require(value.accepts(n) == expected[id].contains(n),
                    pattern + ": bounded acceptance at node " + std::to_string(id));
        }
        if (ast.nodes[id].kind != crank::Kind::Concat) continue;
        Lengths suffix{0};
        const auto& children = ast.nodes[id].children;
        for (std::size_t boundary = children.size();;) {
            const auto& summary = analysis.suffix_length(id, boundary);
            for (std::size_t n = 0; n < 64; ++n)
                require(summary.accepts(n) == suffix.contains(n), pattern + ": bounded suffix");
            if (!boundary) break;
            suffix = product(expected[children[--boundary]], suffix);
        }
    }
}

void huge_and_wrapped() {
    const auto maximum = std::numeric_limits<std::size_t>::max();
    for (const auto& pattern : {
            std::string("(?:a?){") + std::to_string(maximum) + "}",
            std::string("(?:aa?){") + std::to_string(maximum) + "}",
            std::string("(?:a*){") + std::to_string(maximum) + "}",
            std::string("(?:a*){0,") + std::to_string(maximum) + "}"}) {
        const auto ast = crank::parse(pattern);
        const crank::OccurrenceAnalysis analysis(ast);
        const auto& summary = analysis.length(ast.root);
        const bool nullable = pattern.find("aa?") == std::string::npos;
        for (std::size_t n = 0; n < 64; ++n)
            require(summary.accepts(n) == nullable, "huge counts preserve all short nullable words");
    }
    const auto ast = crank::parse("(?:a{63}|a{64})*");
    const crank::OccurrenceAnalysis analysis(ast);
    const auto& summary = analysis.length(ast.root);
    require(summary.short_lengths == (std::uint64_t{1} | (std::uint64_t{1} << 63)),
            "length 64 must not wrap into the short prefix");
    require(summary.accepts(64) && summary.accepts(126) && summary.accepts(127),
            "short-prefix truncation never rejects longer lengths");
    const crank::OccurrenceLength old_aggregate{true, 0, 0, 1, ~std::uint64_t{0}};
    require(old_aggregate.accepts(37), "omitted new field remains conservative");
}

void native_queries() {
    for (const std::string pattern : {
            "(?:a{6}|a{10})*", "(?<S>a{6}(?&S)|a{10}(?&S)|)",
            "(?<S>(?&S)(?&S)|a{6}|a{10})", "(?:a{6}|a{10})*b(?:a{6}|a{10})*"}) {
        const auto ast = crank::parse(pattern);
        const auto expected = oracle(ast);
        crank::detail::AstMachineProgram program(ast);
        for (std::size_t n = 0; n < 64; ++n) {
            std::string subject(n, 'a');
            if (pattern.find('b') != std::string::npos && n) subject[n / 2] = 'b';
            crank::OccurrenceMatchOptions options;
            options.prune = false;
            const auto actual = program.full_match(subject, options);
            options.prune = true;
            require(program.full_match(subject, options) == actual,
                    "pruning preserves native recognition");
            if (pattern.find('b') == std::string::npos)
                require(actual == expected[ast.root].contains(n), "unary native oracle");
        }
    }
    const auto ast = crank::parse("(?<S>(?&S)(?&S)|a{6}|a{10})");
    crank::detail::AstMachineProgram program(ast);
    crank::OccurrenceStatistics stats;
    require(!program.full_match(std::string(14, 'a'), {}, &stats), "small semigroup hole rejected");
    require(stats.states == 0 && stats.work_steps <= 1,
            "short proof rejects before creating native requests");
}
}

int main() {
    try {
        for (const std::string pattern : {
                "", "a", "a{64}", "a{63}a", "(?:a{6}|a{10})*", "(?:a{7}|a{11})+",
                "(?:a?|a{3}){2,5}", "(?:a{63}|a{64})*", "(?:a{63}|a){2}",
                "(?<S>(?&S))", "(?<S>(?&S)|)", "(?<S>(?&S)(?&S)|a{6}|a{10})",
                "(?<S>a{6}(?&S)|a{10}(?&S)|)", "(?<S>a?(?&S)b?|)",
                "(?(DEFINE)(?<A>aa(?&B)|)(?<B>(?&A)bbb|a))(?&A)",
                "(?(DEFINE)(?<A>(?&B))(?<B>(?&A)))(?&A)",
                "(?<x>a*)\\k<x>", "a{#n}b{=n}"}) check_pattern(pattern);
        for (unsigned left = 2; left <= 9; ++left)
            for (unsigned right = 3; right <= 12; ++right)
                check_pattern("(?<S>a{" + std::to_string(left) + "}(?&S)|a{" +
                              std::to_string(right) + "}(?&S)|)");
        huge_and_wrapped();
        native_queries();
        std::cout << "compact proofs: " << checks << " checks, " << analyses << " analyses\n";
    } catch (const std::exception& e) {
        std::cerr << "compact proof failure: " << e.what() << '\n';
        return 1;
    }
}
