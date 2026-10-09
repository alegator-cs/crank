#include "occurrence_analysis.hpp"
#include "occurrence_matcher.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
std::size_t checks = 0, analyses = 0, suffixes = 0, oracle_lengths = 0;
std::size_t membership_cases = 0, endpoint_cases = 0, witness_cases = 0;

void require(bool condition, const std::string& context) {
    ++checks;
    if (!condition) throw std::runtime_error(context);
}

#ifndef CRANK_SUFFIX_BASELINE
using Lengths = std::set<std::size_t>;
Lengths product(const Lengths& left, const Lengths& right, std::size_t maximum) {
    Lengths result;
    for (const auto a : left)
        for (const auto b : right)
            if (a <= maximum && b <= maximum - a) result.insert(a + b);
    return result;
}

// A finite integer-language least fixed point, independent of the summary's
// minima, GCDs and modulo-64 masks. Since lengths never decrease, truncating
// every intermediate sum at maximum cannot hide a shorter derivation.
std::vector<Lengths> bounded_lengths(const crank::Ast& ast, std::size_t maximum) {
    std::vector<Lengths> values(ast.nodes.size());
    Lengths unknown;
    for (std::size_t n = 0; n <= maximum; ++n) unknown.insert(n);
    bool changed;
    do {
        changed = false;
        for (std::size_t id = 0; id < ast.nodes.size(); ++id) {
            const auto& node = ast.nodes[id];
            Lengths next;
            switch (node.kind) {
            case crank::Kind::Empty:
            case crank::Kind::Define:
                next.insert(0);
                break;
            case crank::Kind::CharSet:
                if (node.characters.any() && maximum) next.insert(1);
                break;
            case crank::Kind::Reference:
                next = unknown;
                break;
            case crank::Kind::Call:
                next = node.call_target < values.size() ? values[node.call_target] : unknown;
                break;
            case crank::Kind::Capture:
                next = node.children.empty() ? Lengths{0} : values.at(node.children.front());
                break;
            case crank::Kind::Concat:
                next.insert(0);
                for (const auto child : node.children) next = product(next, values.at(child), maximum);
                break;
            case crank::Kind::Alt:
                for (const auto child : node.children)
                    next.insert(values.at(child).begin(), values.at(child).end());
                break;
            case crank::Kind::Repeat: {
                if (node.children.empty()) { next = unknown; break; }
                const bool dynamic = node.use_count != crank::no_id || node.count_path != crank::no_id;
                const auto lower = dynamic ? 0 : node.lower;
                const auto upper = dynamic ? std::optional<std::size_t>{} : node.upper;
                if (upper && *upper < lower) break;
                if (lower > 8 || (upper && *upper > 8))
                    throw std::runtime_error("bounded oracle received a huge repeat");
                Lengths exact{0};
                for (std::size_t count = 0; ; ++count) {
                    if (count >= lower) next.insert(exact.begin(), exact.end());
                    if (upper && count >= *upper) break;
                    auto following = product(exact, values.at(node.children.front()), maximum);
                    if (following.empty()) break;
                    if (!upper && count >= lower && following == exact) break;
                    exact = std::move(following);
                }
                break;
            }
            }
            const auto before = values[id].size();
            values[id].insert(next.begin(), next.end());
            changed |= values[id].size() != before;
        }
    } while (changed);
    return values;
}

