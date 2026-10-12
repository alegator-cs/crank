#include "regex_ast.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// Compile this same file once against baseline sources and once against the
// modified sources, then compare stdout byte-for-byte. No matching/setup work
// is measured here. Every successfully parsed AST field is serialized, including
// node order and capture/count/path IDs. Rejected sources retain exact messages.
namespace {
std::string hex(std::string_view text) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(text.size() * 2);
    for (unsigned char byte : text) {
        result += digits[byte >> 4];
        result += digits[byte & 15];
    }
    return result;
}

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

template <typename T>
void values(std::ostream& out, const std::vector<T>& items) {
    out << '[';
    for (const auto& item : items) out << item << ',';
    out << ']';
}

std::string snapshot(const crank::Ast& ast) {
    require(ast.root < ast.nodes.size(), "invalid AST root");
    require(!ast.capture_nodes.empty() && ast.capture_nodes[0] == ast.root,
            "whole-match capture differs from AST root");
    require(ast.capture_names.size() == ast.capture_nodes.size(),
            "capture metadata widths differ");
    require(ast.count_names.size() == ast.count_nodes.size(),
            "count metadata widths differ");
    std::ostringstream out;
    out << "root=" << ast.root << ";flags=" << ast.has_references << ast.has_counts
        << ast.has_calls << ast.has_occurrence_paths << ast.has_extended
        << ast.regular() << ";captures=";
    for (const auto& name : ast.capture_names) out << hex(name) << ',';
    values(out, ast.capture_nodes);
    out << ";counts=";
    for (const auto& name : ast.count_names) out << hex(name) << ',';
    values(out, ast.count_nodes);
    out << ";paths=";
    for (const auto& path : ast.count_paths) {
        out << '{' << static_cast<unsigned>(path.anchor) << ',' << path.parents
            << ',' << static_cast<unsigned>(path.metric) << ':';
        for (const auto& step : path.steps) {
            out << static_cast<unsigned>(step.source) << ',' << step.id << ',';
            if (step.index) out << *step.index;
            else out << '-';
            out << ';';
        }
        out << '}';
    }
    out << ";nodes=";
    for (crank::NodeId id = 0; id < ast.nodes.size(); ++id) {
        const auto& node = ast.nodes[id];
        out << '{' << id << ':' << static_cast<unsigned>(node.kind) << ','
            << node.characters.to_string() << ',';
        values(out, node.children);
        for (auto child : node.children)
            require(child < id, "structural AST traversal no longer acyclic");
        out << ',' << node.lower << ',';
        if (node.upper) out << *node.upper;
        else out << '-';
        out << ',' << node.capture_id << ',' << node.call_target << ','
            << node.bind_count << ',' << node.use_count << ',' << node.count_path
            << ',' << node.forward << ',' << node.nullable << ','
            << node.minimum_length << ',' << node.stateful << '}';
    }
    return out.str();
}

void diagnostic(std::string_view source, std::string_view expected) {
    try {
        (void)crank::parse(source);
        throw std::runtime_error("expected invalid source to be rejected");
    } catch (const std::invalid_argument& error) {
        require(error.what() == expected, "contractual diagnostic changed");
    }
}

