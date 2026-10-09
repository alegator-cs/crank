#include "affine_matcher.hpp"
#include "crank.hpp"
#include "fragment_matcher.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <future>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
std::size_t checks = 0, differential_cases = 0, oracle_cases = 0;

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

crank::MatchOptions unpruned() {
    crank::MatchOptions options;
    options.prune = false;
    return options;
}

std::vector<std::string> words(std::string_view alphabet, unsigned maximum) {
    std::vector<std::string> result{std::string{}};
    std::size_t begin = 0, end = 1;
    for (unsigned length = 0; length < maximum; ++length) {
        for (std::size_t i = begin; i < end; ++i)
            for (char byte : alphabet) result.push_back(result[i] + byte);
        begin = end;
        end = result.size();
    }
    return result;
}

std::vector<std::size_t> complete_endpoints(const crank::Ast& ast,
                                           std::string_view subject, std::size_t start) {
    std::vector<std::size_t> result;
    for (const auto& outcome : crank::match_states(ast, subject, start, false, unpruned()))
        result.push_back(outcome.end);
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

void regular_differential() {
    const std::vector<std::string> patterns{
        "", "a", "[ab]*", "(?:ab)*", "(?:ab|ba)*", "a*b*", "(?:a|ab)*b",
        "(?:a?|b?)*", "(?:ab|a)*", "(?:a|ba){0,3}", "(a*)(b*)",
        "(?:[ab]{2}){1,3}", "(?:a[ab]b){0,3}", "ab[\\x00-\\xff]*",
        "[\\x00-\\xff]*a", "[\\x00-\\xff]*", "[^\\x00-\\xff]",
        "(?:[^\\x00-\\xff])*", "(?:ab|ba|aa)*b?"
    };
    for (const auto& pattern : patterns) {
        const crank::Program program(pattern);
        const crank::RegularProgram dfa(program.ast());
        for (const auto& subject : words("ab", 5)) {
            const bool expected = !crank::match_states(program.ast(), subject, 0, true, unpruned()).empty();
            require(dfa.full_match(subject) == expected, "DFA membership: " + pattern);
            require(program.full_match(subject) == expected, "Auto membership: " + pattern);
            crank::RunOptions plain;
            plain.optimize = false;
            require(program.full_match(subject, plain) == expected, "unoptimized Auto: " + pattern);
            ++differential_cases;
            for (std::size_t start = 0; start <= subject.size(); ++start) {
                const auto ends = complete_endpoints(program.ast(), subject, start);
                require(dfa.endpoints(subject, start) == ends, "DFA endpoints: " + pattern);
                require(program.endpoints(subject, start) == ends, "Auto endpoints: " + pattern);
                ++differential_cases;
            }
        }
        expect_exception<std::out_of_range>([&] { (void)dfa.endpoints("ab", 3); },
                                            "DFA must reject an out-of-range start");
    }
}

std::string escape_byte(unsigned byte) {
    constexpr char hex[] = "0123456789abcdef";
    std::string result = "\\x00";
    result[2] = hex[byte >> 4]; result[3] = hex[byte & 15];
    return result;
}

void full_byte_alphabet() {
    // Every first byte has a different required continuation. This exercises
    // all 256 alphabet classes and more than 256 distinguishable DFA states.
    std::string pattern = "(?:";
    for (unsigned byte = 0; byte < 256; ++byte) {
        if (byte) pattern += '|';
        pattern += escape_byte(byte) + escape_byte((byte + 1) % 256);
    }
    pattern += ')';
    crank::RegularOptions options;
    // Determinization retains separate accepting subsets before minimization.
    options.max_dfa_states = 1024;
    const crank::RegularProgram program(crank::parse(pattern), options);
    crank::RegularStatistics stats;
    require(!program.full_match("", &stats), "cyclic-byte language rejects empty input");
    require(stats.dfa_states > 256, "byte continuation fixture must exceed 256 DFA states");
    for (unsigned first = 0; first < 256; ++first) {
        for (unsigned second = 0; second < 256; ++second) {
            const std::string subject{static_cast<char>(first), static_cast<char>(second)};
            require(program.full_match(subject) == (second == (first + 1) % 256),
                    "independent cyclic-byte adjacency oracle");
            ++oracle_cases;
        }
        const std::string valid{static_cast<char>(first), static_cast<char>((first + 1) % 256)};
        require(program.endpoints(valid, 0) == std::vector<std::size_t>{2}, "full-byte valid endpoint");
        require(program.endpoints(valid, 1).empty(), "full-byte incomplete suffix");
        require(program.endpoints(valid, 2).empty(), "full-byte empty suffix");
    }

    for (const std::string simple : {"[\\x00-\\x7f]*", "[\\x80-\\xff]+",
                                    "[\\x00\\x80\\xff]{0,3}", "[^a-c]*"}) {
        const crank::Program integrated(simple);
        const crank::RegularProgram dfa(integrated.ast());
        for (unsigned byte = 0; byte < 256; ++byte) {
            const std::string subject(3, static_cast<char>(byte));
            const bool expected = !crank::match_states(integrated.ast(), subject, 0, true, unpruned()).empty();
            require(dfa.full_match(subject) == expected, "signed-byte DFA differential");
            require(integrated.full_match(subject) == expected, "signed-byte Auto differential");
            for (std::size_t start = 0; start <= subject.size(); ++start)
                require(dfa.endpoints(subject, start) == complete_endpoints(integrated.ast(), subject, start),
                        "signed-byte endpoint differential");
            ++differential_cases;
        }
    }
}

void state_width_boundaries() {
    // Offset compression must preserve destinations near and beyond one-byte
    // limits. The independent language is simply one exact run of a's.
    for (const std::size_t length : {63U, 64U, 126U, 127U, 128U, 253U, 254U, 255U, 256U, 300U}) {
        const crank::RegularProgram dfa(crank::parse("a{" + std::to_string(length) + "}"));
        const std::string valid(length, 'a');
        require(dfa.full_match(valid), "exact long run accepts");
        require(!dfa.full_match(valid + 'a'), "exact long run rejects excess");
        require(!dfa.full_match(std::string_view(valid).substr(1)), "exact long run rejects shortage");
        require(dfa.endpoints(valid + 'a') == std::vector<std::size_t>{length}, "long run endpoint");
        for (const auto position : {std::size_t{0}, length / 2, length - 1}) {
            auto bad = valid; bad[position] = 'b';
            require(!dfa.full_match(bad), "long run rejects internal mutation");
        }
        ++oracle_cases;
    }
}

void accepting_sink_endpoints() {
    struct Case { std::string pattern, subject; };
    std::string every_byte;
    for (unsigned byte = 0; byte < 256; ++byte) every_byte += static_cast<char>(byte);
    const std::vector<Case> cases{
        {"(?:ab|ba)*", "abbaabbaabbaabbaabbaabbaabbaabba"},
        {"(?:ab|ba)*", "abbaabbaabbbaabbaabbaabbaabbaaba"},
        {"a*b*", std::string(23, 'a') + std::string(46, 'b')},
        {"a*b*", std::string(23, 'a') + std::string(45, 'b') + 'a'},
        {"ab[\\x00-\\xff]*", "ab" + every_byte},
        {"ab[\\x00-\\xff]*", "ba" + every_byte},
        {"[\\x00-\\xff]*", every_byte},
        {"a", "b" + std::string(1024, 'a')},
        {"(?:a?|b?)*", "abbaabbaabbaabba"},
        {"(?:ab){2,4}", "abababab"},
        {"(?:ab){2,4}", "ababababab"},
        {"", ""}, {"a*", ""}
    };
    for (const auto& item : cases) {
        const auto ast = crank::parse(item.pattern);
        const crank::RegularProgram dfa(ast);
        const bool expected = !crank::match_states(ast, item.subject, 0, true, unpruned()).empty();
        require(dfa.full_match(item.subject) == expected,
                "DFA entry preserves complete-state membership: " + item.pattern);
        // An accepting self-loop sink must emit every remaining endpoint.
        for (const auto start : {std::size_t{0}, item.subject.size() / 2, item.subject.size()})
            require(dfa.endpoints(item.subject, start) == complete_endpoints(ast, item.subject, start),
                    "sink endpoint preservation: " + item.pattern);
        ++differential_cases;
    }
}

void native_compile_limit_fallback() {
    // Independently compiled DFA is the control; the public handle must retain
    // the same language after deliberately declining regular compilation.
    crank::CompileOptions tiny;
    tiny.regular.max_dfa_states = 0;
    for (const std::string pattern : {"(?:ab|ba)*", "(a*)(b*)", "a*ab", "(?:a?|b?)*",
                                      "(?:ab){2,4}", "[^\\x00-\\xff]"}) {
        const crank::Program fallback(pattern, tiny);
        const crank::RegularProgram dfa(fallback.ast());
        for (const auto& subject : words("ab", 4)) {
            for (const bool prune : {false, true}) {
                crank::RunOptions options;
                options.optimize = prune;
                require(fallback.full_match(subject, options) == dfa.full_match(subject),
                        "DFA compilation decline preserves native membership");
                for (std::size_t start = 0; start <= subject.size(); ++start)
                    require(fallback.endpoints(subject, start, options) == dfa.endpoints(subject, start),
                            "DFA compilation decline preserves every endpoint and offset");
            }
            require(fallback.first_match(subject).has_value() == dfa.full_match(subject),
                    "native fallback retains capture witness availability");
            ++differential_cases;
        }
        crank::RunOptions forced;
        forced.backend = crank::Backend::Dfa;
        expect_exception<crank::ResourceLimit>([&] { (void)fallback.full_match("", forced); },
                "forced DFA compilation limit remains an unresolved request");
    }
    const crank::Program program("(ab|ba)*", tiny);
    crank::RunStatistics stats;
    require(program.full_match("abba", {}, &stats) && stats.backend == "native-ast",
            "automatic DFA decline uses the native request/return core");
    require(program.explain().find("Fallback: native AST requests and returns") != std::string::npos,
            "explanation identifies the retained fallback");
    crank::RunOptions empty_budget;
    empty_budget.occurrence.max_states = 0;
    expect_exception<crank::ResourceLimit>([&] { (void)program.full_match("abba", empty_budget); },
            "native fallback observes its explicit state quota");
    crank::CompileOptions no_nfa;
    no_nfa.regular.max_nfa_states = 0;
    require(crank::Program("(ab|ba)*", no_nfa).full_match("abba"),
            "NFA compilation decline uses the same native fallback");
}

void cached_history_dispatch() {
    const crank::Program original("(ab|ba)*");
    crank::RunOptions history;
    history.backend = crank::Backend::States;
    // Copies retain one immutable compiled program. Concurrent first history
    // queries must neither race compilation nor change ordinary DFA dispatch.
    std::vector<std::future<bool>> pending;
    for (unsigned worker = 0; worker < 4; ++worker) {
        pending.push_back(std::async(std::launch::async, [program = original, history] {
            for (unsigned i = 0; i < 8; ++i) {
                crank::RunStatistics stats;
                if (!program.full_match("abba", history) || program.full_match("abbaa", history)) return false;
                if (!program.full_match("abba", {}, &stats) || stats.backend != "dfa") return false;
                if (program.outcomes("abba").empty() || !program.first_match("abba")) return false;
                if (program.endpoints("abbax", 0, history) != std::vector<std::size_t>{0, 2, 4}) return false;
            }
            return true;
        }));
    }
    for (auto& task : pending) require(task.get(), "cached history queries preserve concurrent automatic dispatch");
    history.match.max_states = 0;
    crank::RunStatistics limited;
    expect_exception<crank::ResourceLimit>([&] { (void)original.full_match("abba", history, &limited); },
            "cached history matching keeps explicit scalar resource limits");
    require(limited.occurrence.limit_reached, "limited history diagnostics reach the public caller");
}

void bulk_scan_integration() {
    for (const std::string pattern : {"[ab]*", "(?:ab)*", "(?:[ab]c){1,40}",
                                      "([\\x00-\\xff]*)", "(?:abcde){0,20}"}) {
        const crank::Program program(pattern);
        const crank::RegularProgram dfa(program.ast());
        const crank::RegularFragment fragment(program.ast(), program.ast().root);
        for (const auto& subject : words("abc", 4)) {
            const bool expected = dfa.full_match(subject);
            require(fragment.full_match(subject) == expected, "bulk fragment agrees with independent DFA");
            require(program.full_match(subject) == expected, "ordinary Auto preserves bulk eligibility semantics");
            ++differential_cases;
        }
    }
    // Captures are transparent for Boolean scans but must remain observable
    // through outcomes, and Auto prefix enumeration must retain every endpoint.
    const crank::Program captures("([ab]*)([ab]*)");
    require(captures.outcomes("abba") == crank::match_states(captures.ast(), "abba", 0, true, unpruned()),
            "Boolean routing must not discard capture outcomes");
    require(captures.endpoints("abbac") == complete_endpoints(captures.ast(), "abbac", 0),
            "Boolean routing must not discard possible endpoints");

    struct Case { std::string pattern, body; std::size_t copies; };
    for (const auto& item : std::vector<Case>{{"[ab]*", "abba", 32}, {"(?:ab)*", "ab", 64},
             {"(?:[ab]c){1,40}", "ac", 40}, {"([\\x00-\\xff]*)", std::string{"\0\x80\xff", 3}, 40},
             {"(?:abcde){0,20}", "abcde", 20}}) {
        const crank::Program program(item.pattern);
        const crank::RegularProgram dfa(program.ast());
        std::string subject;
        for (std::size_t i = 0; i < item.copies; ++i) subject += item.body;
        crank::RunStatistics stats;
        require(program.full_match(subject, {}, &stats), "bulk Auto long valid construction");
        require(stats.backend == "regular-scan", "Auto should select a certified ordinary bulk scanner");
        for (const auto position : {std::size_t{0}, std::size_t{31}, std::size_t{32}, std::size_t{63}, subject.size() - 1}) {
            auto changed = subject; changed[position] = '!';
            require(program.full_match(changed) == dfa.full_match(changed), "bulk mutation at vector boundary");
        }
        crank::RunOptions forced;
        forced.backend = crank::Backend::Dfa;
        require(program.full_match(subject, forced, &stats) && stats.backend == "dfa",
                "forced DFA remains available despite bulk eligibility");
        forced.backend = crank::Backend::Auto;
        forced.optimize = false;
        require(program.full_match(subject, forced, &stats) && stats.backend == "dfa",
                "unoptimized Auto bypasses bulk routing");
    }
}

void fragment_accessors() {
    const auto bounded_ast = crank::parse("[ab]{2,4}");
    const crank::RegularFragment bounded(bounded_ast, bounded_ast.root);
    require(bounded.direct() && bounded.byte_class_repetition(), "bounded class is directly scannable");
    require(bounded.byte_class_prefix("aaaaaa") == 6 && !bounded.full_match("aaaaaa"),
            "class prefix ignores length bounds while full matching enforces them");
    require(bounded.byte_class_prefix("") == 0 && !bounded.full_match(""),
            "empty prefix is valid despite a positive lower bound");
    require(bounded.byte_class_prefix("aab!a") == 3, "class prefix reports first offending byte");
    require(bounded.byte_class_prefix("!aaa") == 0, "class prefix may fail at its first byte");
    require(bounded.regular_program().full_match("abba") == bounded.full_match("abba"),
            "retained regular program preserves the fragment language");

    std::bitset<256> set;
    set.set(0); set.set(128); set.set(255);
    const auto factory = crank::RegularFragment::byte_set(set);
    require(factory.byte_class_repetition(), "byte-set factory exposes its class predicate");
    require(factory.byte_class_prefix(std::string{"\0\x80\xff!", 4}) == 3,
            "factory prefix validates all unsigned byte values");
    expect_exception<std::logic_error>([&] { (void)factory.regular_program(); },
            "byte-set factory must not claim a compiled DFA");
    const auto pair_ast = crank::parse("ab");
    const crank::RegularFragment pair(pair_ast, pair_ast.root);
    require(pair.direct() && !pair.byte_class_repetition(), "literal sequence is not a one-byte predicate");
    expect_exception<std::logic_error>([&] { (void)pair.byte_class_prefix("ab"); },
            "unsupported prefix scanning must reject misuse");
    const auto correlated_ast = crank::parse("(?:ab|ba)*");
    const crank::RegularFragment correlated(correlated_ast, correlated_ast.root);
    require(!correlated.direct(), "correlated alternatives retain their DFA");
    expect_exception<std::logic_error>([&] { (void)correlated.byte_class_prefix("ab"); },
            "DFA language must not be weakened to independent class checks");
}

void correlated_copy_batches() {
    struct Case { std::string pattern; std::vector<std::string> bodies; bool nullable = false; };
    std::vector<Case> cases{
        {"(?:ab|ba)", {"ab", "ba"}},
        {"(?:aab|aba|baa)", {"aab", "aba", "baa"}},
        {"(?:\\x00\\x80|\\x80\\xff|\\xff\\x00)",
            {std::string{"\0\x80", 2}, std::string{"\x80\xff", 2}, std::string{"\xff\0", 2}}},
        // More than 128 distinguishable states retains compact class columns.
        {"(?:a{130}b|a{130}c)", {std::string(130, 'a') + 'b', std::string(130, 'a') + 'c'}}
    };
    // Every byte has a distinct continuation: the packed table requires wide
    // row offsets, so batched matching also exercises its wider dispatch case.
    Case wide{"(?:", {}};
    for (unsigned byte = 0; byte < 256; ++byte) {
        if (byte) wide.pattern += '|';
        wide.pattern += escape_byte(byte) + escape_byte((byte + 1) % 256);
        wide.bodies.push_back(std::string{static_cast<char>(byte), static_cast<char>((byte + 1) % 256)});
    }
    wide.pattern += ')';
    cases.push_back(std::move(wide));
    // Chunk width is a caller choice, not necessarily a fixed language width.
    cases.push_back({"(?:a?|bb)*", {"aaaa", "bbbb", "aabb", "bbaa"}, true});

    for (const auto& item : cases) {
        const auto ast = crank::parse(item.pattern);
        crank::RegularOptions options;
        options.max_dfa_states = 1024;
        const crank::RegularProgram program(ast, options);
        const crank::RegularFragment fragment(ast, ast.root, options);
        const auto width = item.bodies.front().size();
        for (const auto& body : item.bodies) {
            require(!crank::match_states(ast, body, 0, true, unpruned()).empty(),
                    "batched-copy fixture body has independent complete-state membership");
            ++differential_cases;
        }
        for (const std::size_t copies : {0U, 1U, 2U, 7U, 33U}) {
            std::string subject;
            for (std::size_t i = 0; i < copies; ++i)
                subject += item.bodies[(i * 73 + copies) % item.bodies.size()];
            const auto verify = [&] {
                std::size_t expected = 0;
                while (expected < copies && program.full_match(std::string_view(subject).substr(expected * width, width)))
                    ++expected;
                require(program.matching_copies(subject, width, copies) == expected,
                        "batched DFA must reset between bodies and stop at the first rejected copy");
                require(fragment.matching_copies(subject, width, copies) == expected,
                        "fragment batch fallback preserves independent per-body matching");
                ++oracle_cases;
            };
            verify();
            if (!copies) continue;
            for (const auto copy : {std::size_t{0}, copies / 2, copies - 1}) {
                for (const auto phase : {std::size_t{0}, width / 2, width - 1}) {
                    const auto position = copy * width + phase;
                    subject[position] ^= 0x55;
                    verify();
                    subject[position] ^= 0x55;
                }
            }
            if (copies >= 2) {
                subject.front() ^= 0x55;
                subject.back() ^= 0x55;
                verify();
            }
        }
        const auto invalid = [&](std::string_view subject, std::size_t body_width, std::size_t copies) {
            expect_exception<std::invalid_argument>([&] { (void)program.matching_copies(subject, body_width, copies); },
                    "DFA batches reject inconsistent dimensions before inspecting content");
            expect_exception<std::invalid_argument>([&] { (void)fragment.matching_copies(subject, body_width, copies); },
                    "fragment batches preserve dimension checks");
        };
        invalid("a", 0, 1);
        invalid("a", 2, 1);
        invalid("", 2, 1);
        invalid("abab", 2, 0);
        invalid("a", std::numeric_limits<std::size_t>::max(), 1);
        invalid("a", 1, std::numeric_limits<std::size_t>::max());
        for (const auto copies : {std::size_t{0}, std::size_t{7}, std::numeric_limits<std::size_t>::max()}) {
            const auto expected = item.nullable ? copies : 0;
            require(program.matching_copies("", 0, copies) == expected, "zero-width DFA bodies preserve empty-language membership");
            require(fragment.matching_copies("", 0, copies) == expected, "zero-width fragment batches preserve membership");
        }
        require(program.matching_copies("", 4, 0) == 0, "empty DFA batch has no copies to match");
        require(fragment.matching_copies("", 4, 0) == 0, "empty fragment batch has no copies to match");
    }
}

#if defined(__SIZEOF_INT128__)
std::string pseudo_word(std::size_t size, std::uint32_t seed, std::string_view alphabet) {
    std::string result;
    for (std::size_t i = 0; i < size; ++i) {
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        result += alphabet[seed % alphabet.size()];
    }
    return result;
}

bool in_class(std::string_view text, std::string_view alphabet) {
    return std::all_of(text.begin(), text.end(), [&](char byte) { return alphabet.find(byte) != alphabet.npos; });
}

bool split_oracle(std::string_view subject, std::string_view x_alphabet, std::string_view y_alphabet) {
    if (subject.size() % 2) return false;
    const auto half = subject.size() / 2;
    for (std::size_t x = 0; x <= half; ++x) {
        const auto y = half - x;
        const auto xs = subject.substr(0, x), ys = subject.substr(x, y);
        if (in_class(xs, x_alphabet) && in_class(ys, y_alphabet) &&
            ys == subject.substr(half, y) && xs == subject.substr(half + y)) return true;
    }
    return false;
}

void residual_reuse() {
    // Each candidate changes both its capture boundaries and the later copied
    // spans. Validate against a direct split enumerator with no Crank helpers.
    for (const bool overlapping_classes : {false, true}) {
        const std::string x_alphabet = "ab", y_alphabet = overlapping_classes ? "bc" : "ab";
        for (const bool forward : {false, true}) {
        const auto x_pattern = "(?<x>[" + x_alphabet + "]*)";
        const auto y_pattern = "(?<y>[" + y_alphabet + "]*)";
        const auto ast = crank::parse(forward ? "\\k<x>\\k<y>" + y_pattern + x_pattern
                                             : x_pattern + y_pattern + "\\k<y>\\k<x>");
        const auto plan = crank::make_affine_plan(ast);
        require(plan.has_value() && plan->residual(), "reuse fixture must retain one free variable");
        for (const auto x_size : {0U, 1U, 9U, 31U}) {
            for (const auto y_size : {1U, 17U, 65U}) {
                const auto x = pseudo_word(x_size, 317 + x_size, x_alphabet);
                const auto y = pseudo_word(y_size, 917 + y_size, y_alphabet);
                const auto valid = x + y + y + x;
                const auto verify = [&](const std::string& subject) {
                    const bool expected = split_oracle(subject, x_alphabet, y_alphabet);
                    crank::AffineStatistics stats;
                    require(plan->match(subject, &stats) == expected, "cached interval differs from direct split oracle");
                    require(stats.search_complete && !stats.resource_limited, "complete reuse search status");
                    if (stats.candidate_assignments != 0) {
                        crank::AffineMatchOptions budget;
                        budget.max_candidates = stats.candidate_assignments;
                        require(plan->match(subject, nullptr, budget) == expected, "exact candidate budget preserves answer");
                        --budget.max_candidates;
                        expect_exception<crank::ResourceLimit>([&] { (void)plan->match(subject, nullptr, budget); },
                            "one fewer candidate cannot produce a definitive answer");
                        budget.max_candidates = stats.candidate_assignments;
                        budget.max_validation_bytes = stats.validated_bytes + stats.equality_bytes;
                        require(plan->match(subject, nullptr, budget) == expected,
                                "logical validation budget preserves exact answer despite cache reuse");
                    }
                    ++oracle_cases;
                };
                verify(valid);
                for (const auto position : {std::size_t{0}, valid.size() / 4, valid.size() / 2, valid.size() - 1}) {
                    for (const char byte : std::string{"abc"}) {
                        auto changed = valid; changed[position] = byte;
                        verify(changed);
                    }
                }
            }
        }
        }
    }

    const auto rejected = crank::make_affine_plan(crank::parse("(?<x>[ab]*)(?<y>[ab]*)\\k<y>\\k<x>"));
    require(rejected.has_value(), "limited-layout fixture is certified");
    crank::AffineStatistics limited;
    crank::AffineMatchOptions budget;
    budget.max_candidates = 2;
    expect_exception<crank::ResourceLimit>([&] { (void)rejected->match("aaab", &limited, budget); },
            "limited-layout fixture must exhaust before its third candidate");
    require(limited.candidate_assignments == 2 && limited.values == std::vector<std::size_t>{1, 1},
            "candidate exhaustion publishes the last attempted assignment, not the next increment");

    // XYXY lets every split of a doubled word pass both copy comparisons.
    // Its deliberately wrong final literal keeps the search running after
    // successful class checks. The Y interval grows leftward across a byte
    // outside its class, which must invalidate the newly exposed fringe.
    for (const bool forward : {false, true}) {
        const std::string body = "(?<x>[ab]*)(?<y>[bc]*)";
        const std::string refs = "\\k<x>\\k<y>";
        const auto cached = crank::make_affine_plan(crank::parse((forward ? refs + body : body + refs) + "[ab]c"));
        require(cached.has_value() && cached->residual(), "class fringe fixture must retain residual search");
        for (const auto invalid_position : {0U, 1U, 17U, 31U, 63U}) {
            std::string word(64, 'b'); word[invalid_position] = 'a';
            const auto subject = word + word + "bb";
            crank::AffineStatistics stats;
            require(!cached->match(subject, &stats), "growing cached class cannot hide an invalid fringe");
            require(stats.candidate_assignments == 65 && stats.search_complete,
                    "all split candidates are checked despite repeated successful cached spans");
            crank::AffineMatchOptions exact;
            exact.max_candidates = stats.candidate_assignments;
            exact.max_validation_bytes = stats.validated_bytes + stats.equality_bytes;
            require(!cached->match(subject, nullptr, exact), "cache hits keep full logical validation charges");
            --exact.max_validation_bytes;
            expect_exception<crank::ResourceLimit>([&] { (void)cached->match(subject, nullptr, exact); },
                    "one fewer logical validation byte must exhaust even when work is reused");
            ++oracle_cases;
        }
    }
}
#endif
} // namespace

int main() {
    try {
        regular_differential();
        full_byte_alphabet();
        state_width_boundaries();
        accepting_sink_endpoints();
        native_compile_limit_fallback();
        cached_history_dispatch();
        bulk_scan_integration();
        fragment_accessors();
        correlated_copy_batches();
#if defined(__SIZEOF_INT128__)
        residual_reuse();
#endif
        std::cout << "PASS: " << checks << " execution checks; " << differential_cases
                  << " complete-state/DFA differential cases; " << oracle_cases
                  << " independent oracle cases\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