void sound_suffixes(const crank::Ast& ast, std::size_t maximum, const std::string& label) {
    const crank::OccurrenceAnalysis analysis(ast);
    const auto oracle = bounded_lengths(ast, maximum);
    ++analyses;
    for (std::size_t id = 0; id < ast.nodes.size(); ++id) {
        const auto& node = ast.nodes[id];
        if (node.kind != crank::Kind::Concat) continue;
        for (std::size_t boundary = 0; boundary <= node.children.size(); ++boundary) {
            // Deliberately recompute each suffix in forward order from actual
            // integer lengths instead of duplicating the compiler's fold.
            Lengths expected{0};
            for (auto i = boundary; i < node.children.size(); ++i)
                expected = product(expected, oracle.at(node.children[i]), maximum);
            const auto& summary = analysis.suffix_length(id, boundary);
            ++suffixes;
            require(summary.minimum == analysis.suffix_minimum(id).at(boundary),
                    label + ": legacy minimum agrees at boundary " + std::to_string(boundary));
            for (const auto length : expected) {
                ++oracle_lengths;
                require(summary.accepts(length), label + ": removed derivable suffix length " +
                        std::to_string(length) + " at node " + std::to_string(id) +
                        " boundary " + std::to_string(boundary));
            }
            if (boundary == node.children.size())
                require(summary.accepts(0) && !summary.accepts(1) && summary.modulus == 0,
                        label + ": terminal boundary is exactly epsilon");
        }
    }
}

crank::NodeId append_concat(crank::Ast& ast, std::vector<crank::NodeId> children) {
    crank::Node concat;
    concat.kind = crank::Kind::Concat;
    concat.children = std::move(children);
    const auto id = ast.nodes.size();
    ast.nodes.push_back(std::move(concat));
    return id;
}

