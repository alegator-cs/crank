#include "external_regular.hpp"
#include "fragment_matcher.hpp"

#include <atomic>
#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
std::size_t checks = 0, differential_cases = 0, external_answers = 0;

void require(bool value, const std::string& context) {
    ++checks;
    if (!value) throw std::runtime_error(context);
}

template<class Exception, class Function>
void expect_exception(Function&& function, const std::string& context) {
    bool caught = false;
    try { function(); } catch (const Exception&) { caught = true; }
    require(caught, context);
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

std::string escaped_byte(unsigned byte) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result = "\\x00";
    result[2] = digits[byte >> 4];
    result[3] = digits[byte & 15];
    return result;
}

void compare_external(const crank::ExternalRegularProgram& external,
                      std::string_view subject, bool expected,
                      const std::string& context) {
    const auto result = external.try_full_match(subject);
    if (external.available()) {
        require(result.has_value(), context + ": ordinary request should finish in JIT");
        require(*result == expected, context + ": native/JIT membership disagreement");
        ++external_answers;
    } else {
        require(!result, context + ": unavailable engine must return no answer");
    }
    ++differential_cases;
}

void semantic_differential() {
    const std::vector<std::string> patterns{
        "", "a", ".", "[^ab]", "[\\x00-\\xff]", "[^\\x00-\\xff]",
        "(?:[^\\x00-\\xff])*", "a|", "(?:a?|b?)*", "(?:ab|ba)*",
        "(?:a|ab)*b", "(?:ab|ba){0,3}", "(?:a[ab]b){1,3}",
        "((ab|b)+)(a*)", "(?:[a-c]{1,2}|[b-d]{2,3})*", "(?:a|aa)*b",
        "[\\x00\\x0a\\x0d\\x7f\\x80\\xff]*", "(?:a{0}){0,3}",
        "(?:ab){2}", "(?:ab){2,}", "(?:ab){0,2}", "(?:ab)?",
        "(?(DEFINE)(?<A>ab|ba))(?&A)*", "(?(DEFINE)(?<A>a?))(?&A)b(?&A)",
        "(?(DEFINE)(?<A>a|b)(?<B>(?&A)(?&A)))(?&B){0,3}",
        "(?(DEFINE)(?<unused>(?&unused)))(?:ab|ba)*",
        "(?(DEFINE)(?<A>[\\x00\\xff]|ab))(?&A)*"
    };
    auto subjects = words("ab", 5);
    subjects.emplace_back("\n");
    subjects.emplace_back("\r\n");
    subjects.emplace_back(1, '\0');
    subjects.emplace_back("a\0b", 3);
    subjects.emplace_back(1, static_cast<char>(0x80));
    subjects.emplace_back(1, static_cast<char>(0xff));
    subjects.emplace_back(300, 'a');
    subjects.emplace_back(std::string(300, 'a') + 'b');
    std::string pairs;
    for (unsigned i = 0; i < 200; ++i) pairs += i % 3 ? "ab" : "ba";
    subjects.push_back(pairs);

    for (const auto& pattern : patterns) {
        const auto ast = crank::parse(pattern);
        const crank::RegularProgram native(ast);
        const crank::ExternalRegularProgram external(ast, ast.root);
        const crank::RegularFragment fragment(ast, ast.root);
        for (const auto& subject : subjects) {
            const bool expected = native.full_match(subject);
            compare_external(external, subject, expected, pattern);
            crank::RegularStatistics stats;
            require(fragment.full_match(subject, &stats, crank::ExternalRegularPolicy::Native) == expected,
                    pattern + ": forced native fragment");
            require(fragment.full_match(subject, nullptr, crank::ExternalRegularPolicy::Auto) == expected,
                    pattern + ": automatic fragment");
            require(fragment.full_match(subject, &stats, crank::ExternalRegularPolicy::Pcre2) == expected,
                    pattern + ": requested JIT with exact fallback");
            require(!fragment.external_would_run(subject.size(), crank::ExternalRegularPolicy::Native),
                    pattern + ": Native policy never dispatches externally");
            if (fragment.direct())
                require(!fragment.external_would_run(subject.size(), crank::ExternalRegularPolicy::Pcre2),
                        pattern + ": direct proven scan retains priority");
        }
    }
}

