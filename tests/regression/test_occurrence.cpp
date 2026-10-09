#include "crank.hpp"
#include "equality_matcher.hpp"
#include "fragment_matcher.hpp"
#include "occurrence_analysis.hpp"
#include "occurrence_matcher.hpp"
#include "optimizations.hpp"

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
std::size_t checks = 0, oracle_cases = 0, endpoint_cases = 0;

void require(bool condition, const std::string& context) {
    ++checks;
    if (!condition) throw std::runtime_error(context);
}

template<class Exception, class Function>
void expect_exception(Function&& function, const std::string& context) {
    bool caught = false;
    try { function(); } catch (const Exception&) { caught = true; }
    require(caught, context);
}

template<class Function>
decltype(auto) contextual(const std::string& context, Function&& function) {
    try { return function(); }
    catch (const std::exception& error) { throw std::runtime_error(context + ": " + error.what()); }
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

using Language = std::set<std::string>;

void compare_language(const std::string& label, const std::string& pattern,
                      const Language& language, const std::vector<std::string>& subjects,
                      bool check_endpoints = false) {
    const crank::Program integrated(pattern);
    const crank::OccurrenceProgram direct(integrated.ast());
    crank::OccurrenceMatchOptions unpruned;
    unpruned.prune = false;
    for (const auto& subject : subjects) {
        const bool expected = language.contains(subject);
        require(contextual(label + ": direct pattern '" + pattern + "', subject '" + subject + "'",
                           [&] { return direct.full_match(subject); }) == expected,
                label + ": direct membership of '" + subject + "'");
        require(contextual(label + ": integrated subject '" + subject + "'",
                           [&] { return integrated.full_match(subject); }) == expected,
                label + ": integrated membership of '" + subject + "'");
        require(contextual(label + ": unpruned subject '" + subject + "'",
                           [&] { return direct.full_match(subject, unpruned); }) == expected,
                label + ": unpruned independent membership of '" + subject + "'");
        ++oracle_cases;
        if (check_endpoints) {
            for (std::size_t start = 0; start <= subject.size(); ++start) {
                std::vector<std::size_t> ends;
                for (auto end = start; end <= subject.size(); ++end)
                    if (language.contains(subject.substr(start, end - start))) ends.push_back(end);
                require(contextual(label + ": direct endpoints subject '" + subject + "', start " + std::to_string(start),
                                   [&] { return direct.endpoints(subject, start); }) == ends,
                        label + ": direct endpoints at " + std::to_string(start));
                require(contextual(label + ": integrated endpoints subject '" + subject + "', start " + std::to_string(start),
                                   [&] { return integrated.endpoints(subject, start); }) == ends,
                        label + ": integrated endpoints at " + std::to_string(start));
                ++endpoint_cases;
            }
        }
    }
}

void bounded_count_oracles() {
    Language equal;
    for (std::size_t n = 2; n <= 5; ++n) equal.insert(std::string(n, 'a') + std::string(n, 'b'));
    compare_language("bounded binder", "a{#n:2,5}b{=n}", equal, words("ab", 7), true);
    std::vector<std::string> boundary;
    for (std::size_t a = 0; a <= 7; ++a)
        for (std::size_t b = 0; b <= 7; ++b) boundary.push_back(std::string(a, 'a') + std::string(b, 'b'));
    compare_language("bounded binder long boundary", "a{#n:2,5}b{=#0.@count}", equal, boundary);

    Language rows;
    // The oracle constructs rows as integer vectors; it does not parse a
    // pattern or reuse any matcher state, path resolver, or matching engine.
    for (std::size_t count = 2; count <= 3; ++count) {
        std::size_t combinations = 1;
        for (std::size_t i = 0; i < count; ++i) combinations *= 4;
        for (std::size_t encoded = 0; encoded < combinations; ++encoded) {
            auto value = encoded;
            std::vector<std::size_t> columns;
            std::string prefix;
            for (std::size_t i = 0; i < count; ++i) {
                const auto cols = value % 4;
                value /= 4;
                columns.push_back(cols);
                prefix += std::string(cols, 'a') + ';';
            }
            rows.insert(prefix + std::string(columns.at(1), 'b'));
        }
    }
    std::vector<std::string> subjects = words("a;b", 5);
    for (const auto& valid : rows) {
        subjects.push_back(valid);
        subjects.push_back(valid + 'b');
        subjects.push_back('x' + valid);
        if (!valid.empty()) subjects.push_back(valid.substr(0, valid.size() - 1));
    }
    compare_language("row occurrence count", "(?:a{#cols:0,3};){#rows:2,3}b{=rows[1].cols}",
                     rows, subjects);
    compare_language("row occurrence endpoints", "(?:a{#cols:0,3};){#rows:2,3}b{=rows[1].cols}",
                     rows, {";;x", "a;aaa;aa;bbbx", "x;a;bb", "aa;a;bb"}, true);

    const crank::Program given("(?:a{#cols};){#rows}b{=rows[1].cols}");
    require(given.full_match("a;aaa;aa;bbb"), "requested uneven-row example");
    require(!given.full_match("a;aaa;aa;bb"), "path does not use latest global cols");
    const crank::Program legacy("(?:a{#cols};){#rows}b{=cols}");
    require(legacy.full_match("a;aaa;aa;bb"), "bare name retains latest-global semantics");
    require(!legacy.full_match("a;aaa;aa;bbb"), "bare name is distinct from selected row");

    const crank::Program nested(
        "(?:(?:a{#cols:0,3};){#rows:2,2}!){#blocks:2,2}b{=blocks[0].rows[1].cols}");
    for (std::size_t a = 0; a <= 3; ++a)
        for (std::size_t b = 0; b <= 3; ++b)
            for (std::size_t c = 0; c <= 3; ++c)
                for (std::size_t suffix = 0; suffix <= 3; ++suffix) {
                    const auto subject = std::string(a, 'a') + ';' + std::string(b, 'a') + ";!" +
                        std::string(c, 'a') + ";a;!" + std::string(suffix, 'b');
                    require(nested.full_match(subject) == (suffix == b),
                            "nested block/row path follows both iteration owners");
                    ++oracle_cases;
                }
}

// A finite set of independently constructed derivations, with source-rule
// height attached to each word. A word can appear with several heights. The
// grammar has only length-increasing recursive productions, so truncating at
// maximum bytes gives a finite fixed point without a derivation-depth cutoff.
using Annotated = std::pair<std::string, std::size_t>;
std::set<Annotated> balanced_height_derivations(std::size_t maximum) {
    std::set<Annotated> derivations{{"", 0}, {"ab", 0}};
    bool changed;
    do {
        changed = false;
        const auto previous = derivations;
        for (const auto& [inside, inside_height] : previous)
            for (const auto& [after, after_height] : previous) {
                if (inside.size() + after.size() + 2 > maximum) continue;
                changed |= derivations.emplace("a" + inside + "b" + after,
                    1 + std::max(inside_height, after_height)).second;
            }
    } while (changed);
    return derivations;
}

void recursive_height_oracle() {
    const auto annotated = balanced_height_derivations(6);
    require(annotated.contains({"ab", 0}) && annotated.contains({"ab", 1}),
            "independent oracle includes equal-span unequal-height derivations");
    require(annotated.contains({"aabb", 1}) && annotated.contains({"aabb", 2}),
            "independent oracle includes nested ambiguous heights");
    Language language;
    for (const auto& [word, height] : annotated) language.insert(word + std::string(height, 'c'));
    const std::string pattern = "(?(DEFINE)(?<S>a(?&S)b(?&S)|ab|))(?&S)c{=S[0].@height}";
    std::vector<std::string> subjects = words("abc", 5);
    for (const auto& [word, ignored_height] : annotated) {
        (void)ignored_height;
        for (std::size_t height = 0; height <= 4; ++height)
            subjects.push_back(word + std::string(height, 'c'));
    }
    std::sort(subjects.begin(), subjects.end());
    subjects.erase(std::unique(subjects.begin(), subjects.end()), subjects.end());
    compare_language("annotated recursive derivations", pattern, language, subjects);
    compare_language("annotated recursive endpoints", pattern, language,
                     {"abccx", "aabbccx", "ababccx", "xaabbcc", ""}, true);

    const crank::Program depth("(?(DEFINE)(?<S>a(?&S)b|))(?&S)c{=S[0].@height}");
    require(depth.full_match("aaabbbccc"), "requested recursive-height example");
    for (std::size_t n = 0; n <= 8; ++n)
        for (std::size_t suffix = 0; suffix <= 9; ++suffix) {
            const auto subject = std::string(n, 'a') + std::string(n, 'b') + std::string(suffix, 'c');
            require(depth.full_match(subject) == (n == suffix), "leaf height zero, recursive edge height one");
            ++oracle_cases;
        }

    const crank::Program transparent("(?(DEFINE)(?<S>(?<wrapper>a(?&S)b)|))(?&S)c{=S[0].@height}");
    const crank::Program mutual("(?(DEFINE)(?<S>a(?&T)|)(?<T>b(?&S)))(?&S)c{=S[0].@height}");
    for (std::size_t n = 0; n <= 5; ++n) {
        require(transparent.full_match(std::string(n, 'a') + std::string(n, 'b') + std::string(n, 'c')),
                "ordinary capture scopes are transparent to source-rule height");
        std::string alternating;
        for (std::size_t i = 0; i < n; ++i) alternating += "ab";
        require(mutual.full_match(alternating + std::string(2 * n, 'c')),
                "mutually recursive source rules each contribute a height edge");
        require(!mutual.full_match(alternating + std::string(2 * n + 1, 'c')),
                "mutual source height excludes the synthetic root");
    }
    const crank::Program whole("a(?R)b|c{=./@depth}");
    for (std::size_t n = 0; n <= 5; ++n)
        require(whole.full_match(std::string(n, 'a') + std::string(n, 'c') + std::string(n, 'b')),
                "whole-pattern recursion treats initial root as depth-zero source invocation");
}

void paths_scopes_and_missing_values() {
    const crank::Program relative("(?<Outer>a{#n:1,3}(?<Inner>b{=../n}))");
    const crank::Program rooted("(?<Outer>a{#n:1,3}(?<Inner>b{=/$Outer[0].#n}))");
    for (std::size_t a = 0; a <= 4; ++a)
        for (std::size_t b = 0; b <= 4; ++b) {
            const auto word = std::string(a, 'a') + std::string(b, 'b');
            const bool expected = a >= 1 && a <= 3 && a == b;
            require(relative.full_match(word) == expected, "ancestor-relative count path");
            require(rooted.full_match(word) == expected, "explicit root count path");
        }

    const crank::Program active("(?(DEFINE)(?<S>a{=./@depth}b(?&S)?))(?&S)c{=S[0].@height}");
    std::string prefix;
    for (std::size_t level = 0; level <= 5; ++level) {
        prefix += std::string(level, 'a') + 'b';
        require(active.full_match(prefix + std::string(level, 'c')),
                "active rule depth is readable before scope completion");
        require(!active.full_match(prefix + std::string(level + 1, 'c')),
                "active rule depth and final height agree");
    }

    const crank::Program collision("(?<same>a)b{#same:1,3}c{=#same}d{=$same[0].@bytes}");
    const crank::Program numeric("(?<same>a)b{#same:1,3}c{=#0}d{=$1[0].@bytes}");
    for (std::size_t n = 1; n <= 3; ++n) {
        const auto good = "a" + std::string(n, 'b') + std::string(n, 'c') + 'd';
        require(collision.full_match(good), "type prefix disambiguates same-spelled sources");
        require(numeric.full_match(good), "numeric selectors use stable source IDs");
        require(!numeric.full_match(good + 'd'), "group byte metric uses selected occurrence span");
    }

    const crank::Program missing("(?:a{#n:1,1}|c)b{=#n}");
    require(missing.full_match("ab"), "executed binder has a value");
    require(!missing.full_match("c"), "absent binder is not zero");
    const crank::Program zero("a{#n:0,0}b{=#n}");
    require(zero.full_match(""), "executed zero binder is present");
    const crank::Program absent_iteration("(?:a{#cols:0,2};){#rows:0,2}b{=rows[2].cols}");
    for (const auto& subject : {"", ";", ";;", "a;a;", "a;a;b"})
        require(!absent_iteration.full_match(subject), "out-of-range dynamic iteration is missing");
    const crank::Program absent_group("(?<G>a){0,1}b{=G[1].@bytes}");
    require(!absent_group.full_match("a"), "out-of-range dynamic group is missing");

    const crank::Program repeated("(?<G>ab){2}c{=G[1].@bytes}");
    require(repeated.full_match("ababcc"), "group index selects second dynamic group occurrence");
    const crank::Program iteration_bytes("(?:ab){#n:2,2}c{=n[1].@bytes}");
    require(iteration_bytes.full_match("ababcc"), "indexed count iteration exposes byte span");
    require(!iteration_bytes.full_match("ababcccc"), "iteration byte span differs from execution span");
    const crank::Program execution_bytes("(?:ab){#n:2,2}c{=n.@bytes}");
    require(execution_bytes.full_match("ababcccc"), "unindexed count execution exposes total byte span");
    const crank::Program stutter("(?:(?<G>a?)|\\k<G>){3}b{=G[1].@bytes}\\k<G>");
    require(stutter.full_match("aba"),
            "an empty backreference iteration preserves group ordinal before a later capturing iteration");
}

void mixed_environments_and_capture_order() {
    const crank::Program mixed(
        "(?(DEFINE)(?<S>a(?&S)b|))(?<W>[xy]{#n:1,3})(?&S)\\k<W>d{=n}c{=S[0].@height}");
    for (std::size_t width = 1; width <= 3; ++width)
        for (const auto& prefix : words("xy", static_cast<unsigned>(width))) {
            if (prefix.size() != width) continue;
            for (std::size_t depth = 0; depth <= 4; ++depth) {
                const auto good = prefix + std::string(depth, 'a') + std::string(depth, 'b') + prefix +
                    std::string(width, 'd') + std::string(depth, 'c');
                require(mixed.full_match(good), "recursion, byte equality, legacy count, and height compose");
                auto bad = good;
                bad[width + depth * 2] = bad[width + depth * 2] == 'x' ? 'y' : 'x';
                require(!mixed.full_match(bad), "recursive environment preserves a prior byte backreference");
                ++oracle_cases;
            }
        }

    const crank::Program nested("(?(DEFINE)(?<S>a(?&S)b|))(?&S)c{=S[0].@height}");
    const auto nested_outcome = nested.first_match("aaabbbccc");
    require(nested_outcome && nested_outcome->captures.at(1) == crank::Span{0, 6},
            "outer recursive completion supplies the legacy capture value");
    require(nested_outcome->captures.at(0) == crank::Span{0, 9}, "capture zero includes metric suffix");

    const crank::Program siblings("(?(DEFINE)(?<G>a))(?&G)(?&G)b{=G[0].@bytes}");
    const auto sibling_outcome = siblings.first_match("aab");
    require(sibling_outcome && sibling_outcome->captures.at(1) == crank::Span{1, 2},
            "last sibling completion wins the ordinary capture slot");

    const crank::Program recursive_backref("(?(DEFINE)(?<S>a(?&S)b|))(?&S)\\k<S>c{=S[0].@height}");
    require(recursive_backref.full_match("aabbaabbcc"), "backreference sees completed outer recursive span");
    require(!recursive_backref.full_match("aabbabcc"), "backreference does not see the inner recursive span");
}

void syntax_boundaries_and_analysis() {
    for (const std::string pattern : {
            "a{#n:2,1}", "a{#n:,2}", "a{#n:}", "a{#n:2,3,4}",
            "a{#n:184467440737095516160}", "a{#n:1,2}b{=n[-1].@bytes}",
            "a{#n:1,2}b{=n[].@bytes}", "a{#n:1,2}b{=n[1]}",
            "a{#n:1,2}b{=n.@height}", "a{#n:1,2}b{=n.@unknown}",
            "a{#n:1,2}b{=n.@bytes.extra}", "(?<G>a)b{=G[0]}",
            "(?<G>a)b{=G[0].@count}", "(?<G>a)b{=$0[0].@bytes}",
            "a{#n:1,2}b{=#99}", "(?<G>a)b{=$99[0].@bytes}",
            "a{#n:1,2}b{=n[184467440737095516160].@bytes}",
            "(?<same>a)b{#same:1,2}c{=same.@bytes}",
            "(?:){#n}", "(?:a?){#n:1,}",
            "(?(DEFINE)(?<A>))(?&A){#n}",
            "(?(DEFINE)(?<A>(?&A)))(?:a(?&A)){#n}"}) {
        expect_exception<std::invalid_argument>([&] { (void)crank::parse(pattern); },
                                            "malformed or statically invalid occurrence syntax: " + pattern);
    }
    const crank::Program finite_nullable("(?:){#n:2,3}a{=#n}");
    require(finite_nullable.full_match("aa") && finite_nullable.full_match("aaa"),
            "finite nullable binders preserve iteration counts");
    require(!finite_nullable.full_match("") && !finite_nullable.full_match("aaaa"),
            "finite nullable binder respects both bounds");
    const crank::Program exact("a{#n:2}b{=#n}");
    require(exact.full_match("aabb") && !exact.full_match("aaabbb"), "exact bounded binder spelling");
    const crank::Program open("a{#n:2,}b{=#n}");
    require(open.full_match("aaaabbbb") && !open.full_match("ab"), "open upper-bound spelling");
    const crank::Program called_positive("(?(DEFINE)(?<A>a))(?&A){#n}b{=n}");
    require(called_positive.full_match("aaabbb"), "resolved productive call supports an unbounded binder");

    struct AnalysisCase { std::string pattern; bool productive; std::size_t minimum, modulus; };
    for (const auto& item : std::vector<AnalysisCase>{
            {"(?<S>a(?&S)b|)", true, 0, 2},
            {"(?<S>a(?&S)|a)", true, 1, 1},
            {"(?<S>(?&S))", false, 0, 0},
            {"(?:aa|aaaa){2}", true, 4, 2},
            {"(?:ab){#n:2,5}c{=#n}", true, 4, 1}}) {
        const auto ast = crank::parse(item.pattern);
        const crank::OccurrenceAnalysis analysis(ast);
        const auto& result = analysis.length(ast.root);
        require(result.productive == item.productive, "recursive length analysis productivity");
        if (item.productive) {
            require(result.minimum == item.minimum, "recursive length analysis necessary lower bound");
            require(result.modulus == item.modulus, "recursive length analysis sound congruence");
        }
    }

    for (const auto& [base, offset] : std::vector<std::pair<std::string, std::size_t>>{{"", 0}, {"x", 1}}) {
        const auto ast = crank::parse("(?(DEFINE)(?<S>a(?&S)b|" + base + "))(?&S)c{=S[0].@height}");
        const crank::OccurrenceAnalysis analysis(ast);
        const auto& equation = analysis.height_equation();
        require(equation && equation->offset == offset && equation->coefficient == 3,
                "certified linear recursive height equation includes shell and suffix widths");
        for (std::size_t height = 0; height <= 8; ++height) {
            require(equation->height_for_length(offset + 3 * height) == height,
                    "height equation inverts every constructed valid length");
            require(!equation->accepts(offset + 3 * height + 1), "height equation rejects wrong residue");
        }
    }
    for (const std::string pattern : {
            "(?(DEFINE)(?<S>a(?&S)b(?&S)|))(?&S)c{=S[0].@height}",
            "(?(DEFINE)(?<S>a(?&S)b|x|yy))(?&S)c{=S[0].@height}",
            "(?(DEFINE)(?<S>a(?&S)b|))(?&S)c{=S[0].@bytes}"}) {
        const crank::OccurrenceAnalysis analysis(crank::parse(pattern));
        require(!analysis.height_equation(), "height certificate refuses an ineligible recursive equation");
    }
}

void trace_invariants(const crank::TracedOutcome& trace, const std::string& label) {
    require(!trace.occurrences.empty(), label + ": synthetic root is present");
    const auto& root = trace.occurrences.front();
    require(root.kind == crank::OccurrenceKind::Root && root.id == 0 && root.parent == crank::no_id,
            label + ": root identity");
    require(trace.outcome.captures.at(0) == root.span, label + ": root span equals complete capture");
    std::vector<std::size_t> references(trace.occurrences.size());
    for (std::size_t index = 0; index < trace.occurrences.size(); ++index) {
        const auto& record = trace.occurrences[index];
        require(record.id == index && record.closed, label + ": dense, completed occurrence IDs");
        require(record.span.begin <= record.span.end && record.span.end <= trace.outcome.end,
                label + ": valid absolute byte span");
        for (const auto child : record.children) {
            require(child > index && child < trace.occurrences.size(), label + ": preorder child identity");
            ++references.at(child);
            const auto& descendant = trace.occurrences.at(child);
            require(descendant.parent == index, label + ": bidirectional ownership");
            require(record.span.begin <= descendant.span.begin && descendant.span.end <= record.span.end,
                    label + ": child byte span belongs to owner");
        }
    }
    for (std::size_t index = 1; index < references.size(); ++index)
        require(references[index] == 1, label + ": successful derivation is a tree");
}

void traces_and_frozen_obligations() {
    const auto rows_ast = crank::parse("(?:a{#cols};){#rows}b{=rows[1].cols}");
    const crank::OccurrenceProgram rows(rows_ast);
    const auto row_trace = rows.first_traced_match("a;aaa;aa;bbb");
    require(row_trace.has_value(), "uneven-row trace exists");
    trace_invariants(*row_trace, "uneven-row trace");
    require(crank::resolve_occurrence_value(*row_trace, rows_ast.count_paths.at(0)) == 3,
            "completed trace resolver selects second row's column count");
    std::size_t row_executions = 0, col_executions = 0;
    std::vector<std::size_t> column_values;
    for (const auto& record : row_trace->occurrences) {
        if (record.kind != crank::OccurrenceKind::Count) continue;
        if (record.name == "rows") {
            ++row_executions;
            require(record.count == 3 && record.span == crank::Span{0, 9}, "outer count execution records full run");
            require(record.children.size() == 3, "outer count owns three iterations");
        } else if (record.name == "cols") {
            ++col_executions;
            require(record.count.has_value(), "column count is present in trace");
            column_values.push_back(*record.count);
        }
    }
    require(row_executions == 1 && col_executions == 3 && column_values == std::vector<std::size_t>{1, 3, 2},
            "every repeated execution retains its own count value");

    const auto recursive_ast = crank::parse("(?(DEFINE)(?<S>a(?&S)b|))(?&S)c{=S[0].@height}");
    const crank::OccurrenceProgram recursive(recursive_ast);
    const auto trace = recursive.first_traced_match("aaabbbccc");
    require(trace.has_value(), "recursive trace exists");
    trace_invariants(*trace, "recursive trace");
    std::size_t invocations = 0;
    for (const auto& record : trace->occurrences) {
        if (record.kind != crank::OccurrenceKind::Group || record.name != "S") continue;
        require(record.source_rule && record.depth == invocations && record.height == 3 - invocations,
                "rule depths and synthesized heights retain all recursive invocations");
        require(record.span == crank::Span{invocations, 6 - invocations}, "recursive invocation span oracle");
        ++invocations;
    }
    require(invocations == 4, "nullable leaf invocation is retained with height zero");
    require(crank::resolve_occurrence_value(*trace, recursive_ast.count_paths.at(0)) == 3,
            "height path resolver reads the selected root invocation");

    const crank::OccurrenceProgram ambiguous(crank::parse(
        "(?(DEFINE)(?<S>a(?&S)b|ab|))(?&S)c{=S[0].@height}"));
    for (const std::size_t height : {0U, 1U}) {
        const auto proof = ambiguous.first_traced_match("ab" + std::string(height, 'c'));
        require(proof.has_value(), "equal-span alternative history remains selectable by height");
        trace_invariants(*proof, "equal-span alternative trace");
        const auto selected = std::find_if(proof->occurrences.begin(), proof->occurrences.end(), [](const auto& rec) {
            return rec.kind == crank::OccurrenceKind::Group && rec.name == "S" && rec.depth == 0;
        });
        require(selected != proof->occurrences.end() && selected->span == crank::Span{0, 2} && selected->height == height,
                "trace retains the successful derivation instead of merging equal spans");
    }

    const crank::Program future("b{=/$G.@bytes}(?<G>a|cc){2}");
    require(future.full_match("bacc"), "future unindexed selector binds the next applicable occurrence");
    require(!future.full_match("bbacc"), "future selector does not drift to a later group occurrence");
    const crank::Program skipped("b{=/$g.@bytes}(?<g>a)?");
    require(!skipped.full_match(""), "unfulfilled forward occurrence is missing rather than zero");
    require(skipped.full_match("ba"), "forward byte obligation resolves when its scope closes");
    const crank::Program self_bytes("(?<g>b{=./@bytes})");
    const crank::Program root_bytes("b{=/@bytes}");
    for (std::size_t n = 0; n <= 5; ++n) {
        require(self_bytes.full_match(std::string(n, 'b')), "active group byte obligation freezes current group");
        require(root_bytes.full_match(std::string(n, 'b')), "root byte obligation validates at whole-match closure");
    }
    const crank::Program self_height("(?<G>b{=./@height}(?&G)?a)c{=G[0].@height}");
    for (std::size_t height = 0; height <= 4; ++height) {
        const auto good = std::string(height * (height + 1) / 2, 'b') +
            std::string(height + 1, 'a') + std::string(height, 'c');
        require(contextual("pending self height valid " + std::to_string(height), [&] { return self_height.full_match(good); }),
                "pending current height remains frozen to its original invocation");
        require(!contextual("pending self height invalid " + std::to_string(height), [&] { return self_height.full_match('b' + good); }),
                "pending current height cannot be satisfied by a descendant");
    }
}

void left_recursion_and_resource_limits() {
    const crank::OccurrenceProgram left(crank::parse("(?<S>(?&S)a|a)b{=S[0].@height}"));
    for (const bool prune : {false, true}) {
        crank::OccurrenceMatchOptions options;
        options.prune = prune;
        options.max_call_depth = 64;
        options.max_steps = 100000;
        for (std::size_t n = 1; n <= 8; ++n) {
            crank::OccurrenceStatistics stats;
            const auto word = std::string(n, 'a') + std::string(n - 1, 'b');
            const auto context = "productive left recursion n=" + std::to_string(n) + ", prune=" + std::to_string(prune);
            require(contextual(context + " valid", [&] { return left.full_match(word, options, &stats); }),
                    "productive left recursion derives its finite language");
            require(!stats.limit_reached, "productive left recursion completes within input-induced depth");
            require(!contextual(context + " invalid", [&] { return left.full_match(word + 'b', options); }),
                    "productive left recursion rejects excess metric suffix");
            std::vector<std::size_t> expected_ends{1};
            if (word.size() != 1) expected_ends.push_back(word.size());
            require(contextual(context + " endpoints", [&] { return left.endpoints(word + 'x', 0, options); }) == expected_ends,
                    "productive left recursion enumerates every endpoint and terminates");
        }
    }

    const crank::OccurrenceProgram program(crank::parse("a{#n:2,4}b{=#n}"));
    crank::OccurrenceStatistics reference;
    require(program.full_match("aaabbb", {}, &reference), "resource-budget fixture matches");
    require(reference.states > 0 && reference.work_steps > 0 && reference.occurrences > 0,
            "resource-budget fixture reports actual work");
    for (const int budget : {0, 1, 2, 3}) {
        crank::OccurrenceMatchOptions options;
        if (budget == 0) options.max_states = 0;
        if (budget == 1) options.max_steps = 0;
        if (budget == 2) options.max_occurrences = 0;
        if (budget == 3) options.max_repeat = 1;
        crank::OccurrenceStatistics stats;
        expect_exception<crank::ResourceLimit>([&] { (void)program.full_match("aaabbb", options, &stats); },
                                              "exhausted occurrence budget is not reported as no match");
        require(stats.limit_reached, "occurrence budget exposes incomplete-result diagnostic");
    }

    const crank::OccurrenceProgram nested(crank::parse("(?(DEFINE)(?<S>a(?&S)b|))(?&S)c{=S[0].@height}"));
    crank::OccurrenceMatchOptions exact_depth;
    exact_depth.max_call_depth = 4;
    require(nested.full_match("aaabbbccc", exact_depth), "call-depth budget includes root and nullable leaf");
    crank::OccurrenceStatistics depth_stats;
    expect_exception<crank::ResourceLimit>([&] { (void)nested.full_match("aaaabbbbcccc", exact_depth, &depth_stats); },
                                          "insufficient call-depth budget is not a false negative");
    require(depth_stats.limit_reached, "call-depth budget exposes incomplete-result diagnostic");

    const crank::OccurrenceProgram epsilon(crank::parse("(?<S>(?&S)|)c{=S[0].@bytes}"));
    crank::OccurrenceMatchOptions finite;
    finite.max_call_depth = 6;
    require(epsilon.full_match("", finite), "finite Boolean witness survives an unbounded nullable recursion");
    require(epsilon.first_traced_match("", finite).has_value(), "finite trace witness survives a nullable recursion");
    for (const bool endpoints : {false, true}) {
        crank::OccurrenceStatistics stats;
        expect_exception<crank::ResourceLimit>([&] {
            if (endpoints) (void)epsilon.endpoints("", 0, finite, &stats);
            else (void)epsilon.match_outcomes("", 0, true, finite, &stats);
        }, "exhaustive nullable recursion reports an explicit limit");
        require(stats.limit_reached, "nullable recursion limit diagnostic is set");
    }
    crank::OccurrenceStatistics stats;
    expect_exception<crank::ResourceLimit>([&] { (void)epsilon.full_match("a", finite, &stats); },
                                          "unexplored epsilon histories cannot yield a false negative");
    require(stats.limit_reached, "negative nullable recursion records its unresolved branch");
}

void wide_continuation_teardown() {
    constexpr std::size_t width = 20000;
    const crank::OccurrenceProgram program(crank::parse(std::string(width, 'a')));
    auto subject = std::string(width, 'a');
    subject.back() = 'b';
    // Exercise the history-frame teardown contract explicitly. Pure native
    // recognition can now reject the final byte before allocating any frames.
    crank::OccurrenceMatchOptions history;
    history.share_calls = false;
    require(!program.full_match(subject, history), "wide flat continuations reject safely without recursive destruction");
    auto limited = history;
    limited.max_states = 128;
    expect_exception<crank::ResourceLimit>([&] { (void)program.full_match(subject, limited); },
                                          "partially allocated wide continuation tears down safely at a budget limit");
}

void byte_and_endpoint_boundaries() {
    const crank::Program program("(?<G>[\\x00-\\xff]{#n:1,3})b{=G[0].@bytes}");
    for (unsigned byte = 0; byte < 256; ++byte) {
        std::string good(2, static_cast<char>(byte));
        good += "bb";
        require(program.full_match(good), "occurrence offsets support NUL and every high byte");
        if (byte != static_cast<unsigned>('b'))
            require(!program.full_match(good + 'b'), "byte metric excludes suffix bytes");
        ++oracle_cases;
    }
    const crank::OccurrenceProgram direct(crank::parse("a{#n:1,3}b{=#n}"));
    require(direct.endpoints("xaabbx", 1) == std::vector<std::size_t>{5},
            "nonzero endpoint start keeps absolute span coordinates");
    require(direct.endpoints("xaabbx", 6).empty(), "empty tail is a valid endpoint start");
    expect_exception<std::out_of_range>([&] { (void)direct.endpoints("ab", 3); },
                                        "endpoint start past subject is an error");
    for (const std::size_t width : {63U, 64U, 127U, 128U, 255U, 256U}) {
        const crank::Program wide("(?<G>a{" + std::to_string(width) + "})b{=G[0].@bytes}");
        const auto valid = std::string(width, 'a') + std::string(width, 'b');
        require(wide.full_match(valid), "byte metric crosses offset width boundary");
        require(!wide.full_match(valid + 'b'), "byte metric rejects boundary overrun");
    }
}

void legacy_backend_eligibility() {
    for (const std::string pattern : {"a{#n:1,2}b{=#n}", "(?<G>a)b{=G[0].@bytes}"}) {
        auto ast = crank::parse(pattern);
        for (const bool preserve_flags : {true, false}) {
            if (!preserve_flags) { ast.has_extended = false; ast.has_occurrence_paths = false; }
            require(!crank::make_greedy_plan(ast) && !crank::make_count_plan(ast) &&
                    !crank::make_equality_plan(ast) && !crank::make_affine_plan(ast),
                    "legacy plans inspect unsupported source nodes even if advisory flags are cleared");
            expect_exception<std::invalid_argument>([&] { const crank::RegularProgram old(ast); },
                "direct DFA cannot erase an occurrence constraint");
            const crank::OccurrenceProgram unified(ast);
            for (const std::string subject : {"", "ab", "aabb", "abb", "aab"}) {
                auto facade = crank::match_states(ast, subject);
                auto direct = unified.match_outcomes(subject);
                std::sort(facade.begin(), facade.end());
                std::sort(direct.begin(), direct.end());
                require(facade == direct,
                    "state facade preserves full occurrence outcomes even with stale advisory flags");
                require(!facade.empty() == (subject == "ab" ||
                    (pattern == "a{#n:1,2}b{=#n}" && subject == "aabb")),
                    "independent bounded count/path membership oracle");
            }
            const crank::ExternalRegularProgram external(ast, ast.root);
            require(!external.available(), "external regular serializer declines occurrence constraints");
        }
        const auto literal = std::find_if(ast.nodes.begin(), ast.nodes.end(), [](const auto& node) {
            return node.kind == crank::Kind::CharSet && node.characters.test(static_cast<unsigned>('a'));
        });
        require(literal != ast.nodes.end(), "regular fragment fixture contains literal a");
        const crank::RegularFragment fragment(ast, static_cast<crank::NodeId>(literal - ast.nodes.begin()));
        require(fragment.full_match("a") && !fragment.full_match("b"),
                "a pure regular subtree remains available inside an occurrence AST");
    }
    auto bounded = crank::parse("a{#n:1,2}b{=n}");
    bounded.has_extended = false;
    require(!crank::make_count_plan(bounded), "legacy count plan inspects binder bounds independently of flags");
}

} // namespace

int main() {
    std::string stage;
    try {
        stage = "bounded_count_oracles";
        bounded_count_oracles();
        stage = "recursive_height_oracle";
        recursive_height_oracle();
        stage = "paths_scopes_and_missing_values";
        paths_scopes_and_missing_values();
        stage = "mixed_environments_and_capture_order";
        mixed_environments_and_capture_order();
        stage = "syntax_boundaries_and_analysis";
        syntax_boundaries_and_analysis();
        stage = "byte_and_endpoint_boundaries";
        byte_and_endpoint_boundaries();
        stage = "legacy_backend_eligibility";
        legacy_backend_eligibility();
        stage = "traces_and_frozen_obligations";
        traces_and_frozen_obligations();
        stage = "left_recursion_and_resource_limits";
        left_recursion_and_resource_limits();
        stage = "wide_continuation_teardown";
        wide_continuation_teardown();
        std::cout << "{\"status\":\"PASS\",\"occurrence_checks\":" << checks
                  << ",\"independent_oracle_cases\":" << oracle_cases
                  << ",\"endpoint_cases\":" << endpoint_cases << "}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL in " << stage << " after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
