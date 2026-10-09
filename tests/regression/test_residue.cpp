#include "occurrence_analysis.hpp"
#include "occurrence_matcher.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
std::size_t checks = 0, analyses = 0, oracle_lengths = 0;
std::size_t generated_cases = 0, membership_cases = 0, history_cases = 0;

void require(bool condition, const std::string& context) {
    ++checks;
    if (!condition) throw std::runtime_error(context);
}

using Lengths = std::set<std::size_t>;

Lengths product(const Lengths& left, const Lengths& right, std::size_t maximum) {
    Lengths result;
    for (const auto a : left)
        for (const auto b : right)
            if (a <= maximum && b <= maximum - a) result.insert(a + b);
    return result;
}

// This oracle enumerates actual integer lengths, not modular residues or GCDs.
// Truncation is sound: AST concatenation only adds nonnegative lengths. Calls
// are interpreted by the least fixed point of finite derivations. References
// deliberately range over every bounded length, and unresolved counts over
// every nonnegative number of iterations, independently of runtime bindings.
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
                // Oracle fixtures use small finite counts. Huge-count tests
                // below use separately justified languages instead.
                if (lower > 16 || (upper && *upper > 16))
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
            changed |= before != values[id].size();
        }
    } while (changed);
    return values;
}

void sound_lengths(const crank::Ast& ast, std::size_t maximum, const std::string& label) {
    const crank::OccurrenceAnalysis analysis(ast);
    const auto oracle = bounded_lengths(ast, maximum);
    ++analyses;
    for (std::size_t id = 0; id < ast.nodes.size(); ++id)
        for (const auto length : oracle[id]) {
            ++oracle_lengths;
            require(analysis.length(id).accepts(length), label + ": removed derivable length " +
                    std::to_string(length) + " at AST node " + std::to_string(id));
        }
}

crank::OccurrenceLength root_length(const std::string& pattern) {
    const auto ast = crank::parse(pattern);
    const crank::OccurrenceAnalysis analysis(ast);
    ++analyses;
    return analysis.length(ast.root);
}

void distinguishing_residues() {
    for (const std::string pattern : {
            "(?<S>aa(?&S)bb||a)",
            "(?<S>(?&S)aaaa||a)",
            "(?(DEFINE)(?<A>aa(?&B)||a)(?<B>(?&A)bb))(?&A)"}) {
        const auto length = root_length(pattern);
        require(length.productive && length.minimum == 0 && length.modulus == 1,
                pattern + ": single-congruence hull deliberately loses the distinction");
        for (std::size_t n = 0; n <= 511; ++n)
            require(length.accepts(n) == (n % 4 <= 1), pattern + ": two residues modulo four");
    }

    const auto bounded = root_length("(?:aaaa){0,2}");
    for (const auto n : {0U, 4U, 8U}) require(bounded.accepts(n), "bounded powers preserve possible lengths");
    for (const auto n : {12U, 16U, 60U}) require(!bounded.accepts(n), "bounded powers retain finite residue union");

    const auto fixed = root_length("(?:a{64}|a{65}){2}");
    for (const auto n : {128U, 129U, 130U}) require(fixed.accepts(n), "independent fixed-repeat choices");
    require(!fixed.accepts(131), "fixed-repeat residue set is not immediately widened to its GCD");

    const auto wrap = root_length("(?:a{63}|a{64})(?:aa|aaaa)");
    for (const auto n : {65U, 66U, 67U, 68U}) require(wrap.accepts(n), "cyclic convolution crosses word boundary");
    require(!wrap.accepts(69), "cyclic convolution keeps the gap after wraparound");

    const auto wrapped_repeat = root_length("(?:a{63}|a{64}){2}");
    for (const auto n : {126U, 127U, 128U}) require(wrapped_repeat.accepts(n), "power crosses residue zero");
    require(!wrapped_repeat.accepts(129), "power preserves residues near zero");

    const auto translated = root_length("(?<S>a{64}(?&S)||a)a{63}");
    for (std::size_t n = 63; n <= 319; ++n)
        require(translated.accepts(n) == (n % 64 == 63 || n % 64 == 0),
                "recursive union followed by a translated suffix");

    const auto nullable = root_length("(?:aaaa?)*");
    for (std::size_t n = 12; n <= 256; ++n)
        require(nullable.accepts(n), "unbounded sums eventually cover every residue");

    const auto positive_lower = root_length("(?:aaaa|aaaaaa){3,}");
    const auto nullable_lower = root_length("(?:aaaa|){3,}");
    const auto empty_lower = root_length("(?:){3,}");
    for (std::size_t n = 0; n <= 256; ++n) {
        require(positive_lower.accepts(n) == (n >= 12 && n % 2 == 0),
                "unbounded repeat retains its positive lower-count prefix");
        require(nullable_lower.accepts(n) == (n % 4 == 0),
                "nullable body satisfies a positive lower count without consuming");
        require(empty_lower.accepts(n) == (n == 0),
                "an arbitrarily large positive number of empty bodies remains epsilon");
    }

    // A fixed modulus of 64 cannot retain this modulo-three union: stepping
    // by three visits every bit. The bounded prefix additionally excludes
    // impossible actual lengths below 64; larger lengths remain approximate.
    const auto odd_period = root_length("(?<S>a(?&S)bb||a)");
    require(odd_period.modulus == 1, "odd-period union also has the unrestricted GCD hull");
    require(odd_period.residues == ~std::uint64_t{0}, "odd period saturates residue bits");
    for (std::size_t n = 0; n <= 256; ++n)
        require(odd_period.accepts(n) == (n >= 64 || n % 3 <= 1),
                "bounded prefix refines a saturated residue abstraction");
    const crank::OccurrenceProgram odd_program(crank::parse("(?<S>a(?&S)bb||a)"));
    require(!odd_program.full_match("aa"), "bounded prefix rejects an impossible short length");
}