void basic_contracts() {
    auto empty = crank::parse("");
    require(empty.nodes.size() == 1 && empty.root == 0 &&
            empty.nodes[0].kind == crank::Kind::Empty, "empty AST changed");
    auto literal = crank::parse("a");
    require(literal.nodes.size() == 1 && literal.root == 0 &&
            literal.nodes[0].kind == crank::Kind::CharSet &&
            literal.nodes[0].characters.test('a'), "literal AST changed");
    auto pair = crank::parse("ab");
    require(pair.nodes.size() == 3 && pair.root == 2 &&
            pair.nodes[2].kind == crank::Kind::Concat &&
            pair.nodes[2].children == std::vector<crank::NodeId>{0, 1},
            "concatenation AST changed");
    auto forward = crank::parse(R"(\k<x>(?<x>a|b))");
    require(forward.nodes[0].forward && forward.nodes[0].capture_id == 1,
            "forward capture reference direction changed");
    auto backward = crank::parse(R"((?<x>a|b)\k<x>)");
    require(!backward.nodes.back().forward, "concat unexpectedly marked forward");
    const auto& backward_ref = backward.nodes[backward.nodes.size() - 2];
    require(backward_ref.kind == crank::Kind::Reference &&
            !backward_ref.forward && backward_ref.capture_id == 1,
            "backward capture reference direction changed");
    diagnostic(")", "regex byte 0: unexpected closing parenthesis");
    diagnostic("(", "regex byte 1: unclosed group");
    diagnostic("a**", "regex byte 2: stacked, lazy, and possessive quantifiers are unsupported");
    diagnostic("^", "regex byte 1: anchors and assertions are unsupported");
    diagnostic("\\", "regex byte 1: trailing escape");
    diagnostic("[z-a]", "regex byte 4: descending character range");
    diagnostic(R"([\d-a])", "regex byte 5: a range endpoint must be one byte");
    diagnostic("a{1,0}", "regex byte 6: repetition upper bound is smaller than its lower bound");
    diagnostic("a{1", "regex byte 3: expected closing brace after repetition count");
    diagnostic("(?&missing)", "regex byte 11: call to an unknown capture name 'missing'");
    diagnostic(R"((\1))", "regex byte 3: reference to a syntactically open capture is unsupported");
    diagnostic(R"(\k<missing>a)", "regex byte 11: unknown capture name 'missing'");
    diagnostic("a{=n}b{#n}", "regex byte 5: count reference requires a previously declared binder: 'n'");
    diagnostic("(a?){#n}", "regex byte 8: a named count body must provably consume at least one byte");
}

void short_strings(std::vector<std::string>& result, std::string prefix,
                   unsigned remaining) {
    result.push_back(prefix);
    if (remaining == 0) return;
    for (char byte : std::string_view("a|()*?{}[]\\"))
        short_strings(result, prefix + byte, remaining - 1);
}

std::uint32_t next(std::uint32_t& state) {
    state = state * 1664525U + 1013904223U;
    return state;
}

std::string regular(std::uint32_t& state, unsigned depth) {
    static const std::vector<std::string> atoms = {
        "", "a", "b", "ab", "[ab]", "[^a]", ".", R"(\d)", R"(\x00)",
        "[]a]", R"([\x00-\xff])", " ", "(?:)", "(?:a|)"};
    static const std::vector<std::string> suffixes = {
        "", "*", "+", "?", "{0}", "{1}", "{0,2}", "{2,}", "{2,3}"};
    auto atom = atoms[next(state) % atoms.size()];
    if (depth != 0) {
        switch (next(state) % 5) {
        case 0: atom = "(?:" + regular(state, depth - 1) + ")"; break;
        case 1: atom = "(" + regular(state, depth - 1) + ")"; break;
        case 2: atom = "(?:" + regular(state, depth - 1) + "|" +
                       regular(state, depth - 1) + ")"; break;
        case 3: atom = regular(state, depth - 1) + regular(state, depth - 1); break;
        default: break;
        }
    }
    // Always group the result before adding a quantifier, so concatenations and
    // empty strings remain legal generated atoms.
    return "(?:" + atom + ")" + suffixes[next(state) % suffixes.size()];
}