void all_bytes() {
    // Two-byte words encode a permutation of the entire byte alphabet.
    // Exhausting all 65,536 inputs independently checks serializer escaping,
    // class/range endpoints, anchoring, signed-char handling, and embedded NUL.
    std::string pattern = "(?:";
    for (unsigned byte = 0; byte < 256; ++byte) {
        if (byte) pattern += '|';
        pattern += escaped_byte(byte) + escaped_byte((73 * byte + 19) % 256);
    }
    pattern += ')';
    const auto ast = crank::parse(pattern);
    crank::RegularOptions options;
    options.max_dfa_states = 1024;
    const crank::RegularProgram native(ast, options);
    const crank::ExternalRegularProgram external(ast, ast.root);
    for (unsigned first = 0; first < 256; ++first)
        for (unsigned second = 0; second < 256; ++second) {
            const char bytes[]{static_cast<char>(first), static_cast<char>(second)};
            const std::string_view subject(bytes, 2);
            const bool expected = second == (73 * first + 19) % 256;
            require(native.full_match(subject) == expected, "independent two-byte permutation oracle");
            compare_external(external, subject, expected, "all-byte permutation");
        }
    compare_external(external, "", false, "full anchor rejects empty input");
    compare_external(external, std::string(3, '\0'), false, "full anchor rejects trailing bytes");

    const auto all = crank::parse("[\\x00-\\xff]*");
    const crank::ExternalRegularProgram every_byte(all, all.root);
    std::string corpus;
    for (unsigned byte = 0; byte < 256; ++byte) corpus += static_cast<char>(byte);
    compare_external(every_byte, corpus, true, "all-byte class includes newline and high bytes");
    compare_external(every_byte, std::string_view{}, true, "empty string_view may have null data");
}

void fallback_and_limits() {
    const auto ast = crank::parse("(?:a|aa)*b");
    const crank::RegularFragment fragment(ast, ast.root);
    const crank::ExternalRegularProgram external(ast, ast.root);
    require(!fragment.direct(), "limit fixture requires the generic regular path");
    const std::string accepted = std::string(600, 'a') + 'b';
    const std::string rejected = std::string(300, 'a') + 'b' + std::string(300, 'a') + 'c';
    if (external.available()) {
        require(!external.try_full_match(accepted, 0),
                "zero PCRE2 work budget returns no answer, never false");
        require(fragment.external_available(), "nondirect fragment retains its optional JIT");
        require(fragment.external_would_run(accepted.size(), crank::ExternalRegularPolicy::Pcre2),
                "forced external policy selects available JIT");
        require(!fragment.external_would_run(crank::external_auto_min_bytes - 1,
                                            crank::ExternalRegularPolicy::Auto),
                "small automatic fragments retain native execution");
        require(fragment.external_would_run(crank::external_auto_min_bytes,
                                           crank::ExternalRegularPolicy::Auto),
                "automatic external size threshold is inclusive");
    }
    require(fragment.full_match(accepted, nullptr, crank::ExternalRegularPolicy::Pcre2, 0),
            "JIT resource failure falls back to exact positive answer");
    require(!fragment.full_match(rejected, nullptr, crank::ExternalRegularPolicy::Pcre2, 0),
            "JIT resource failure falls back to exact negative answer");
    require(fragment.full_match(accepted, nullptr, crank::ExternalRegularPolicy::Auto, 0),
            "automatic policy preserves fallback after JIT limit");

    const auto direct_ast = crank::parse("[ab]*");
    const crank::RegularFragment direct(direct_ast, direct_ast.root);
    require(direct.direct() && !direct.external_available(), "direct class kernel avoids JIT compilation");
    require(direct.full_match(std::string(600, 'a'), nullptr, crank::ExternalRegularPolicy::Pcre2, 0),
            "external budget cannot disable direct class scan");
}