std::uint32_t random_state = 0x52e51d9bU;
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

void generated_soundness() {
    for (unsigned i = 0; i < 48; ++i) {
        const auto pattern = expression(3);
        sound_lengths(crank::parse(pattern), 96, "generated expression " + std::to_string(i) + " " + pattern);
        ++generated_cases;
    }
    for (unsigned i = 0; i < 32; ++i) {
        const auto left = std::string(next_random() % 5, 'a');
        const auto right = std::string(next_random() % 5, 'b');
        const auto base = std::string(next_random() % 4, 'a');
        const auto other = std::string(next_random() % 4, 'b');
        const auto suffix = i % 3 == 0 ? "(?&A){0,2}" : "";
        const auto pattern = "(?(DEFINE)(?<A>" + left + "(?&B)" + right + "|" + base +
            ")(?<B>(?&C)|" + other + ")(?<C>(?&A)" + suffix + "|" +
            (i % 2 == 0 ? std::string{} : "a") + "))(?&A)";
        sound_lengths(crank::parse(pattern), 80, "generated recursive grammar " + std::to_string(i));
        ++generated_cases;
    }
    for (const std::string pattern : {
            "(?<S>(?&S))", "(?(DEFINE)(?<A>(?&B))(?<B>(?&A)))(?&A)",
            "(?(DEFINE)(?<A>(?&B)|)(?<B>(?&A)|aaaa))(?&A)",
            "(?<S>(?&S)(?&S)|a|)", "(?<S>(?&S){0,2}aaaa|)",
            "(?<S>(?:aa(?&S)bb)*)", "(?(DEFINE)(?<unused>(?&unused)))a"})
        sound_lengths(crank::parse(pattern), 80, pattern);

    for (const std::string pattern : {
            "(?<S>(?&S))", "(?(DEFINE)(?<A>(?&B))(?<B>(?&A)))(?&A)"}) {
        const auto length = root_length(pattern);
        require(!length.productive, pattern + ": a bare recursion cycle is not epsilon");
        for (std::size_t n = 0; n <= 80; ++n) require(!length.accepts(n), "unproductive cycle rejects every length");
    }
}

crank::Ast repeat_ast(std::size_t lower, std::optional<std::size_t> upper,
                      bool nullable, std::size_t unit = 1) {
    auto ast = crank::parse(nullable ? "(?:a{" + std::to_string(unit) + "}|)" :
                                      "a{" + std::to_string(unit) + "}");
    crank::Node repeat;
    repeat.kind = crank::Kind::Repeat;
    repeat.children = {ast.root};
    repeat.lower = lower;
    repeat.upper = upper;
    ast.root = ast.nodes.size();
    ast.nodes.push_back(std::move(repeat));
    return ast;
}