std::vector<std::string> corpus() {
    std::vector<std::string> result = {
        "", "a", "ab", "a|b", "a||b|", "|a", "||", "()", "(?:)",
        "(a)", "(?<x>a)", "a*", "a+", "a?", "a{0}", "a{0,0}", "a{2,}",
        "(a?)*", "((a|b)*(c|d)*)*", ".", "[]a]", "[^]a]", "[-a]", "[a-]",
        "[a-cx-z]", R"([\x00-\xff])", R"(\d\D\w\W\s\S\n\r\t\f\v\a)",
        R"(\(\)\|\*\+\?\{\}\[\]\\\^\$)",
        R"(\k<x>(?<x>a|b))", R"((?<x>a|b)\k<x>)", R"(\1(a|b))", R"((a|b)\1)",
        R"((?<x>a)\k<x>|(?<y>b)\k<y>)", R"(\12(a)(b)(c)(d)(e)(f)(g)(h)(i)(j)(k)(l))",
        "(?<S>a(?&S)b|)", "(?<S>(?&S)|)", "(?<S>(?&S))", "(?R)|a",
        "(?<S>(?&T))(?<T>a(?&S)|)", "(?1)(a)", "(?(DEFINE))",
        "(?(DEFINE)(?<S>a(?&S)b|))(?&S)",
        "a{#n}b{=n}c{=n}", "a{#n:0,2}b{=n}", "(?:){#n:2}", "(?:){#n:0,2}",
        "a{#n:2,}b{=n}", "(?<S>a(?&S)|b){#n}",
        "(?<S>a(?&S)|b)(?&S){#n}", "(?<S>(?&S))a{#n}",
        "(?<S>(?&S))(?&S){#n}", "(?<S>(?&S)|)(?&S){#n}",
        "(?<S>(?&S))(?:a(?&S)){#n}", "(?<S>(?&S))(?:a(?&S)){#n:1,}",
        "(?<S>(?&S))a{#n}(?:b(?&S)){=n}",
        "(?<S>(?&S)|c)(?:a(?&S)){#n}",
        "(?:a{#cols};){#rows}b{=rows[1].cols}",
        "(?(DEFINE)(?<S>a(?&S)b|))(?&S)c{=S[0].@height}",
        "(?<G>a)b{=G[0].@bytes}", "(?<G>a)b{=$G[0].@depth}",
        "a{#n:0,2}b{=#n}", "a{#n:0,2}b{=#0}", "(?<G>a)b{=$1[0].@bytes}",
        "a{#n}b{=n.@bytes}", "(?<G>a)b{=G.@bytes}",
        "a{=./@bytes}", "a{=../@depth}", "a{=../../@height}", "a{=/@bytes}",
        "(?<n>a)a{#n:1,2}b{=#n}", "(?<n>a)a{#n:1,2}b{=$n[0].@bytes}",
        ")", "(", "(?", "(?:", "(?<", "(?<1>a)", "(?<x", "(?<x>a)(?<x>b)",
        "(?=a)", "(?!a)", "(?<=a)", "(?(x)a)", "(?(DEFINE)a)", "(?(DEFINE)(a))",
        "(?(DEFINE)(?<S>a)", "(?&missing)", "(?&S", "(?0)", "(?1)", "(?R",
        R"(\1)", R"((\1))", R"((?<x>\k<x>))", R"(\k<missing>a)",
        R"(\k)", R"(\k<x)", R"(\0)", R"([\1])", "\\", R"(\q)", R"(\x0)", R"(\xGG)",
        "[", "[]", "[^]", "[z-a]", R"([\d-a])", R"([a-\w])", "[[:alpha:]]",
        "[[.a.]]", "[[=a=]]", "^", "$", "*", "a**", "a*?", "a++", "a?+",
        "a{", "a{}", "a{,2}", "a{1", "a{2,1}", "a{1, 2}", "a{#n:3,2}",
        "a{=n}b{#n}", "(a?){#n}", "a{#n}b{#n}", "a{#n}(b?){=n}",
        "a{#n:1,2}b{=n[1]}", "(?<G>a)b{=G[0].@count}",
        "a{#n:1,2}b{=n.@height}", "a{#n:1,2}b{=#99}", "a{=$0.@bytes}",
        "a{=1}", "a{=missing[0].@bytes}", "a{=./@unknown}", "(?<G>a)b{=G}",
        "(?<n>a)a{#n:1,2}b{=n[0].@bytes}", "a{#n}b{=n[}", "a{#n}b{=n.}",
        R"(\k<missing>(?&absent))", R"((?&absent)\k<missing>)",
        "(?&absent)a{=missing[0].@bytes}",
        "(a?){#n}b{=missing[0].@bytes}",
    };
    const auto max = std::to_string(std::numeric_limits<std::size_t>::max());
    result.push_back("a{" + max + "}");
    result.push_back("(a?){" + max + "}");
    result.push_back("a{" + max + "0}");
    result.push_back("(?:aa){" + max + "}a");
    const std::string long_name = "long_identifier_" + std::string(128, 'x');
    result.push_back("\\k<" + long_name + ">(?<" + long_name + ">a)");
    result.push_back("a{#" + long_name + "}b{=" + long_name + "}");
    result.push_back("a{=" + long_name + "[0].@bytes}a{#" + long_name + ":0,2}");
    for (unsigned byte = 0; byte < 256; ++byte)
        result.push_back(std::string(1, static_cast<char>(byte)));
    short_strings(result, "", 3);
    std::uint32_t state = 20261012U;
    for (unsigned i = 0; i < 300; ++i) result.push_back(regular(state, 3));
    for (const auto& source : std::vector<std::string>{
             "(?<x>a|b)\\k<x>", "(?:a{#cols};){#rows}b{=rows[1].cols}",
             "(?(DEFINE)(?<S>a(?&S)b|))(?&S)c{=S[0].@height}",
             "(?<n>a)a{#n:1,2}b{=$n[0].@bytes}"})
        for (std::size_t n = 0; n < source.size(); ++n) result.push_back(source.substr(0, n));
    result.push_back(std::string(1024, 'a'));
    std::string alternatives, definitions, counts;
    for (unsigned i = 0; i < 128; ++i) {
        if (i != 0) alternatives += '|';
        alternatives += i % 3 == 0 ? "" : i % 3 == 1 ? "a?" : "bc";
        definitions += "(?<R" + std::to_string(i) + ">a|)";
        counts += "a{#N" + std::to_string(i) + ":0,2}";
    }
    result.push_back(alternatives);
    result.push_back("(?(DEFINE)" + definitions + ")(?&R127)");
    result.push_back(counts + "b{=#N127}");
    for (unsigned depth : {255U, 256U, 257U})
        result.push_back(std::string(depth, '(') + "a" + std::string(depth, ')'));
    return result;
}
}