void arithmetic_fixtures() {
    for (const std::string pattern : {
            "(?<A>a*)(?<B>(?:aa)*)(?<C>(?:bb)*)",
            "(?<A>a*)(?<B>aaaa|a)(?<C>bbbb|b)",
            "(?(DEFINE)(?<A>a(?&A)|)(?<B>aa(?&B)|)(?<C>bb(?&C)|))(?&A)(?&B)(?&C)",
            "(?(DEFINE)(?<A>a*)(?<B>(?&B)aa|)(?<C>(?&C)bb|))(?&A)(?&B)(?&C)",
            "(?(DEFINE)(?<B>(?&C)|)(?<C>aa(?&B)))(?:a|aa)(?&B)(?&C)",
            "(?(DEFINE)(?<B>(?&B)|aa(?&B)|))a*(?&B)(?:bb)*",
            "(?(DEFINE)(?<dead>(?&dead)))a*(?&dead)(?:bb)*",
            "(?(DEFINE)(?<dead>(?&dead)))a*(?&dead){0}(?:bb)*",
            "(?(DEFINE)(?<B>(?&B)(?&B)|aa|))a*(?&B)(?:bb){1,3}",
            "(?(DEFINE)(?<B>aa(?&C)|)(?<C>(?&B)bb|a))a*(?&B)(?&C)",
            "(?:a{0,3})(?:(?:aa|){2,3})(?:b{0,2})",
            "(?:a{0,3})(?:aaaa|){3,}(?:bb|){2,}",
            "(?<x>aa|a)\\k<x>(?:bb)*",
            "a*(?:aaaa){#n:1,3}b{=n}",
            "a*(?:aaaa){#n:1,3}b{=#0.@count}"})
        sound_suffixes(crank::parse(pattern), 80, pattern);

    {
        const auto ast = crank::parse("(?<A>a*)(?<B>aaaa|a)(?<C>bbbb|b)");
        require(ast.nodes.at(ast.root).kind == crank::Kind::Concat &&
                ast.nodes.at(ast.root).children.size() == 3, "disjunctive fixture has A B C boundaries");
        const crank::OccurrenceAnalysis analysis(ast);
        ++analyses;
        const auto& suffix = analysis.suffix_length(ast.root, 1);
        require(suffix.productive && suffix.minimum == 2 && suffix.modulus == 3,
                "combined suffix retains nontrivial congruence");
        for (std::size_t n = 0; n <= 16; ++n)
            require(suffix.accepts(n) == (n == 2 || n == 5 || n == 8),
                    "finite B C suffix keeps all independent choices and no nearby extra lengths");
        require(analysis.length(ast.root).accepts(3) && !suffix.accepts(3),
                "root admission does not imply admission of an internal remaining length");
    }
    {
        const auto ast = crank::parse("(?<A>a*)(?<B>aaaa|a|)(?<C>bbbb|b|)");
        const crank::OccurrenceAnalysis analysis(ast);
        ++analyses;
        const auto& suffix = analysis.suffix_length(ast.root, 1);
        require(suffix.minimum == 0 && suffix.modulus == 1,
                "disjunctive suffix cannot be pruned by its single-congruence hull");
        for (const auto n : {0U, 1U, 2U, 4U, 5U, 8U})
            require(suffix.accepts(n), "modulo mask retains independent nullable choices");
        for (const auto n : {3U, 6U, 7U, 9U})
            require(!suffix.accepts(n), "modulo mask rejects holes lost by GCD alone");
    }
    {
        auto ast = crank::parse("(?<dead>(?&dead))");
        const auto dead = ast.root;
        crank::Node character;
        character.kind = crank::Kind::CharSet;
        character.characters.set(static_cast<unsigned char>('a'));
        const auto letter = ast.nodes.size();
        ast.nodes.push_back(character);
        const auto empty = ast.nodes.size();
        ast.nodes.emplace_back();
        ast.root = append_concat(ast, {dead, letter, empty});
        const crank::OccurrenceAnalysis analysis(ast);
        ++analyses;
        require(!analysis.suffix_length(ast.root, 0).productive,
                "unproductive first child makes the whole concat unproductive");
        require(analysis.suffix_length(ast.root, 1).accepts(1),
                "an unproductive prefix does not poison a productive suffix");
        sound_suffixes(ast, 16, "productive suffix after dead recursive prefix");
    }
    {
        auto ast = crank::parse("a");
        crank::Node repeat;
        repeat.kind = crank::Kind::Repeat;
        repeat.children = {ast.root};
        repeat.lower = std::numeric_limits<std::size_t>::max();
        repeat.upper = repeat.lower;
        const auto huge = ast.nodes.size();
        ast.nodes.push_back(repeat);
        ast.root = append_concat(ast, {huge, ast.root});
        const crank::OccurrenceAnalysis analysis(ast);
        ++analyses;
        const auto& suffix = analysis.suffix_length(ast.root, 0);
        require(suffix.minimum == std::numeric_limits<std::size_t>::max(),
                "overflowed suffix minimum saturates");
        require(!suffix.accepts(0) && !suffix.accepts(std::numeric_limits<std::size_t>::max()),
                "SIZE_MAX plus one is not misrepresented as zero or the saturation sentinel");
        require(analysis.suffix_length(ast.root, 1).accepts(1),
                "overflow in a preceding child leaves the suffix singleton intact");
    }
    for (const bool path : {false, true}) {
        auto ast = crank::parse("a*(?:aa){2}(?:bbbb)*");
        bool found = false;
        for (auto& node : ast.nodes) {
            if (node.kind != crank::Kind::Repeat || node.lower != 2) continue;
            if (path) node.count_path = 0;
            else node.use_count = 0;
            found = true;
        }
        require(found, "dynamic fixture contains the repeat being made unresolved");
        sound_suffixes(ast, 80, path ? "unresolved path count in suffix" : "unresolved scalar count in suffix");
    }
}