void huge_repetitions() {
    constexpr auto maximum = std::numeric_limits<std::size_t>::max();
    // Every n <= 256 is realizable by choosing n consuming iterations and
    // empty iterations elsewhere. This also exercises an inclusive range
    // containing SIZE_MAX + 1 possible counts, without representing that sum.
    for (const auto lower : {std::size_t{0}, maximum - 1, maximum}) {
        const auto ast = repeat_ast(lower, maximum, true);
        const crank::OccurrenceAnalysis analysis(ast);
        ++analyses;
        for (std::size_t n = 0; n <= 256; ++n)
            require(analysis.length(ast.root).accepts(n), "huge nullable repetition retains every feasible small length");
    }
    {
        const auto ast = repeat_ast(maximum, maximum, true, 64);
        const crank::OccurrenceAnalysis analysis(ast);
        ++analyses;
        for (std::size_t n = 0; n <= 256; ++n)
            require(analysis.length(ast.root).accepts(n) == (n % 64 == 0),
                    "huge fixed power of an epsilon-or-64-byte body");
    }
    {
        const auto ast = repeat_ast(maximum, maximum, false, 2);
        const crank::OccurrenceAnalysis analysis(ast);
        ++analyses;
        require(!analysis.length(ast.root).accepts(0) && !analysis.length(ast.root).accepts(maximum - 1),
                "overflowed actual length retains a saturated lower bound");
        require(!analysis.length(ast.root).accepts(maximum),
                "residue arithmetic rejects the saturation sentinel for exact SIZE_MAX times two");
    }
    {
        const auto ast = repeat_ast(2, 1, true);
        const crank::OccurrenceAnalysis analysis(ast);
        ++analyses;
        require(!analysis.length(ast.root).productive, "invalid repetition interval is empty");
    }
}