void structural_routing_policy() {
    struct Fixture {
        std::string pattern, valid_unit;
        bool prefer_large_native;
    };
    const std::vector<Fixture> fixtures{
        {"(?:ab|ba)*", "ab", true},
        {"(((ab|ba))*)", "ba", true},
        {"(?:a{1}b|b{2})*", "ab", true},
        {"(?:(?:ab|ba)|(?:aa|bb))*", "ab", true},
        {"(?:ab|ba){3,}", "ab", true},
        {"(?:a|bb)*", "bb", false},
        {"(?:abc|cba)*", "abc", false},
        {"(?:ab|ba){0,12}", "ab", false},
        {"x(?:ab|ba)*", "ab", false},
        {"(?:ab|)*", "ab", false},
        {"(?:a+b|ba)*", "ab", false}
    };
    for (const auto& fixture : fixtures) {
        const auto ast = crank::parse(fixture.pattern);
        const crank::RegularFragment fragment(ast, ast.root);
        const crank::RegularProgram native(ast);
        require(!fragment.direct(), fixture.pattern + ": routing fixture uses a generic fragment");
        for (const std::size_t length : {255U, 256U, 4094U, 4095U, 4096U, 4097U, 16384U}) {
            const bool available = fragment.external_available();
            const bool automatic = available && length >= crank::external_auto_min_bytes &&
                !(fixture.prefer_large_native && length >= 4096);
            require(fragment.external_would_run(length, crank::ExternalRegularPolicy::Auto) == automatic,
                    fixture.pattern + ": structural Auto routing at size boundary");
            require(fragment.external_would_run(length, crank::ExternalRegularPolicy::Pcre2) == available,
                    fixture.pattern + ": explicit PCRE2 overrides the Auto size heuristic");
            require(!fragment.external_would_run(length, crank::ExternalRegularPolicy::Native),
                    fixture.pattern + ": explicit Native stays local at all sizes");

            std::string subject;
            while (subject.size() < length) subject += fixture.valid_unit;
            subject.resize(length);
            const bool expected = native.full_match(subject);
            require(fragment.full_match(subject, nullptr, crank::ExternalRegularPolicy::Auto) == expected,
                    fixture.pattern + ": routing policy preserves exact native membership");
            require(fragment.full_match(subject, nullptr, crank::ExternalRegularPolicy::Pcre2) == expected,
                    fixture.pattern + ": forced JIT agrees with exact native membership");
            subject.back() = '!';
            require(fragment.full_match(subject, nullptr, crank::ExternalRegularPolicy::Auto) ==
                    native.full_match(subject), fixture.pattern + ": large late rejection remains exact");
        }
    }

    const auto ast = crank::parse("(?:a|b)*");
    const crank::RegularFragment direct(ast, ast.root);
    require(direct.direct(), "width-one alternation is already a direct class scan");
    for (const auto policy : {crank::ExternalRegularPolicy::Auto, crank::ExternalRegularPolicy::Pcre2})
        require(!direct.external_would_run(16384, policy),
                "structural routing and explicit PCRE2 preserve direct-scan priority");
}

