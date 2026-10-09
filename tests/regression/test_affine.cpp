#include "affine_matcher.hpp"
#include "crank.hpp"
#include "fragment_matcher.hpp"

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
std::size_t checks = 0, differential_cases = 0, construction_cases = 0;
#if defined(__SIZEOF_INT128__)
constexpr bool affine_available = true;
#else
constexpr bool affine_available = false;
#endif

void require(bool condition, const std::string& context) {
    ++checks;
    if (!condition) throw std::runtime_error(context);
}

std::string bytes(std::string_view text) {
    std::ostringstream result;
    result << '"';
    for (unsigned char byte : text) {
        if (byte >= 32 && byte < 127 && byte != '\\' && byte != '"') result << byte;
        else result << "\\x" << std::hex << std::setw(2) << std::setfill('0') << unsigned(byte);
    }
    result << '"';
    return result.str();
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

crank::MatchOptions unpruned() {
    crank::MatchOptions result;
    result.prune = false;
    return result;
}

void differential(const crank::Program& program,
                  const std::optional<crank::AffinePlan>& plan,
                  std::string_view pattern, std::string_view subject) {
    // Complete outcomes retain every observable environment, with no arithmetic
    // pruning. They provide a separate execution path from Boolean projections.
    const bool expected = !crank::match_states(program.ast(), subject, 0, true, unpruned()).empty();
    const std::string context = bytes(pattern) + " on " + bytes(subject);
    if (plan) require(plan->match(subject) == expected, "affine differs: " + context);
    require(program.full_match(subject) == expected, "Auto differs: " + context);
    ++differential_cases;
}

void short_properties() {
    struct Group {
        std::vector<std::string> patterns;
        std::string alphabet;
        unsigned maximum;
        bool certified;
    };
    const std::vector<Group> groups{
        {{"(a*)(b*)\\2\\1", "(a*)(b*)\\1\\2",
          "\\k<x>\\k<y>(?<x>a*)(?<y>b*)",
          "(?<x>a?)b*\\k<x>", "(?<x>a{2})(?<y>b*)\\k<y>\\k<x>",
          "a{#n}b{#m}a{=n}b{=m}", "(?:ab|ba){#n}a{=n}",
          "(?<x>(?:ab|ba)*)\\k<x>", "(?<x>[ab]*)\\k<x>"}, "ab", 6, true},
        {{"(?<x>a*)(?<y>b*)c*\\k<y>\\k<x>",
          "(?<x>[ab]*)c{#n}c{=n}\\k<x>",
          "a{#n}b{#m}c{=n}a{=m}", "(?:ab|ba){#n}c{=n}",
          "(?:ab|ba){#n}(?:ac|ca){#m}a{=n}c{=m}",
          "(?<x>[ab]*)c\\k<x>"}, "abc", 4, true},
        {{"(?<x>[ab]*)(?<y>[cd]*)\\k<y>\\k<x>",
          "a{#n}b{#m}c{=n}d{=m}", "(?<x>[ab]*)c{#n}d{=n}\\k<x>"},
          "abcd", 4, true},
        {{"(?<x>[ab]*)(?<y>[ab]*)\\k<x>\\k<y>",
          "(?<x>a*)a*\\k<x>", "a{#n}a{#m}a{=n}a{=m}"}, "ab", 4, true},
        {{"(?<x>a)?\\k<x>", "(?:(?<x>a|b)\\k<x>)*",
          "\\k<x>(?:(?<x>a)|b)", "(?:(?<x>a*))?\\k<x>",
          "(?<x>(?<y>a*))\\k<y>\\k<x>",
          "(?<x>a{#n})b{=n}\\k<x>", "(?:a{#n}b{=n})*",
          "(?:(?:a{#n})?b{=n})", "(?:ab|abb){#n}a{=n}"}, "ab", 4, false},
        {{"(?:ab|acb){#n}d{=n}", "(?:a{#n}b{=n})|c"}, "abcd", 4, false},
    };
    for (const auto& group : groups) {
        const auto subjects = words(group.alphabet, group.maximum);
        for (const auto& pattern : group.patterns) {
            const crank::Program program(pattern);
            const auto plan = crank::make_affine_plan(program.ast());
            require(plan.has_value() == (affine_available && group.certified),
                    "wrong affine eligibility: " + bytes(pattern));
            for (const auto& subject : subjects) differential(program, plan, pattern, subject);
        }
    }
}

void constructed(std::string_view pattern, std::string_view subject, bool expected,
                 bool certified = true, bool compare_complete = true) {
    const crank::Program program{std::string(pattern)};
    const auto plan = crank::make_affine_plan(program.ast());
    require(plan.has_value() == (affine_available && certified), "construction eligibility: " + bytes(pattern));
    const std::string context = bytes(pattern) + " on " + bytes(subject);
    if (plan) require(plan->match(subject) == expected, "constructed affine answer: " + context);
    require(program.full_match(subject) == expected, "constructed Auto answer: " + context);
    if (compare_complete)
        require(!crank::match_states(program.ast(), subject, 0, true, unpruned()).empty() == expected,
                "constructed complete answer: " + context);
    ++construction_cases;
}

void independent_constructions() {
    for (std::size_t n = 0; n <= 12; ++n) {
        for (std::size_t m = 0; m <= 12; ++m) {
            const std::string a(n, 'a'), b(m, 'b');
            constructed("(a*)(b*)\\2\\1", a + b + b + a, true);
            constructed("a{#n}b{#m}c{=n}d{=m}", a + b + std::string(n, 'c') + std::string(m, 'd'), true);
            if (n && m) {
                auto wrong_order = a + b + b + a;
                std::swap(wrong_order[n - 1], wrong_order[n]);
                constructed("(a*)(b*)\\2\\1", wrong_order, false);
                // Identical total length and identical per-byte histogram do
                // not certify that the inferred segments occur in order.
                auto counts = a + b + std::string(n, 'c') + std::string(m, 'd');
                std::swap(counts[n - 1], counts[n]);
                constructed("a{#n}b{#m}c{=n}d{=m}", counts, false);
            }
        }
    }

    for (const auto& word : words("ab", 4)) {
        for (const auto& tail : words("cd", 3)) {
            constructed("(?<x>[ab]*)(?<y>[cd]*)\\k<y>\\k<x>", word + tail + tail + word, true);
            constructed("\\k<x>\\k<y>(?<x>[ab]*)(?<y>[cd]*)", word + tail + word + tail, true);
            if (word.size() >= 2 && word.front() != word.back()) {
                auto unequal_copy = word;
                std::swap(unequal_copy.front(), unequal_copy.back());
                constructed("(?<x>[ab]*)(?<y>[cd]*)\\k<y>\\k<x>",
                            word + tail + tail + unequal_copy, false);
            }
        }
    }

    for (std::size_t n : {std::size_t{1}, std::size_t{2}, std::size_t{7}, std::size_t{31}}) {
        std::string body;
        for (std::size_t i = 0; i < n; ++i) body += i % 2 ? "ba" : "ab";
        constructed("(?:ab|ba){#n}c{=n}", body + std::string(n, 'c'), true);
        if (n >= 2) {
            body.replace(0, 4, "aabb");
            constructed("(?:ab|ba){#n}c{=n}", body + std::string(n, 'c'), false);
            // The broader Cartesian product really does accept the same bytes.
            constructed("[ab]{4}(?:ab|ba){#n}c{2}c{=n}", body + std::string(n, 'c'), true);
        }
    }
    constructed("(?<x>(?:ab|ba)*)\\k<x>", "aabbaabb", false);
    constructed("(?<x>[ab]*)\\k<x>", "aabbaabb", true);
    constructed("(?<x>a*)\\k<x>", "", true);
    constructed("(?<x>)\\k<x>", "", true);
    constructed("\\k<x>(?<x>)", "", true);
    constructed("(?<x>a?)\\k<x>", "", true);
    constructed("(?<x>a)?\\k<x>", "", false, false);

    // An invariant marker count is necessary but not enough to turn a variable
    // width body into a fixed-width span. This supported language must retain
    // its exact general fallback until a stronger span certificate exists.
    constructed("(?:ab|acb){#n}d{=n}", "abacbdd", true, false);
    constructed("(?:ab|acb){#n}d{=n}", "acbabdd", true, false);
    constructed("(?:ab|acb){#n}d{=n}", "aabcbdd", false, false);
    constructed("(?:ab|acb){#n}(?:ef|eghf){=n}", "abacbefeghf", true, false);
    constructed("(?:ab|acb){#n}(?:ef|eghf){=n}", "abacbeefghf", false, false);
}

void byte_inputs() {
    const std::string alphabet{"\0\x80\xff", 3};
    for (const std::string pattern : {
             "(?<x>[\\x00\\x80]*)(?<y>\\xff*)\\k<y>\\k<x>",
             "\\x00{#n}\\x80{#m}\\xff{=n}\\x00{=m}"}) {
        const crank::Program program(pattern);
        const auto plan = crank::make_affine_plan(program.ast());
        require(plan.has_value() == affine_available, "binary affine certificate availability differs");
        for (const auto& subject : words(alphabet, 5)) differential(program, plan, pattern, subject);
    }
    std::string all;
    for (unsigned byte = 0; byte < 256; ++byte) all += static_cast<char>(byte);
    constructed("(?<x>[\\x00-\\xff]*)\\k<x>", all + all, true, true, false);
    auto swapped = all;
    std::swap(swapped[0], swapped[255]);
    constructed("(?<x>[\\x00-\\xff]*)\\k<x>", all + swapped, false, true, false);
}

void observation_properties() {
    const std::string boundary_bytes{"\0\x7f\x80\xff", 4};
    const std::vector<std::string> boundary_patterns{
        "(?<hi>[\\x80-\\xff]*)(?<lo>[\\x00-\\x7f]*)\\k<lo>\\k<hi>",
        "\\k<hi>\\k<lo>(?<hi>[\\x80-\\xff]*)(?<lo>[\\x00-\\x7f]*)"};
    const auto subjects = words(boundary_bytes, 5);
    for (const auto& pattern : boundary_patterns) {
        const crank::Program program(pattern);
        const auto plan = crank::make_affine_plan(program.ast());
        require(plan.has_value(), "signed-byte boundary certificate missing");
        for (const auto& subject : subjects) differential(program, plan, pattern, subject);
    }
    const auto boundary_plan = crank::make_affine_plan(crank::parse(boundary_patterns[0]));
    for (const std::size_t n : {0U, 1U, 15U, 16U, 17U, 31U, 32U, 33U, 63U, 64U, 65U, 127U}) {
        const std::size_t m = (7 * n + 5) % 67;
        std::string hi, lo;
        const unsigned char highs[]{0x80, 0xfe, 0xff, 0x81};
        const unsigned char lows[]{0x00, 0x7f, 0x01, 0x7e};
        for (std::size_t i = 0; i < n; ++i) hi += static_cast<char>(highs[i % 4]);
        for (std::size_t i = 0; i < m; ++i) lo += static_cast<char>(lows[i % 4]);
        const auto good = hi + lo + lo + hi;
        crank::AffineStatistics stats;
        require(boundary_plan->match(good, &stats), "long signed-byte construction must accept");
        require(stats.histogram_bytes == 0 && stats.direct_count_bytes == good.size(),
                "one selected half-range must use exactly one direct counting pass");
        require(stats.observed_bytes == stats.direct_count_bytes + stats.histogram_bytes + stats.delimiter_bytes,
                "observation statistics must report all actual counting and delimiter scans");
        if (n) {
            auto fractional = good;
            fractional.back() = '\x7f';
            require(!boundary_plan->match(fractional, &stats) && stats.arithmetic_rejected,
                    "crossing 0x80 boundary must expose fractional copy lengths");
        }
        if (n && m) {
            auto wrong_order = good;
            std::swap(wrong_order[n - 1], wrong_order[n]);
            require(!boundary_plan->match(wrong_order), "same signed-byte histogram must not hide ordering failure");
        }
        if (n >= 2) {
            auto wrong_copy = good;
            std::swap(wrong_copy[n + 2 * m], wrong_copy[n + 2 * m + 1]);
            require(!boundary_plan->match(wrong_copy), "same high-byte histogram must not hide unequal copies");
        }
    }
    const std::string mixed_pattern = "(?<x>[a-c]*)(?<y>[xz]*)(?<z>[0-9]*)\\k<z>\\k<y>\\k<x>";
    const crank::Program mixed(mixed_pattern);
    const auto mixed_plan = crank::make_affine_plan(mixed.ast());
    require(mixed_plan.has_value(), "mixed observation-mask certificate missing");
    for (const auto& subject : words("acxyz09", 4)) differential(mixed, mixed_plan, mixed_pattern, subject);
    const std::string good = "abcxz019019xzabc";
    crank::AffineStatistics stats;
    require(mixed_plan->match(good, &stats), "mixed contiguous/noncontiguous mask construction must accept");
    require(stats.histogram_bytes == good.size() && stats.direct_count_bytes == 0,
            "a noncontiguous selected mask must retain the shared histogram fallback");
    require(stats.observed_bytes == good.size(), "histogram fallback must account for one observation pass");
    auto bad = good;
    std::swap(bad[0], bad[3]);
    require(!mixed_plan->match(bad), "mixed-mask equations must not certify reordered spans");

    const auto ranges = crank::make_affine_plan(crank::parse(
        "(?<x>[a-c]*)(?<y>[x-z]*)(?<z>[0-9]*)\\k<z>\\k<y>\\k<x>"));
    require(ranges.has_value() && ranges->match(good, &stats), "two-range observation construction must accept");
    require(stats.histogram_bytes == 0 && stats.direct_count_bytes == 2 * good.size() &&
                stats.observed_bytes == stats.direct_count_bytes,
            "two selected range masks must report two direct passes");
}

void arithmetic_and_caps() {
    if (!affine_available) return;
    const auto plan = crank::make_affine_plan(crank::parse("(a*)(b*)\\2\\1"));
    require(plan.has_value(), "arithmetic test certificate missing");
    crank::AffineStatistics statistics;
    require(!plan->match("aaabb", &statistics), "fractional capture lengths must reject");
    require(statistics.arithmetic_rejected, "fractional solution should reject arithmetically");
    require(plan->match("aabbbbaa", &statistics), "integral solution must accept");
    require(!statistics.arithmetic_rejected, "statistics must reset after rejection");
    require(statistics.candidate_assignments == 1, "certified assignment must be unique");

    const auto signed_plan = crank::make_affine_plan(crank::parse("(?:ab){#n}(?:abb){#m}"));
    require(signed_plan.has_value(), "non-diagonal count matrix must be certified");
    require(!signed_plan->match("aaa", &statistics) && statistics.arithmetic_rejected,
            "negative solution must reject before conversion to an unsigned count");
    const auto offset_plan = crank::make_affine_plan(crank::parse("!(a*)(b*)\\2\\1"));
    require(offset_plan.has_value(), "fixed-offset matrix must be certified");
    require(!offset_plan->match("", &statistics) && statistics.arithmetic_rejected,
            "input shorter than the fixed offset must reject without subtraction underflow");

    std::string many, copies;
    for (unsigned i = 0; i < 9; ++i) {
        if (i == 8) {
            const auto at_limit = crank::make_affine_plan(crank::parse(many + copies));
            require(at_limit.has_value(), "eight independent variables should fit the certificate cap");
            require(at_limit->match("abcdefghabcdefgh"), "eight-variable solved construction must accept");
        }
        const auto name = std::string("v") + std::to_string(i);
        many += "(?<" + name + ">" + char('a' + i) + "*)";
        copies += "\\k<" + name + ">";
    }
    require(!crank::make_affine_plan(crank::parse(many + copies)), "variable cap must decline safely");
    const auto huge = std::to_string(std::numeric_limits<std::size_t>::max());
    require(!crank::make_affine_plan(crank::parse("a{#n}(?:a{" + huge + "}){=n}")),
            "overflowing coefficient must decline safely");

    crank::RegularOptions tiny;
    tiny.max_nfa_states = tiny.max_dfa_states = 1;
    require(!crank::make_affine_plan(crank::parse("(?<x>(?:a|ab)*)\\k<x>"), tiny),
            "DFA fallback compile caps must decline safely");
}

void delimiter_properties() {
    const std::vector<std::string> certified{
        "([ab]*):([ab]*):\\2:\\1",
        "(?<x>[ab]*):(?<y>[ab]*)\\k<x>\\k<y>",
        "(?<x>[ab]*):(?<y>[ab:]*)\\k<x>\\k<y>",
        "(?:a:|:b)(?<x>[ab]*):(?<y>[ab]*)\\k<y>\\k<x>",
        "::(?<x>[ab]*):(?<y>[ab]*)\\k<y>\\k<x>",
        "(?<x>[ab:]*):(?<y>[ab:]*)\\k<x>\\k<y>",
        "(?:a:|ab)(?<x>[ab]*):(?<y>[ab]*)\\k<y>\\k<x>"};
    const std::vector<std::string> declined{};
    const auto subjects = words("ab:", 5);
    for (bool eligible : {false, true}) {
        for (const auto& pattern : eligible ? certified : declined) {
            const crank::Program program(pattern);
            const auto plan = crank::make_affine_plan(program.ast());
            require(plan.has_value() == (eligible && affine_available),
                    "delimiter eligibility: " + bytes(pattern));
            for (const auto& subject : subjects) differential(program, plan, pattern, subject);
        }
    }
    for (const auto& left : words("ab", 3)) {
        for (const auto& right : words("ab", 3)) {
            constructed(certified[0], left + ':' + right + ':' + right + ':' + left, true);
            constructed(certified[3], "a:" + left + ':' + right + right + left, true);
            constructed(certified[3], ":b" + left + ':' + right + right + left, true);
            constructed(certified[4], "::" + left + ':' + right + right + left, true);
            if (!left.empty()) {
                auto misplaced_separator = left + ':' + right + ':' + right + ':' + left;
                std::swap(misplaced_separator[left.size() - 1], misplaced_separator[left.size()]);
                constructed(certified[0], misplaced_separator, false);
            }
        }
    }
    constructed(certified[0], "abab", false); // No separator at all.
    constructed(certified[0], "ab:ba:ba:ab", true);
    constructed(certified[0], "ab:ba:ab:ba", false); // Every histogram equation still holds.
    constructed(certified[2], "ab:b::aabb::a", true); // Later spans may contain separators.
    constructed(certified[3], ":aab:babaab", false); // Same fixed-prefix histogram, wrong word.
}

void malformed_ast_declines() {
    const auto base = crank::parse("(?<x>a*)b{#n}c{=n}\\k<x>");
    const auto node_of = [&](crank::Kind kind) {
        for (crank::NodeId i = 0; i < base.nodes.size(); ++i)
            if (base.nodes[i].kind == kind) return i;
        throw std::runtime_error("missing malformed-AST test fixture node");
    };
    const auto decline = [&](crank::Ast ast, const std::string& defect) {
        require(!crank::make_affine_plan(ast), "malformed AST should decline: " + defect);
    };
    auto ast = base;
    ast.root = ast.nodes.size();
    decline(std::move(ast), "invalid root");
    ast = base;
    ast.nodes[ast.root].children.front() = ast.nodes.size();
    decline(std::move(ast), "invalid child");
    ast = base;
    ast.nodes[ast.root].children.push_back(ast.root);
    decline(std::move(ast), "reachable cycle");
    ast = base;
    ast.nodes[node_of(crank::Kind::Capture)].children.clear();
    decline(std::move(ast), "capture without a body");
    ast = base;
    ast.nodes[node_of(crank::Kind::Repeat)].children.clear();
    decline(std::move(ast), "repeat without a body");
    ast = base;
    ast.nodes[node_of(crank::Kind::Reference)].children.push_back(node_of(crank::Kind::CharSet));
    decline(std::move(ast), "reference with a child");
    ast = base;
    ast.nodes[node_of(crank::Kind::CharSet)].kind = static_cast<crank::Kind>(255);
    decline(std::move(ast), "unknown node kind");
    for (const auto slot : {crank::no_id, std::size_t{0}, base.capture_names.size()}) {
        ast = base;
        ast.nodes[node_of(crank::Kind::Capture)].capture_id = slot;
        decline(std::move(ast), "invalid capture slot");
        ast = base;
        ast.nodes[node_of(crank::Kind::Reference)].capture_id = slot;
        decline(std::move(ast), "invalid reference slot");
    }
    ast = base;
    for (auto& node : ast.nodes) if (node.bind_count != crank::no_id) {
        node.bind_count = ast.count_names.size();
        break;
    }
    decline(std::move(ast), "invalid count binder");
    ast = base;
    for (auto& node : ast.nodes) if (node.use_count != crank::no_id) {
        node.use_count = ast.count_names.size();
        break;
    }
    decline(std::move(ast), "invalid count use");
    ast = base;
    for (auto& node : ast.nodes) if (node.bind_count != crank::no_id) {
        node.lower = 1;
        break;
    }
    decline(std::move(ast), "named count binder with a noncanonical lower bound");
    ast = base;
    for (auto& node : ast.nodes) if (node.use_count != crank::no_id) {
        node.upper = 3;
        break;
    }
    decline(std::move(ast), "named count use with a noncanonical upper bound");
    ast = base;
    ast.nodes[node_of(crank::Kind::Repeat)].lower = 2;
    ast.nodes[node_of(crank::Kind::Repeat)].upper = 1;
    decline(std::move(ast), "contradictory repetition bounds");
}

void large_plans() {
    if (!affine_available) return;
    const std::size_t n = 12000, m = 17000;
    std::string good = std::string(n, 'a') + std::string(2 * m, 'b') + std::string(n, 'a');
    const crank::Program program("(a*)(b*)\\2\\1");
    const auto plan = crank::make_affine_plan(program.ast());
    require(plan.has_value(), "large multi-capture certificate missing");
    require(plan->match(good), "large multi-capture construction must accept");
    crank::RunOptions tiny;
    tiny.match.max_states = tiny.match.max_steps = tiny.match.max_repeat = 1;
    require(program.full_match(good, tiny), "certified Auto must avoid general evaluator budgets");
    std::swap(good[n - 1], good[n]);
    require(!plan->match(good), "large histogram-preserving ordering defect must reject");
    require(!program.full_match(good, tiny), "invalid certified Auto must avoid general budgets");
}

void fragment_properties() {
    const std::vector<std::string> patterns{
        "", "a", "[ab]", "[^a]", "[ab][bc]", "(ab){2}", "[ab]*", "[ab]{1,3}",
        "(?:a[bc]){0,3}", "(?:ab|ba)*", "(?:ab|ba){2}", "(?:[ac]|[bd])*",
        "(?:a?|b)", "(?:a?){2}", "(?:){2,3}", "(?:ab){0}", "(?:ab){2,3}"};
    const auto subjects = words("abc", 5);
    for (const auto& pattern : patterns) {
        const auto ast = crank::parse(pattern);
        const crank::RegularFragment fragment(ast, ast.root);
        for (const auto& subject : subjects) {
            const bool expected = !crank::match_states(ast, subject, 0, true, unpruned()).empty();
            require(fragment.full_match(subject) == expected,
                    "regular fragment differs: " + bytes(pattern) + " on " + bytes(subject));
            ++differential_cases;
        }
    }

    const auto byte_ast = crank::parse("[\\x00\\x80\\xff]*");
    const crank::RegularFragment byte_fragment(byte_ast, byte_ast.root);
    for (const auto& subject : words(std::string{"\0\x7f\x80\xff", 4}, 4)) {
        const bool expected = std::all_of(subject.begin(), subject.end(), [](unsigned char byte) {
            return byte == 0 || byte == 0x80 || byte == 0xff;
        });
        require(byte_fragment.full_match(subject) == expected, "noncontiguous high-byte scan differs");
    }
    std::bitset<256> byte_set;
    byte_set.set(0); byte_set.set(0x80); byte_set.set(0xff);
    const auto direct = crank::RegularFragment::byte_set(byte_set);
    const auto empty_set = crank::RegularFragment::byte_set({});
    for (unsigned byte = 0; byte < 256; ++byte) {
        const std::string subject(1, static_cast<char>(byte));
        require(direct.full_match(subject) == byte_set[byte], "static byte-set classifier differs");
        require(!empty_set.full_match(subject), "empty byte set must reject every byte");
    }
    require(!direct.full_match(""), "one byte set does not match epsilon");
    require(!empty_set.full_match(""), "empty byte set language excludes epsilon");

    // The vector-of-roots constructor is used for fixed surrounding fragments.
    const auto pieces = crank::parse("ab(c)d");
    const auto& roots = pieces.nodes[pieces.root].children;
    require(roots.size() == 4, "test fixture must expose four concatenation roots");
    const crank::RegularFragment partial(pieces, std::vector<crank::NodeId>{roots[1], roots[2]});
    require(partial.full_match("bc") && !partial.full_match("abcd"), "root subset must define exact fragment");
    const crank::RegularFragment empty(pieces, std::vector<crank::NodeId>{});
    require(empty.full_match("") && !empty.full_match("a"), "empty root sequence must match only epsilon");

    crank::RegularOptions tiny;
    tiny.max_nfa_states = tiny.max_dfa_states = 1;
    bool limited = false;
    try { (void)crank::RegularFragment(pieces, roots.front(), tiny); }
    catch (const std::length_error&) { limited = true; }
    require(limited, "direct normal fragment construction must preserve DFA compile caps");
}

void repeated_fragments() {
    constexpr std::size_t copies = 73;
    for (std::size_t width = 1; width <= 10; ++width) {
        std::string pattern, body;
        for (std::size_t phase = 0; phase < width; ++phase) {
            pattern += phase % 2 ? "[x-z]" : "[a-c]";
            body += phase % 2 ? 'y' : 'b';
        }
        const auto ast = crank::parse(pattern);
        const crank::RegularFragment fragment(ast, ast.root);
        std::string subject;
        for (std::size_t i = 0; i < copies; ++i) subject += body;
        require(fragment.matching_copies(subject, width, copies) == copies,
                "valid repeated Cartesian body must accept");
        // Every position is visited, including each phase across the block
        // boundary and the scalar tail. Return the earliest bad body exactly.
        for (std::size_t i = 0; i < subject.size(); ++i) {
            const char saved = subject[i];
            subject[i] = '!';
            require(fragment.matching_copies(subject, width, copies) == i / width,
                    "wrong first failure for width=" + std::to_string(width) + " byte=" + std::to_string(i));
            subject[i] = saved;
        }
        subject[width * 40] = '!';
        subject[width * 4 + width - 1] = '!';
        require(fragment.matching_copies(subject, width, copies) == 4,
                "phase scanning must report first body, not first phase");
        require(fragment.matching_copies("", width, 0) == 0, "zero copies must accept an empty span");
    }

    for (const std::string pattern : {"ab|ba", "[ab][ab]", "[\\x00\\x80][\\x7f\\xff]"}) {
        const auto ast = crank::parse(pattern);
        const crank::RegularFragment fragment(ast, ast.root);
        const auto samples = words(pattern.front() == '[' && pattern[1] == '\\' ?
            std::string{"\0\x7f\x80\xff", 4} : std::string("ab"), 4);
        for (const auto& subject : samples) {
            if (subject.size() != 4) continue;
            std::size_t expected = 0;
            while (expected < 2 && !crank::match_states(ast,
                    std::string_view(subject).substr(2 * expected, 2), 0, true, unpruned()).empty()) ++expected;
            require(fragment.matching_copies(subject, 2, 2) == expected,
                    "repeated fragment differs from separately validated copies");
        }
    }

    const auto eps_ast = crank::parse("");
    const crank::RegularFragment epsilon(eps_ast, eps_ast.root);
    require(epsilon.matching_copies("", 0, 71) == 71, "empty fragment admits zero-width copies");
    const auto nonempty_ast = crank::parse("a");
    const crank::RegularFragment nonempty(nonempty_ast, nonempty_ast.root);
    require(nonempty.matching_copies("", 0, 71) == 0, "nonempty fragment rejects zero-width copies");
    for (const auto& dimensions : std::vector<std::pair<std::size_t, std::size_t>>{
            {0, 1}, {1, 0}, {2, 1}, {std::numeric_limits<std::size_t>::max(), 2}}) {
        bool caught = false;
        try { (void)nonempty.matching_copies("a", dimensions.first, dimensions.second); }
        catch (const std::invalid_argument&) { caught = true; }
        require(caught, "invalid or overflowing copy dimensions must throw");
    }
}

void long_pair_fragments() {
    constexpr std::size_t copies = 73;
    // These inputs enter the 32-lane pair-set reduction, cross two block
    // boundaries, and retain a scalar tail at every implemented stride.
    for (std::size_t width = 1; width <= 8; ++width) {
        std::string pattern, subject;
        for (std::size_t phase = 0; phase < width; ++phase)
            pattern += phase % 2 ? "[+-]" : "[\\x00\\xff]";
        for (std::size_t copy = 0; copy < copies; ++copy)
            for (std::size_t phase = 0; phase < width; ++phase)
                subject += phase % 2 ? (copy % 2 ? '-' : '+') : static_cast<char>(copy % 2 ? 0xff : 0);
        const auto ast = crank::parse(pattern);
        const crank::RegularFragment fragment(ast, ast.root);
        require(fragment.matching_copies(subject, width, copies) == copies,
                "long pair-set Cartesian construction must accept");
        for (const std::size_t copy : {0U, 30U, 31U, 32U, 33U, 62U, 63U, 64U, 72U}) {
            for (std::size_t phase = 0; phase < width; ++phase) {
                const auto position = copy * width + phase;
                const char saved = subject[position];
                for (unsigned byte = 0; byte < 256; ++byte) {
                    subject[position] = static_cast<char>(byte);
                    const bool allowed = phase % 2 ? byte == '+' || byte == '-' : byte == 0 || byte == 0xff;
                    require(fragment.matching_copies(subject, width, copies) == (allowed ? copies : copy),
                            "pair-set scan first failure differs at width=" + std::to_string(width) +
                            " copy=" + std::to_string(copy) + " phase=" + std::to_string(phase) +
                            " byte=" + std::to_string(byte));
                }
                subject[position] = saved;
            }
        }
        subject[40 * width] = '!';
        subject[4 * width + width - 1] = '!';
        require(fragment.matching_copies(subject, width, copies) == 4,
                "pair-set phases must preserve earliest failing body");
    }
    // Short literals share the stride scanner; widths above eight retain their
    // separate literal path. Exercise both sides of that dispatch boundary.
    for (std::size_t width = 1; width <= 10; ++width) {
        const auto literal = std::string("abcdefghij").substr(0, width);
        const auto ast = crank::parse(literal);
        const crank::RegularFragment fragment(ast, ast.root);
        std::string subject;
        for (std::size_t copy = 0; copy < copies; ++copy) subject += literal;
        require(fragment.matching_copies(subject, width, copies) == copies,
                "long literal repetition must accept");
        for (std::size_t position = 0; position < subject.size(); ++position) {
            const char saved = subject[position];
            subject[position] = '!';
            require(fragment.matching_copies(subject, width, copies) == position / width,
                    "literal dispatch must return the earliest failing copy");
            subject[position] = saved;
        }
    }
}
} // namespace

int main() {
    try {
        fragment_properties();
        repeated_fragments();
        long_pair_fragments();
        if (affine_available) {
            short_properties();
            independent_constructions();
            byte_inputs();
            observation_properties();
            arithmetic_and_caps();
            delimiter_properties();
            malformed_ast_declines();
            large_plans();
        } else {
            require(!crank::make_affine_plan(crank::parse("(a*)(b*)\\2\\1")),
                    "unsupported arithmetic platform must decline the affine certificate");
            std::cout << "SKIP: affine certificate suites require compiler 128-bit integers; "
                         "regular-fragment validation remains active\n";
        }
        std::cout << "PASS: " << checks << " affine/fragment checks; "
                  << differential_cases << " complete-state differential cases; "
                  << construction_cases << " independent construction cases\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
