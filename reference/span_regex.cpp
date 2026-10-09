#include "span_regex.hpp"

#include <algorithm>
#include <bitset>
#include <limits>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace crank::experimental {
namespace {

using NodeId = std::size_t;
using Endpoints = std::vector<std::size_t>;
constexpr auto unlimited = std::numeric_limits<std::size_t>::max();

enum class Kind { Empty, CharSet, Concat, Alt, Repeat };

struct Node {
    Kind kind = Kind::Empty;
    std::bitset<256> characters;
    std::vector<NodeId> children;
    std::size_t lower = 0;
    std::optional<std::size_t> upper;
    bool nullable = true;
    // A saturated lower bound; enough to reject impossible remaining spans.
    std::size_t minimum_length = 0;
};

std::size_t saturated_add(std::size_t a, std::size_t b) {
    return a > unlimited - b ? unlimited : a + b;
}

std::size_t saturated_multiply(std::size_t a, std::size_t b) {
    return b != 0 && a > unlimited / b ? unlimited : a * b;
}

struct Ast {
    std::vector<Node> nodes;
    NodeId root = 0;
};

struct ClassToken {
    std::bitset<256> characters;
    std::optional<unsigned char> single;
};

ClassToken one_byte(unsigned char byte) {
    ClassToken token;
    token.characters.set(byte);
    token.single = byte;
    return token;
}

class Parser {
public:
    explicit Parser(std::string_view pattern) : pattern_(pattern) {}

    Ast parse() {
        ast_.root = alternatives();
        if (position_ != pattern_.size()) {
            fail("unexpected closing parenthesis");
        }
        return std::move(ast_);
    }

private:
    static constexpr std::size_t nesting_limit = 256;
    std::string_view pattern_;
    std::size_t position_ = 0;
    std::size_t nesting_ = 0;
    Ast ast_;

    [[noreturn]] void fail(const std::string& message) const {
        throw std::invalid_argument("regex byte " + std::to_string(position_) +
                                    ": " + message);
    }

    bool at(char ch) const {
        return position_ < pattern_.size() && pattern_[position_] == ch;
    }

    bool take(char ch) {
        if (!at(ch)) return false;
        ++position_;
        return true;
    }

    NodeId append(Node node) {
        const auto id = ast_.nodes.size();
        ast_.nodes.push_back(std::move(node));
        return id;
    }

    NodeId empty() { return append(Node{}); }

    NodeId combine(Kind kind, std::vector<NodeId> children) {
        if (children.empty()) return empty();
        if (children.size() == 1) return children.front();
        Node node;
        node.kind = kind;
        node.children = std::move(children);
        node.nullable = kind == Kind::Concat;
        node.minimum_length = kind == Kind::Alt ? unlimited : 0;
        for (const auto child : node.children) {
            const auto& part = ast_.nodes[child];
            if (kind == Kind::Concat) {
                node.nullable = node.nullable && part.nullable;
                node.minimum_length = saturated_add(node.minimum_length,
                                                    part.minimum_length);
            } else {
                node.nullable = node.nullable || part.nullable;
                node.minimum_length = std::min(node.minimum_length,
                                              part.minimum_length);
            }
        }
        return append(std::move(node));
    }

    NodeId alternatives() {
        std::vector<NodeId> branches{concatenation()};
        while (take('|')) branches.push_back(concatenation());
        return combine(Kind::Alt, std::move(branches));
    }

    NodeId concatenation() {
        std::vector<NodeId> children;
        while (position_ < pattern_.size() && !at(')') && !at('|')) {
            children.push_back(quantified_atom());
        }
        return combine(Kind::Concat, std::move(children));
    }

    std::size_t count() {
        if (position_ == pattern_.size() || pattern_[position_] < '0' ||
            pattern_[position_] > '9') {
            fail("expected an unsigned repetition count");
        }
        std::size_t value = 0;
        while (position_ < pattern_.size() && pattern_[position_] >= '0' &&
               pattern_[position_] <= '9') {
            const auto digit = static_cast<std::size_t>(pattern_[position_] - '0');
            if (value > (unlimited - digit) / 10) fail("repetition count overflow");
            value = value * 10 + digit;
            ++position_;
        }
        return value;
    }

    NodeId quantified_atom() {
        const auto child = atom();
        Node node;
        node.kind = Kind::Repeat;
        node.children = {child};
        if (take('*')) {
            node.lower = 0;
        } else if (take('+')) {
            node.lower = 1;
        } else if (take('?')) {
            node.lower = 0;
            node.upper = 1;
        } else if (take('{')) {
            node.lower = count();
            if (take(',')) {
                if (!at('}')) node.upper = count();
            } else {
                node.upper = node.lower;
            }
            if (!take('}')) fail("expected closing brace after repetition count");
            if (node.upper && *node.upper < node.lower) {
                fail("repetition upper bound is smaller than its lower bound");
            }
        } else {
            return child;
        }
        if (at('*') || at('+') || at('?') || at('{')) {
            fail("stacked, lazy, and possessive quantifiers are unsupported");
        }
        const auto& body = ast_.nodes[child];
        node.nullable = node.lower == 0 || body.nullable;
        node.minimum_length = saturated_multiply(node.lower, body.minimum_length);
        return append(std::move(node));
    }

