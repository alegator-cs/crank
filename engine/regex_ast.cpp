#include "regex_ast.hpp"
#include "occurrence_analysis.hpp"

#include <algorithm>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace crank {
namespace {
constexpr auto unlimited = std::numeric_limits<std::size_t>::max();

std::size_t saturated_add(std::size_t a, std::size_t b) {
    return a > unlimited - b ? unlimited : a + b;
}

std::size_t saturated_multiply(std::size_t a, std::size_t b) {
    return b != 0 && a > unlimited / b ? unlimited : a * b;
}

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
        ast_.capture_nodes[0] = ast_.root;
        resolve_references();
        resolve_calls();
        resolve_count_paths();
        validate_count_bodies();
        return std::move(ast_);
    }

private:
    static constexpr std::size_t nesting_limit = 256;
    std::string_view pattern_;
    std::size_t position_ = 0;
    std::size_t nesting_ = 0;
    Ast ast_;
    std::unordered_map<std::string, std::size_t> captures_by_name_;
    std::unordered_map<std::string, std::size_t> counts_by_name_;
    std::vector<bool> capture_open_{false};
    struct UnresolvedReference {
        NodeId node;
        std::string name; // Empty for a numeric target.
        std::size_t source_position;
    };
    std::vector<UnresolvedReference> references_;
    struct UnresolvedCall {
        NodeId node;
        std::string name; // Empty for a numeric target; zero denotes (?R).
        std::size_t capture;
        std::size_t source_position;
    };
    std::vector<UnresolvedCall> calls_;
    struct UnresolvedPathStep {
        std::string name; // Empty for a numeric source ID.
        std::optional<OccurrenceSource> source;
        std::size_t numeric_id = no_id;
        std::size_t source_position = 0;
    };
    struct UnresolvedPath {
        std::size_t path;
        std::vector<UnresolvedPathStep> steps;
        bool has_metric = false;
        std::size_t source_position = 0;
    };
    std::vector<UnresolvedPath> paths_;
    struct CountBodyCheck {
        NodeId body;
        std::size_t source_position;
    };
    std::vector<CountBodyCheck> count_body_checks_;

    static bool identifier_start(unsigned char byte) {
        return (byte >= 'a' && byte <= 'z') ||
               (byte >= 'A' && byte <= 'Z') || byte == '_';
    }

    static bool identifier_continue(unsigned char byte) {
        return identifier_start(byte) || (byte >= '0' && byte <= '9');
    }

    std::string identifier() {
        if (position_ == pattern_.size() ||
            !identifier_start(static_cast<unsigned char>(pattern_[position_]))) {
            fail("expected an identifier starting with an ASCII letter or underscore");
        }
        const auto begin = position_++;
        while (position_ < pattern_.size() &&
               identifier_continue(static_cast<unsigned char>(pattern_[position_]))) {
            ++position_;
        }
        return std::string(pattern_.substr(begin, position_ - begin));
    }

    void resolve_references() {
        for (const auto& reference : references_) {
            auto& node = ast_.nodes[reference.node];
            if (!reference.name.empty()) {
                const auto found = captures_by_name_.find(reference.name);
                if (found == captures_by_name_.end()) {
                    position_ = reference.source_position;
                    fail("unknown capture name '" + reference.name + "'");
                }
                node.capture_id = found->second;
            }
            if (node.capture_id == 0 || node.capture_id >= ast_.capture_names.size()) {
                position_ = reference.source_position;
                fail("reference to an unknown capture group");
            }
        }
    }

    NodeId reference(std::size_t target, std::string name = {}) {
        Node node;
        node.kind = Kind::Reference;
        node.stateful = true;
        node.capture_id = target;
        node.forward = target == no_id || target >= ast_.capture_names.size();
        if (!node.forward && capture_open_[target]) {
            fail("reference to a syntactically open capture is unsupported");
        }
        // References may consume zero bytes; no unsound length pruning.
        node.nullable = true;
        node.minimum_length = 0;
        ast_.has_references = true;
        const auto id = append(std::move(node));
        references_.push_back({id, std::move(name), position_});
        return id;
    }

    void resolve_calls() {
        for (const auto& call : calls_) {
            auto capture = call.capture;
            if (!call.name.empty()) {
                const auto found = captures_by_name_.find(call.name);
                if (found == captures_by_name_.end()) {
                    position_ = call.source_position;
                    fail("call to an unknown capture name '" + call.name + "'");
                }
                capture = found->second;
            }
            if (capture >= ast_.capture_nodes.size() ||
                ast_.capture_nodes[capture] == no_id) {
                position_ = call.source_position;
                fail("call to an unknown capture group");
            }
            ast_.nodes[call.node].capture_id = capture;
            ast_.nodes[call.node].call_target = ast_.capture_nodes[capture];
        }
    }

    NodeId call(std::size_t capture, std::string name = {}) {
        Node node;
        node.kind = Kind::Call;
        // A recursive target has no safe positive lower bound at parse time.
        // Store its link separately so structural AST traversal stays acyclic.
        node.nullable = true;
        node.minimum_length = 0;
        node.stateful = true;
        ast_.has_calls = true;
        const auto id = append(std::move(node));
        calls_.push_back({id, std::move(name), capture, position_});
        return id;
    }

    NodeId definitions() {
        // The prefix "(?(DEFINE)" has already been consumed. Rules are
        // structural children for discovery, but the block itself is epsilon.
        Node node;
        node.kind = Kind::Define;
        node.stateful = true;
        ast_.has_calls = true;
        while (!at(')')) {
            if (position_ == pattern_.size()) fail("unclosed DEFINE block");
            if (pattern_.substr(position_, 3) != "(?<") {
                fail("DEFINE blocks may contain only named capture definitions");
            }
            node.children.push_back(atom());
        }
        ++position_;
        --nesting_;
        return append(std::move(node));
    }

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
            node.stateful = node.stateful || part.stateful;
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

    void repetition_bounds(Node& node) {
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
    }

    OccurrenceMetric occurrence_metric() {
        const auto name = identifier();
        if (name == "count") return OccurrenceMetric::Count;
        if (name == "height") return OccurrenceMetric::Height;
        if (name == "depth") return OccurrenceMetric::Depth;
        if (name == "bytes") return OccurrenceMetric::Bytes;
        fail("unknown occurrence metric '@" + name + "'");
    }

    std::size_t occurrence_path() {
        CountPath path;
        UnresolvedPath unresolved;
        unresolved.path = ast_.count_paths.size();
        unresolved.source_position = position_;
        bool anchored = false;
        if (pattern_.substr(position_, 3) == "../") {
            anchored = true;
            path.anchor = OccurrenceAnchor::Ancestor;
            do {
                ++path.parents;
                position_ += 3;
            } while (pattern_.substr(position_, 3) == "../");
        } else if (pattern_.substr(position_, 2) == "./") {
            anchored = true;
            path.anchor = OccurrenceAnchor::Current;
            position_ += 2;
        } else if (take('/')) {
            anchored = true;
        }

        if (anchored && take('@')) {
            path.metric = occurrence_metric();
            unresolved.has_metric = true;
        } else {
            while (true) {
                UnresolvedPathStep label;
                label.source_position = position_;
                if (take('#')) label.source = OccurrenceSource::Count;
                else if (take('$')) label.source = OccurrenceSource::Group;
                if (position_ < pattern_.size() && pattern_[position_] >= '0' &&
                    pattern_[position_] <= '9') {
                    if (!label.source) fail("numeric occurrence sources require '#' or '$'");
                    label.numeric_id = count();
                } else {
                    label.name = identifier();
                }
                OccurrenceStep step;
                if (take('[')) {
                    step.index = count();
                    if (!take(']')) fail("expected ']' after occurrence index");
                }
                path.steps.push_back(std::move(step));
                unresolved.steps.push_back(std::move(label));
                if (!take('.')) break;
                if (take('@')) {
                    path.metric = occurrence_metric();
                    unresolved.has_metric = true;
                    break;
                }
            }
        }
        if (!take('}')) fail("expected closing brace after occurrence path");
        ast_.count_paths.push_back(std::move(path));
        paths_.push_back(std::move(unresolved));
        ast_.has_occurrence_paths = true;
        ast_.has_extended = true;
        return ast_.count_paths.size() - 1;
    }

    void resolve_count_paths() {
        for (const auto& unresolved : paths_) {
            auto& path = ast_.count_paths[unresolved.path];
            for (std::size_t i = 0; i < unresolved.steps.size(); ++i) {
                const auto& label = unresolved.steps[i];
                auto& step = path.steps[i];
                position_ = label.source_position;
                if (label.name.empty()) {
                    step.source = *label.source;
                    step.id = label.numeric_id;
                    if (step.source == OccurrenceSource::Count) {
                        if (step.id >= ast_.count_names.size()) fail("unknown numeric count source");
                    } else if (step.id == 0 || step.id >= ast_.capture_names.size()) {
                        fail("unknown numeric group source; use '/' for the root scope");
                    }
                    continue;
                }
                const auto count_source = counts_by_name_.find(label.name);
                const auto group_source = captures_by_name_.find(label.name);
                const bool count_found = count_source != counts_by_name_.end() &&
                    (!label.source || *label.source == OccurrenceSource::Count);
                const bool group_found = group_source != captures_by_name_.end() &&
                    (!label.source || *label.source == OccurrenceSource::Group);
                if (count_found && group_found) {
                    fail("ambiguous occurrence source '" + label.name +
                         "'; use '#' for a count or '$' for a group");
                }
                if (!count_found && !group_found) {
                    fail("unknown occurrence source '" + label.name + "'");
                }
                step.source = count_found ? OccurrenceSource::Count : OccurrenceSource::Group;
                step.id = count_found ? count_source->second : group_source->second;
            }
            position_ = unresolved.source_position;
            if (path.steps.empty()) continue; // Scope record type is checked at runtime.
            const auto& terminal = path.steps.back();
            if (terminal.source == OccurrenceSource::Group && !unresolved.has_metric) {
                fail("a group occurrence path requires '@bytes', '@depth', or '@height'");
            }
            if (path.metric == OccurrenceMetric::Count &&
                (terminal.source != OccurrenceSource::Count || terminal.index)) {
                fail("'@count' requires an unindexed count occurrence");
            }
            if (terminal.source == OccurrenceSource::Count &&
                (path.metric == OccurrenceMetric::Height ||
                 path.metric == OccurrenceMetric::Depth)) {
                fail("'@height' and '@depth' require a group or scope occurrence");
            }
        }
        position_ = pattern_.size();
    }

    void validate_count_bodies() {
        const bool needs_analysis = ast_.has_calls && !count_body_checks_.empty();
        for (const auto& check : count_body_checks_) {
            if (ast_.nodes[check.body].minimum_length != 0) continue;
            if (!needs_analysis) {
                position_ = check.source_position;
                fail("a named count body must provably consume at least one byte");
            }
        }
        if (!needs_analysis) return;
        const OccurrenceAnalysis analysis(ast_);
        for (const auto& check : count_body_checks_) {
            const auto& length = analysis.length(check.body);
            if (!length.productive || length.minimum == 0) {
                position_ = check.source_position;
                fail("a named count body must provably consume at least one byte");
            }
        }
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
            if (at('#') || at('=')) {
                const bool bind = take('#');
                if (!bind) ++position_; // '='
                if (bind) {
                    const auto name = identifier();
                    if (take(':')) {
                        repetition_bounds(node);
                        ast_.has_extended = true;
                    } else if (!take('}')) {
                        fail("expected closing brace or ':' after count name");
                    }
                    if (counts_by_name_.contains(name)) {
                        fail("duplicate repetition count name '" + name + "'");
                    }
                    node.bind_count = ast_.count_names.size();
                    ast_.count_names.push_back(name);
                    ast_.count_nodes.push_back(no_id);
                    counts_by_name_.emplace(name, node.bind_count);
                    if (!node.upper) count_body_checks_.push_back({child, position_});
                } else {
                    const auto begin = position_;
                    std::string name;
                    if (position_ < pattern_.size() &&
                        identifier_start(static_cast<unsigned char>(pattern_[position_]))) {
                        name = identifier();
                    }
                    if (!name.empty() && take('}')) {
                        const auto found = counts_by_name_.find(name);
                        if (found == counts_by_name_.end()) {
                            fail("count reference requires a previously declared binder: '" + name + "'");
                        }
                        node.use_count = found->second;
                        count_body_checks_.push_back({child, position_});
                    } else {
                        position_ = begin;
                        node.count_path = occurrence_path();
                    }
                }
                ast_.has_counts = true;
            } else {
                repetition_bounds(node);
            }
        } else {
            return child;
        }
        if (at('*') || at('+') || at('?') || at('{')) {
            fail("stacked, lazy, and possessive quantifiers are unsupported");
        }
        const auto& body = ast_.nodes[child];
        node.stateful = body.stateful || node.bind_count != no_id ||
                        node.use_count != no_id || node.count_path != no_id;
        node.nullable = node.lower == 0 || body.nullable;
        node.minimum_length = saturated_multiply(node.lower, body.minimum_length);
        const auto binder = node.bind_count;
        const auto id = append(std::move(node));
        if (binder != no_id) ast_.count_nodes[binder] = id;
        return id;
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
                fail("numeric escapes are unsupported here; use \\xHH for a byte");
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
            bool capturing = true;
            std::string name;
            if (take('?')) {
                if (take(':')) {
                    capturing = false;
                } else if (take('<')) {
                    name = identifier();
                    if (!take('>')) fail("expected '>' after capture name");
                } else if (take('&')) {
                    const auto target = identifier();
                    if (!take(')')) fail("expected ')' after rule call name");
                    --nesting_;
                    return call(no_id, target);
                } else if (take('R')) {
                    if (!take(')')) fail("expected ')' after (?R whole-pattern call");
                    --nesting_;
                    return call(0);
                } else if (position_ < pattern_.size() && pattern_[position_] >= '0' &&
                           pattern_[position_] <= '9') {
                    const auto target = count();
                    if (target == 0) fail("numeric rule calls must be positive; use (?R) for the whole pattern");
                    if (!take(')')) fail("expected ')' after numeric rule call");
                    --nesting_;
                    return call(target);
                } else if (take('(')) {
                    if (pattern_.substr(position_, 7) != "DEFINE)") {
                        fail("only (?(DEFINE)...) conditional blocks are supported");
                    }
                    position_ += 7;
                    return definitions();
                } else {
                    fail("unsupported group; expected a capture, non-capturing group, rule call, or DEFINE block");
                }
            }
            std::size_t capture = no_id;
            if (capturing) {
                capture = ast_.capture_names.size();
                if (!name.empty() && captures_by_name_.contains(name)) {
                    fail("duplicate capture name '" + name + "'");
                }
                ast_.capture_names.push_back(name);
                ast_.capture_nodes.push_back(no_id);
                capture_open_.push_back(true);
                if (!name.empty()) captures_by_name_.emplace(name, capture);
            }
            const auto child = alternatives();
            if (!take(')')) fail("unclosed group");
            --nesting_;
            if (!capturing) return child;
            capture_open_[capture] = false;
            Node node;
            node.kind = Kind::Capture;
            node.children = {child};
            node.capture_id = capture;
            node.nullable = ast_.nodes[child].nullable;
            node.minimum_length = ast_.nodes[child].minimum_length;
            node.stateful = true;
            const auto id = append(std::move(node));
            ast_.capture_nodes[capture] = id;
            return id;
        }
        Node node;
        node.kind = Kind::CharSet;
        node.nullable = false;
        node.minimum_length = 1;
        if (byte == '[') {
            node.characters = character_class();
        } else if (byte == '\\') {
            if (position_ < pattern_.size() && pattern_[position_] >= '0' &&
                pattern_[position_] <= '9') {
                if (at('0')) fail("group zero and numeric byte escapes are unsupported; use \\xHH for a byte");
                return reference(count());
            }
            if (take('k')) {
                if (!take('<')) fail("expected '<' after \\k");
                const auto name = identifier();
                if (!take('>')) fail("expected '>' after reference name");
                const auto found = captures_by_name_.find(name);
                return reference(found == captures_by_name_.end() ? no_id : found->second, name);
            }
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

} // namespace

Ast parse(std::string_view pattern) {
    return Parser(pattern).parse();
}

} // namespace crank