void environment_conservatism() {
    for (const bool forward : {false, true}) {
        auto ast = crank::parse("aa");
        crank::Node reference;
        reference.kind = crank::Kind::Reference;
        reference.capture_id = 0;
        reference.forward = forward;
        const auto reference_id = ast.nodes.size();
        ast.nodes.push_back(reference);
        crank::Node concat;
        concat.kind = crank::Kind::Concat;
        concat.children = {ast.root, reference_id};
        ast.root = ast.nodes.size();
        ast.nodes.push_back(concat);
        const crank::OccurrenceAnalysis analysis(ast);
        ++analyses;
        for (std::size_t n = 0; n <= 256; ++n) {
            require(analysis.length(reference_id).accepts(n), "unresolved reference has unknown length");
            require(analysis.length(ast.root).accepts(n) == (n >= 2), "reference uncertainty survives concatenation");
        }
        sound_lengths(ast, 80, "reference overapproximation");
    }
    for (const bool path : {false, true}) {
        auto ast = repeat_ast(7, 7, false, 4);
        auto& repeat = ast.nodes[ast.root];
        if (path) repeat.count_path = 0;
        else repeat.use_count = 0;
        const crank::OccurrenceAnalysis analysis(ast);
        ++analyses;
        for (std::size_t n = 0; n <= 256; ++n)
            require(analysis.length(ast.root).accepts(n) == (n % 4 == 0),
                    "dynamic count ignores placeholder fixed bounds but retains body lengths");
        sound_lengths(ast, 80, "dynamic count overapproximation");
    }
    {
        auto ast = repeat_ast(2, 3, false, 4);
        ast.nodes[ast.root].bind_count = 0;
        const crank::OccurrenceAnalysis analysis(ast);
        ++analyses;
        require(analysis.length(ast.root).accepts(8) && analysis.length(ast.root).accepts(12),
                "count binder retains both declared choices");
        require(!analysis.length(ast.root).accepts(4) && !analysis.length(ast.root).accepts(16),
                "count binder retains its finite interval");
        sound_lengths(ast, 80, "count binder range");
    }
    {
        auto ast = repeat_ast(0, {}, false);
        for (auto& node : ast.nodes)
            if (node.kind == crank::Kind::CharSet) node.characters.reset();
        const crank::OccurrenceAnalysis analysis(ast);
        ++analyses;
        require(analysis.length(ast.root).accepts(0) && !analysis.length(ast.root).accepts(1),
                "zero iterations of an unproductive body still give epsilon");
        sound_lengths(ast, 40, "empty character set star");
    }
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

bool same_trace(const std::optional<crank::TracedOutcome>& a,
                const std::optional<crank::TracedOutcome>& b) {
    if (a.has_value() != b.has_value()) return false;
    if (!a) return true;
    if (a->outcome != b->outcome || a->occurrences.size() != b->occurrences.size()) return false;
    for (std::size_t i = 0; i < a->occurrences.size(); ++i) {
        const auto& x = a->occurrences[i];
        const auto& y = b->occurrences[i];
        if (x.id != y.id || x.parent != y.parent || x.kind != y.kind ||
            x.source_id != y.source_id || x.source_node != y.source_node || x.name != y.name ||
            x.span != y.span || x.count != y.count || x.iteration != y.iteration ||
            x.depth != y.depth || x.height != y.height || x.source_rule != y.source_rule ||
            x.closed != y.closed || x.children != y.children) return false;
    }
    return true;
}

void matcher_equivalence() {
    const crank::OccurrenceProgram recursive(crank::parse("(?<S>aa(?&S)bb||a)"));
    std::set<std::string> language;
    for (std::size_t depth = 0; depth <= 4; ++depth) {
        language.insert(std::string(2 * depth, 'a') + std::string(2 * depth, 'b'));
        language.insert(std::string(2 * depth + 1, 'a') + std::string(2 * depth, 'b'));
    }
    auto subjects = words("ab", 6);
    subjects.insert(subjects.end(), language.begin(), language.end());
    for (const auto& subject : subjects) {
        const bool expected = language.contains(subject);
        ++membership_cases;
        for (const bool shared : {false, true}) {
            crank::OccurrenceMatchOptions pruned, unpruned;
            pruned.share_calls = unpruned.share_calls = shared;
            unpruned.prune = false;
            const auto label = "recursive membership '" + subject + "' shared=" + std::to_string(shared);
            require(recursive.full_match(subject, pruned) == expected, label + " pruned independent word oracle");
            require(recursive.full_match(subject, unpruned) == expected, label + " unpruned independent word oracle");
            const auto witness = recursive.first_match(subject, pruned);
            require(witness.has_value() == expected && witness == recursive.first_match(subject, unpruned),
                    label + " scalar witness unchanged");
            if (subject.size() <= 6) {
                for (std::size_t start = 0; start <= subject.size(); ++start) {
                    std::vector<std::size_t> endpoints;
                    for (auto end = start; end <= subject.size(); ++end)
                        if (language.contains(subject.substr(start, end - start))) endpoints.push_back(end);
                    require(recursive.endpoints(subject, start, pruned) == endpoints &&
                            recursive.endpoints(subject, start, unpruned) == endpoints,
                            label + " prefix endpoints at " + std::to_string(start));
                }
            }
        }
    }

    struct Fixture { std::string pattern; std::vector<std::string> subjects; };
    const std::vector<Fixture> histories{
        {"(?:aaaa){#n:0,2}b{=n}", {"", "aaaab", "aaaaaaaabb", "aaaa", "aaaaaaaab"}},
        {"(?:aaaa){#n:0,2}b{=#0.@count}", {"", "aaaab", "aaaaaaaabb", "aaaabb", "aaaaaaaa"}},
        {"(?<x>aaaa|a)\\k<x>", {"aa", "aaaaaaaa", "aaaaa", "", "aaaaaa"}},
        {"(?(DEFINE)(?<S>aa(?&S)bb||a))(?&S)c{=S[0].@height}",
            {"", "a", "aabbc", "aaabbc", "aaaabbbbcc", "aabb", "aabbcc"}},
        {"(?:a{#cols:0,2};){#rows:2,2}b{=rows[1].cols}",
            {";;", "a;aa;bb", "aa;a;b", "a;aa;b", "a;aa;bbb"}}
    };
    for (const auto& fixture : histories) {
        const auto ast = crank::parse(fixture.pattern);
        sound_lengths(ast, 48, "stateful " + fixture.pattern);
        const crank::OccurrenceProgram program(ast);
        require(!program.supports_shared_calls(), "stateful fixture uses the occurrence-history VM");
        for (const auto& subject : fixture.subjects) {
            crank::OccurrenceMatchOptions off;
            off.prune = false;
            const auto label = fixture.pattern + " subject '" + subject + "'";
            require(program.full_match(subject) == program.full_match(subject, off), label + " Boolean pruning equivalence");
            require(program.first_match(subject) == program.first_match(subject, off), label + " scalar pruning equivalence");
            require(program.match_outcomes(subject) == program.match_outcomes(subject, 0, true, off),
                    label + " all observable outcomes unchanged");
            require(same_trace(program.first_traced_match(subject), program.first_traced_match(subject, off)),
                    label + " occurrence trace, repetition count, depth and height unchanged");
            ++history_cases;
        }
    }
}
} // namespace

int main() {
    try {
        distinguishing_residues();
        generated_soundness();
        huge_repetitions();
        environment_conservatism();
        matcher_equivalence();
        std::cout << "residue checks=" << checks << " analyses=" << analyses
                  << " generated=" << generated_cases << " oracle_lengths=" << oracle_lengths
                  << " membership=" << membership_cases << " histories=" << history_cases << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "residue test failure: " << error.what() << '\n';
        return 1;
    }
}