    static int hex_digit(unsigned char byte) {
        if (byte >= '0' && byte <= '9') return byte - '0';
        if (byte >= 'a' && byte <= 'f') return byte - 'a' + 10;
        if (byte >= 'A' && byte <= 'F') return byte - 'A' + 10;
        return -1;
    }

    ClassToken escaped() {
        if (position_ == pattern_.size()) fail("trailing escape");
        const auto code = static_cast<unsigned char>(pattern_[position_++]);
        switch (code) {
        case 'n': return one_byte('\n');
        case 'r': return one_byte('\r');
        case 't': return one_byte('\t');
        case 'f': return one_byte('\f');
        case 'v': return one_byte('\v');
        case 'a': return one_byte('\a');
        case 'x': {
            if (pattern_.size() - position_ < 2) fail("expected two hexadecimal digits");
            const auto hi = hex_digit(static_cast<unsigned char>(pattern_[position_]));
            const auto lo = hex_digit(static_cast<unsigned char>(pattern_[position_ + 1]));
            if (hi < 0 || lo < 0) fail("expected two hexadecimal digits");
            position_ += 2;
            return one_byte(static_cast<unsigned char>(16 * hi + lo));
        }
        case 'd': case 'D': case 'w': case 'W': case 's': case 'S': {
            ClassToken token;
            if (code == 'd' || code == 'D' || code == 'w' || code == 'W') {
                for (unsigned byte = '0'; byte <= '9'; ++byte) token.characters.set(byte);
            }
            if (code == 'w' || code == 'W') {
                for (unsigned byte = 'a'; byte <= 'z'; ++byte) token.characters.set(byte);
                for (unsigned byte = 'A'; byte <= 'Z'; ++byte) token.characters.set(byte);
                token.characters.set('_');
            }
            if (code == 's' || code == 'S') {
                for (const unsigned char byte : {' ', '\t', '\n', '\r', '\f', '\v'}) {
                    token.characters.set(byte);
                }
            }
            if (code == 'D' || code == 'W' || code == 'S') token.characters.flip();
            return token;
        }
        default:
            if (code >= '0' && code <= '9') {
                fail("backreferences and numeric escapes are unsupported; use \\xHH for a byte");
            }
            if ((code >= 'a' && code <= 'z') || (code >= 'A' && code <= 'Z')) {
                fail("unsupported escape or assertion");
            }
            return one_byte(code);
        }
    }

    ClassToken class_token() {
        if (position_ == pattern_.size()) fail("unterminated character class");
        if (take('\\')) return escaped();
        if (at('[') && position_ + 1 < pattern_.size()) {
            const auto marker = pattern_[position_ + 1];
            if (marker == ':' || marker == '.' || marker == '=') {
                fail("POSIX character classes, collating symbols, and equivalence classes are unsupported");
            }
        }
        return one_byte(static_cast<unsigned char>(pattern_[position_++]));
    }

    std::bitset<256> character_class() {
        const bool negate = take('^');
        bool first = true;
        std::bitset<256> characters;
        while (position_ < pattern_.size()) {
            // A closing bracket is a literal when it is the first class item.
            if (!first && take(']')) return negate ? ~characters : characters;
            const auto left = class_token();
            first = false;
            if (at('-') && position_ + 1 < pattern_.size() &&
                pattern_[position_ + 1] != ']') {
                ++position_;
                const auto right = class_token();
                if (!left.single || !right.single) fail("a range endpoint must be one byte");
                if (*left.single > *right.single) fail("descending character range");
                for (unsigned byte = *left.single; byte <= *right.single; ++byte) {
                    characters.set(byte);
                }
            } else {
                characters |= left.characters;
            }
        }
        fail("unterminated character class");
    }

    NodeId atom() {
        if (position_ == pattern_.size()) fail("expected an atom");
        const auto byte = static_cast<unsigned char>(pattern_[position_++]);
        if (byte == '(') {
            if (++nesting_ > nesting_limit) fail("group nesting exceeds prototype limit of 256");
            if (take('?') && !take(':')) fail("only ordinary and (?:...) groups are supported");
            const auto child = alternatives();
            if (!take(')')) fail("unclosed group");
            --nesting_;
            return child;
        }
        Node node;
        node.kind = Kind::CharSet;
        node.nullable = false;
        node.minimum_length = 1;
        if (byte == '[') {
            node.characters = character_class();
        } else if (byte == '\\') {
            node.characters = escaped().characters;
        } else if (byte == '.') {
            node.characters.set();
            node.characters.reset('\n');
        } else if (byte == '^' || byte == '$') {
            fail("anchors and assertions are unsupported");
        } else if (byte == '*' || byte == '+' || byte == '?' || byte == '{' || byte == '}') {
            fail("quantifier or closing brace without a preceding atom");
        } else {
            node.characters.set(byte);
        }
        return append(std::move(node));
    }
};

class Matcher {
public:
    Matcher(const Ast& ast, std::string_view subject)
        : ast_(ast), subject_(subject), memo_(ast.nodes.size()) {}