int main(int argc, char** argv) {
    try {
        const bool emit_snapshot = argc == 2 && std::string_view(argv[1]) == "--snapshot";
        basic_contracts();
        const auto sources = corpus();
        std::size_t accepted = 0, rejected = 0;
        for (std::size_t id = 0; id < sources.size(); ++id) {
            auto source = sources[id]; // Each invocation owns a fresh input.
            if (emit_snapshot) std::cout << id << '\t' << hex(source) << '\t';
            try {
                auto ast = crank::parse(source);
                const auto first = snapshot(ast);
                require(first == snapshot(crank::parse(source)), "repeat parse changed AST");
                source.assign(source.size(), 'z');
                require(first == snapshot(ast), "AST borrows modified source storage");
                if (emit_snapshot) std::cout << "OK\t" << first << '\n';
                ++accepted;
            } catch (const std::invalid_argument& error) {
                if (emit_snapshot) std::cout << "ERR\t" << error.what() << '\n';
                ++rejected;
            }
        }
        std::cerr << "frontend_snapshot cases=" << sources.size()
                  << " accepted=" << accepted << " rejected=" << rejected << '\n';
    } catch (const std::exception& error) {
        std::cerr << "frontend_snapshot failed: " << error.what() << '\n';
        return 1;
    }
}