std::uint32_t random_state = 0x76b8c51dU;
std::uint32_t next_random() {
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}
std::string expression(unsigned depth) {
    if (!depth) {
        static const std::vector<std::string> atoms{"", "a", "bb", "aaaa", "[ab]", "a?"};
        return atoms[next_random() % atoms.size()];
    }
    const auto choice = next_random() % 5;
    const auto first = expression(depth - 1);
    if (choice == 0) return "(?:" + first + "|" + expression(depth - 1) + ")";
    if (choice == 1) return "(?:" + first + ")(?:" + expression(depth - 1) + ")";
    if (choice == 2) return "(?:" + first + "){0,3}";
    if (choice == 3) return "(?:" + first + "){2}";
    return "(?:" + first + ")*";
}
void generated_arithmetic() {
    for (unsigned i = 0; i < 32; ++i) {
        const auto pattern = "(?<A>" + expression(2) + ")(?<B>" + expression(2) +
            ")(?<C>" + expression(2) + ")";
        sound_suffixes(crank::parse(pattern), 64, "generated concat " + std::to_string(i));
    }
    for (unsigned i = 0; i < 24; ++i) {
        const auto left = std::string(next_random() % 5, 'a');
        const auto right = std::string(next_random() % 5, 'b');
        const auto base = std::string(next_random() % 3, 'a');
        const auto pattern = "(?(DEFINE)(?<B>" + left + "(?&C)" + right + "|" + base +
            ")(?<C>(?&B)" + (i % 3 == 0 ? "(?&B){0,2}" : "") + "|" +
            (i % 2 == 0 ? "" : "b") + "))a*(?&B)(?&C)";
        sound_suffixes(crank::parse(pattern), 64, "generated recursive suffix " + std::to_string(i));
    }
}
#endif

struct Symbol { bool terminal; std::size_t nonterminal; std::string text; };
Symbol n(std::size_t id) { return {false, id, {}}; }
Symbol t(std::string text) { return {true, 0, std::move(text)}; }
struct Production { std::size_t lhs; std::vector<Symbol> rhs; };
using Language = std::set<std::string>;

// Explicit grammar productions are a second independent oracle. This finite
// language calculation has no AST nodes, input positions, demands or returns.
std::vector<Language> bounded_languages(std::size_t nonterminals,
                                      const std::vector<Production>& productions,
                                      std::size_t maximum) {
    std::vector<Language> languages(nonterminals);
    bool changed;
    do {
        changed = false;
        for (const auto& production : productions) {
            Language partial{std::string{}};
            for (const auto& symbol : production.rhs) {
                const Language terminal{symbol.text};
                const auto& suffix = symbol.terminal ? terminal : languages.at(symbol.nonterminal);
                Language next;
                for (const auto& a : partial)
                    for (const auto& b : suffix)
                        if (a.size() + b.size() <= maximum) next.insert(a + b);
                partial = std::move(next);
            }
            auto& destination = languages.at(production.lhs);
            const auto before = destination.size();
            destination.insert(partial.begin(), partial.end());
            changed |= before != destination.size();
        }
    } while (changed);
    return languages;
}

struct Fixture {
    std::string name, pattern;
    std::size_t nonterminals;
    std::vector<Production> productions;
};