    const Endpoints& evaluate(NodeId id, std::size_t start) {
        auto& entries = memo_[id];
        if (const auto found = entries.find(start); found != entries.end()) {
            ++statistics.memo_hits;
            return found->second;
        }
        ++statistics.node_evaluations;
        const auto& node = ast_.nodes[id];
        Endpoints result;
        if (node.minimum_length <= subject_.size() - start) {
            switch (node.kind) {
            case Kind::Empty:
                result.push_back(start);
                break;
            case Kind::CharSet:
                if (start < subject_.size() &&
                    node.characters.test(static_cast<unsigned char>(subject_[start]))) {
                    result.push_back(start + 1);
                }
                break;
            case Kind::Concat:
                result.push_back(start);
                for (const auto child : node.children) {
                    result = advance(child, result);
                    if (result.empty()) break;
                }
                break;
            case Kind::Alt: {
                std::vector<bool> seen(subject_.size() + 1);
                for (const auto child : node.children) {
                    for (const auto end : evaluate(child, start)) seen[end] = true;
                }
                result = collect(seen, start);
                break;
            }
            case Kind::Repeat:
                result = repeat(node, start);
                break;
            }
        }
        statistics.endpoint_insertions += result.size();
        return entries.emplace(start, std::move(result)).first->second;
    }

    Statistics statistics;

private:
    const Ast& ast_;
    std::string_view subject_;
    std::vector<std::unordered_map<std::size_t, Endpoints>> memo_;

    static Endpoints collect(const std::vector<bool>& seen, std::size_t start) {
        Endpoints result;
        for (auto end = start; end < seen.size(); ++end) {
            if (seen[end]) result.push_back(end);
        }
        return result;
    }

    Endpoints advance(NodeId child, const Endpoints& starts) {
        if (starts.empty()) return {};
        std::vector<bool> seen(subject_.size() + 1);
        for (const auto start : starts) {
            for (const auto end : evaluate(child, start)) seen[end] = true;
        }
        return collect(seen, starts.front());
    }

    Endpoints closure(NodeId child, const Endpoints& seeds, std::size_t cap) {
        if (seeds.empty() || cap == 0) return seeds;
        // Multi-source breadth-first search records the smallest additional
        // iteration count needed for each endpoint. We accept every count from
        // zero through cap, so longer paths to an already reached endpoint
        // cannot improve any continuation.
        std::vector<std::size_t> distance(subject_.size() + 1, unlimited);
        Endpoints queue = seeds;
        for (const auto seed : seeds) distance[seed] = 0;
        std::size_t previous_round = unlimited;
        for (std::size_t head = 0; head < queue.size(); ++head) {
            const auto start = queue[head];
            const auto round = distance[start];
            if (round == cap) continue;
            if (round != previous_round) {
                ++statistics.repetition_rounds;
                previous_round = round;
            }
            for (const auto end : evaluate(child, start)) {
                if (distance[end] == unlimited) {
                    distance[end] = round + 1;
                    queue.push_back(end);
                }
            }
        }
        std::sort(queue.begin(), queue.end());
        return queue;
    }

    Endpoints repeat(const Node& node, std::size_t start) {
        const auto child = node.children.front();
        const auto& body = ast_.nodes[child];
        const auto remaining = subject_.size() - start;
        Endpoints frontier{start};

        if (body.nullable) {
            // An epsilon iteration can pad every path to meet the lower bound.
            // Each nonempty iteration consumes >= 1 byte, so at most remaining
            // such iterations matter, even if either written bound is enormous.
            const auto cap = std::min(node.upper.value_or(unlimited), remaining);
            return closure(child, frontier, cap);
        }

        const auto cap = std::min(node.upper.value_or(unlimited),
                                  remaining / body.minimum_length);
        if (node.lower > cap) return {};
        // First require exactly lower iterations; then any additional count up
        // to cap - lower is allowed, making shortest-path closure sufficient.
        for (std::size_t round = 0; round < node.lower; ++round) {
            ++statistics.repetition_rounds;
            frontier = advance(child, frontier);
            if (frontier.empty()) return {};
        }
        return closure(child, frontier, cap - node.lower);
    }
};

} // namespace

struct Program::Impl {
    explicit Impl(std::string_view pattern) : ast(Parser(pattern).parse()) {}
    const Ast ast;
};

Program::Program(std::string pattern) : impl_(std::make_shared<const Impl>(pattern)) {}

std::vector<std::size_t> Program::endpoints(std::string_view subject,
                                          std::size_t start,
                                          Statistics* statistics) const {
    if (statistics) *statistics = {};
    if (start > subject.size()) throw std::out_of_range("match start exceeds subject size");
    Matcher matcher(impl_->ast, subject);
    auto result = matcher.evaluate(impl_->ast.root, start);
    if (statistics) *statistics = matcher.statistics;
    return result;
}

bool Program::full_match(std::string_view subject, Statistics* statistics) const {
    const auto result = endpoints(subject, 0, statistics);
    return std::binary_search(result.begin(), result.end(), subject.size());
}

} // namespace crank::experimental