void declined_ast_forms() {
    for (const auto& pattern : {"(a*)\\1", "a{#n}b{=n}", "(?<S>a(?&S)b|)"}) {
        const auto ast = crank::parse(pattern);
        const crank::ExternalRegularProgram unsupported(ast, ast.root);
        require(!unsupported.available() && !unsupported.try_full_match(""),
                "stateful constructs do not cross the external Boolean boundary");
    }

    const auto defined = crank::parse("(?(DEFINE)(?<unused>a))b");
    const crank::ExternalRegularProgram declarations(defined, defined.root);
    compare_external(declarations, "b", true, "unused definitions execute epsilon");
    compare_external(declarations, "a", false, "definition bodies do not execute");

    // Bound repeated traversal of a small shared DAG even when its leaves emit
    // no bytes, so the serialization byte cap alone cannot bound construction.
    crank::Ast epsilon_dag;
    epsilon_dag.nodes.resize(19);
    for (std::size_t i=0; i<epsilon_dag.nodes.size(); ++i) {
        epsilon_dag.nodes[i].kind=crank::Kind::Concat;
        if (i) epsilon_dag.nodes[i].children={i-1,i-1};
    }
    epsilon_dag.root=epsilon_dag.nodes.size()-1;
    require(!crank::ExternalRegularProgram(epsilon_dag,epsilon_dag.root).available(),
            "exponential acyclic expansion hits work cap");
    crank::Ast oversized_source;
    oversized_source.nodes.resize(16385);
    require(!crank::ExternalRegularProgram(oversized_source,0).available(),
            "source cap checked before scratch allocation");
    auto invalid_call=crank::parse("(?(DEFINE)(?<A>a))(?&A)");
    for (auto& node:invalid_call.nodes) if(node.kind==crank::Kind::Call) node.call_target=crank::no_id;
    require(!crank::ExternalRegularProgram(invalid_call,invalid_call.root).available(),
            "invalid call target declined");
    auto false_flags=crank::parse("(?(DEFINE)(?<A>(a)\\2))(?&A)");
    false_flags.has_references=false;false_flags.has_calls=false;
    require(!crank::ExternalRegularProgram(false_flags,false_flags.root).available(),
            "actual executed reference overrides source flags");

    auto malformed = crank::parse("a");
    const crank::ExternalRegularProgram invalid_root(malformed, crank::no_id);
    require(!invalid_root.available(), "invalid root is declined safely");
    malformed.nodes[malformed.root].kind = crank::Kind::Concat;
    malformed.nodes[malformed.root].children = {malformed.root};
    const crank::ExternalRegularProgram cycle(malformed, malformed.root);
    require(!cycle.available(), "structural cycle is declined safely");

    auto large_repeat = crank::parse("a*");
    large_repeat.nodes[large_repeat.root].lower = 65536;
    large_repeat.nodes[large_repeat.root].upper = 65536;
    const crank::ExternalRegularProgram oversized(large_repeat, large_repeat.root);
    require(!oversized.available(), "oversized PCRE2 bounded repetition is declined");

    // An empty Alt in a directly supplied AST means the empty language, while
    // an empty Concat means epsilon. Their source syntax need not be identical.
    crank::Ast ast;
    ast.nodes.resize(1);
    ast.nodes[0].kind = crank::Kind::Alt;
    const crank::ExternalRegularProgram empty_alternation(ast, 0);
    compare_external(empty_alternation, "", false, "empty Alt rejects epsilon");
    ast.nodes[0].kind = crank::Kind::Concat;
    const crank::ExternalRegularProgram empty_concatenation(ast, 0);
    compare_external(empty_concatenation, "", true, "empty Concat accepts epsilon");
    compare_external(empty_concatenation, "a", false, "empty Concat consumes no bytes");

    auto zero_cycle = crank::parse("(a){0}");
    const auto capture = zero_cycle.nodes[zero_cycle.root].children.at(0);
    zero_cycle.nodes[capture].children = {capture};
    expect_exception<std::invalid_argument>([&] {
        const crank::RegularFragment invalid(zero_cycle, zero_cycle.root);
    }, "a skipped zero-repeat body cannot expose a structural cycle to scanner analysis");

    const auto recursive_ast = crank::parse("(?<S>a(?&S)b|)");
    for (std::size_t node = 0; node < recursive_ast.nodes.size(); ++node)
        if (recursive_ast.nodes[node].kind == crank::Kind::CharSet &&
            recursive_ast.nodes[node].characters.test('a')) {
            const crank::RegularFragment subfragment(recursive_ast, node);
            require(subfragment.full_match("a") && !subfragment.full_match("b"),
                    "regular subtree inside recursive AST remains independently valid");
        }
}

void concurrent_reuse() {
    const auto ast = crank::parse("(?:(?:ab|ba)+c|a[0-9]{1,3})*");
    const crank::RegularProgram native(ast);
    const crank::RegularFragment fragment(ast, ast.root);
    const crank::ExternalRegularProgram external(ast, ast.root);
    const std::vector<std::string> subjects{
        "", "abc", "bac", "ababc", "a12", "a1234", "abab", "a12baca3",
        "a0abcbaca999", std::string(600, 'a'), "abc\n", std::string("abc\0", 4)
    };
    std::vector<bool> expected;
    for (const auto& subject : subjects) expected.push_back(native.full_match(subject));
    std::atomic<bool> valid{true};
    std::vector<std::thread> threads;
    constexpr unsigned worker_count = 6, repetitions = 1000;
    for (unsigned worker = 0; worker < worker_count; ++worker)
        threads.emplace_back([&, worker] {
            for (unsigned iteration = 0; iteration < repetitions; ++iteration) {
                const auto index = (iteration * 7 + worker) % subjects.size();
                const auto& subject = subjects[index];
                const auto answer = external.try_full_match(subject, iteration % 2 ? 0 : 1000000);
                if ((answer && *answer != expected[index]) ||
                    fragment.full_match(subject, nullptr, crank::ExternalRegularPolicy::Pcre2,
                                        iteration % 2 ? 0 : 1000000) != expected[index])
                    valid.store(false, std::memory_order_relaxed);
            }
        });
    for (auto& thread : threads) thread.join();
    require(valid.load(), "shared immutable code and per-thread scratch preserve answers");
    differential_cases += worker_count * repetitions;
}
} // namespace

int main() {
    try {
        semantic_differential();
        all_bytes();
        fallback_and_limits();
        structural_routing_policy();
        declined_ast_forms();
        concurrent_reuse();
        std::cout << "PASS: " << checks << " external checks, " << differential_cases
                  << " differential/oracle cases, " << external_answers << " JIT answers; adapter "
                  << (crank::ExternalRegularProgram::compiled_with_pcre2() ? "enabled" : "disabled") << '\n';
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