std::vector<Fixture> fixtures() {
    // S=0, A=1, B=2, C=3. The first fixture deliberately has variable B and C:
    // no singleton suffix can demand B's endpoint after an A completion.
    const std::vector<Production> even_tail{
        {0, {n(1), n(2), n(3)}}, {1, {t("a"), n(1)}}, {1, {}},
        {2, {t("aa"), n(2)}}, {2, {}}, {3, {t("bb"), n(3)}}, {3, {}}
    };
    auto shared = even_tail;
    shared.push_back({0, {n(1), n(2), n(3), t("b")}});
    auto nullable = even_tail;
    nullable.push_back({2, {n(2)}});
    auto dead = even_tail;
    dead.erase(std::remove_if(dead.begin(), dead.end(), [](const auto& p) { return p.lhs == 2; }), dead.end());
    dead.push_back({2, {n(2)}});
    return {
        {"variable even suffix",
         "(?(DEFINE)(?<A>a(?&A)|)(?<B>aa(?&B)|)(?<C>bb(?&C)|))(?&A)(?&B)(?&C)", 4, even_tail},
        {"left recursive suffix",
         "(?(DEFINE)(?<A>a(?&A)|)(?<B>(?&B)aa|)(?<C>(?&C)bb|))(?&A)(?&B)(?&C)", 4, even_tail},
        {"nullable cyclic suffix",
         "(?(DEFINE)(?<A>a(?&A)|)(?<B>(?&B)|aa(?&B)|)(?<C>bb(?&C)|))(?&A)(?&B)(?&C)", 4, nullable},
        {"shared prefix under opposite suffix parity",
         "(?(DEFINE)(?<A>a(?&A)|)(?<B>aa(?&B)|)(?<C>bb(?&C)|))(?:(?&A)(?&B)(?&C)|(?&A)(?&B)(?&C)b)", 4, shared},
        {"reversed opposite parity branches",
         "(?(DEFINE)(?<A>a(?&A)|)(?<B>aa(?&B)|)(?<C>bb(?&C)|))(?:(?&A)(?&B)(?&C)b|(?&A)(?&B)(?&C))", 4, shared},
        {"unproductive suffix",
         "(?(DEFINE)(?<A>a(?&A)|)(?<B>(?&B))(?<C>bb(?&C)|))(?&A)(?&B)(?&C)", 4, dead},
        {"recursive nesting followed by variable suffixes",
         "(?(DEFINE)(?<A>a(?&A)b|)(?<B>aa(?&B)|)(?<C>bb(?&C)|))(?&A)(?&B)(?&C)", 4,
         {{0, {n(1), n(2), n(3)}}, {1, {t("a"), n(1), t("b")}}, {1, {}},
          {2, {t("aa"), n(2)}}, {2, {}}, {3, {t("bb"), n(3)}}, {3, {}}}},
        {"GCD one suffix residue holes",
         "(?<A>a*)(?<B>aaaa|a|)(?<C>bbbb|b|)", 4,
         {{0, {n(1), n(2), n(3)}}, {1, {t("a"), n(1)}}, {1, {}},
          {2, {t("aaaa")}}, {2, {t("a")}}, {2, {}},
          {3, {t("bbbb")}}, {3, {t("b")}}, {3, {}}}},
        {"shared request under different exact enclosing ends",
         "(?(DEFINE)(?<A>a*)(?<B>(?:aa)*)(?<C>(?:bb)*)(?<S>(?&A)(?&B)(?&C)))(?:(?&S)|(?&S)b)", 4, shared},
        {"nested nullable captures preserve scalar effects",
         "(?<A>a*)(?<B>(?<inside>(?:aa)*))(?<C>(?:bb)*)", 4, even_tail}
    };
}

std::vector<std::string> words(std::string_view alphabet, unsigned maximum) {
    std::vector<std::string> result{std::string{}};
    std::size_t begin = 0, end = 1;
    for (unsigned length = 0; length < maximum; ++length) {
        for (auto i = begin; i < end; ++i)
            for (char byte : alphabet) result.push_back(result[i] + byte);
        begin = end;
        end = result.size();
    }
    return result;
}

crank::OccurrenceMatchOptions strategy(unsigned mask, bool prune) {
    crank::OccurrenceMatchOptions options;
    options.compress_returns = (mask & 1U) != 0;
    options.collapse_control = (mask & 2U) != 0;
    options.prune = prune;
    return options;
}

void runtime_equivalence() {
    for (const auto& fixture : fixtures()) {
        const crank::OccurrenceProgram program(crank::parse(fixture.pattern));
        require(program.supports_shared_calls(), fixture.name + ": eligible for native sharing");
        const auto language = bounded_languages(fixture.nonterminals, fixture.productions, 5).at(0);
        for (const auto& subject : words("ab", 5)) {
            const bool expected = language.contains(subject);
            const auto label = fixture.name + " subject '" + subject + "'";
            ++membership_cases;
            for (unsigned mask = 0; mask < 4; ++mask) {
                for (const bool prune : {false, true}) {
                    crank::OccurrenceStatistics statistics;
                    require(program.full_match(subject, strategy(mask, prune), &statistics) == expected,
                            label + ": independent membership mask=" + std::to_string(mask) +
                            " prune=" + std::to_string(prune));
                    require(!statistics.limit_reached && statistics.proof_nodes == 0,
                            label + ": recognition completes without materializing witnesses");
                }
            }
            for (std::size_t start = 0; start <= subject.size(); ++start) {
                std::vector<std::size_t> expected_ends;
                for (auto end = start; end <= subject.size(); ++end)
                    if (language.contains(subject.substr(start, end - start))) expected_ends.push_back(end);
                ++endpoint_cases;
                for (const unsigned mask : {0U, 3U})
                    for (const bool prune : {false, true})
                        require(program.endpoints(subject, start, strategy(mask, prune)) == expected_ends,
                                label + ": independent unanchored endpoints at " + std::to_string(start));
            }
            if (subject.size() <= 3) {
                const auto witness = program.first_match(subject, strategy(0, true));
                require(witness.has_value() == expected, label + ": first witness existence");
                for (unsigned mask = 1; mask < 4; ++mask)
                    require(program.first_match(subject, strategy(mask, true)) == witness,
                            label + ": recognition-only strategies preserve scalar captures");
                ++witness_cases;
            }
        }
        // The enclosing subject's tail is an upper bound for endpoints, never
        // an exact demand. Invalid padding must not remove a valid prefix.
        for (const std::string subject : {"aaz", "aaabz", "aabbbz", "z"}) {
            std::vector<std::size_t> expected_ends;
            for (std::size_t end = 0; end <= subject.size(); ++end)
                if (language.contains(subject.substr(0, end))) expected_ends.push_back(end);
            require(program.endpoints(subject) == expected_ends,
                    fixture.name + ": arbitrary padding must not become an exact suffix demand");
            ++endpoint_cases;
        }
    }
}

void statistics_record(const crank::OccurrenceStatistics& s) {
    std::cout << " stats=" << s.states << ',' << s.work_steps << ',' << s.down_steps << ',' << s.up_steps
        << ',' << s.pruned << ',' << s.arithmetic_pruned << ',' << s.path_checks << ',' << s.forward_obligations
        << ',' << s.max_call_depth << ',' << s.occurrences << ',' << s.memo_entries << ',' << s.memo_hits
        << ',' << s.return_links << ',' << s.completions << ',' << s.replay_steps << ',' << s.proof_nodes
        << ',' << s.max_pending << ',' << s.compression_lookups << ',' << s.compression_hops
        << ',' << s.compressed_completions << ',' << s.compression_cycles << ',' << s.control_redirects
        << ',' << s.ordered_positions << ',' << s.duplicate_completions << ',' << s.limit_reached;
}

// Compile with CRANK_SUFFIX_BASELINE against the frozen pre-change analysis
// header and AST core, then compare this mode's output byte-for-byte. This
// checks actual scalar proof/capture ordering and work scheduling, rather
// than assuming that Boolean language equivalence implies witness identity.
void witness_records() {
    for (const auto& fixture : fixtures()) {
        const crank::OccurrenceProgram program(crank::parse(fixture.pattern));
        for (const auto& subject : words("ab", 5)) {
            crank::OccurrenceStatistics statistics;
            const auto witness = program.first_match(subject, {}, &statistics);
            std::cout << fixture.name << " subject='" << subject << "' match=" << witness.has_value();
            if (witness) {
                std::cout << " end=" << witness->end << " captures=";
                for (const auto& span : witness->captures) {
                    if (span) std::cout << span->begin << ':' << span->end;
                    else std::cout << '-';
                    std::cout << ',';
                }
            }
            statistics_record(statistics);
            std::cout << '\n';
        }
    }
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--witness-record") {
            witness_records();
            return 0;
        }
        if (argc != 1) throw std::runtime_error("usage: test_suffix [--witness-record]");
#ifndef CRANK_SUFFIX_BASELINE
        arithmetic_fixtures();
        generated_arithmetic();
#endif
        runtime_equivalence();
        std::cout << "suffix checks=" << checks << " analyses=" << analyses
                  << " suffixes=" << suffixes << " oracle_lengths=" << oracle_lengths
                  << " membership=" << membership_cases << " endpoints=" << endpoint_cases
                  << " witnesses=" << witness_cases << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "suffix test failure: " << error.what() << '\n';
        return 1;
    }
}
