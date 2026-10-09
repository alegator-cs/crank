#include "affine_matcher.hpp"
#include "fragment_matcher.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <utility>


#if defined(__SIZEOF_INT128__)
namespace crank {
namespace {
using Integer = std::int64_t;
// GCC and Clang supply exact wide intermediates without a runtime dependency.
__extension__ using Wide = __int128;
constexpr Integer coefficient_limit = Integer{1} << 40;
constexpr std::size_t variable_limit = 8;
static_assert(sizeof(std::size_t) <= 8);
struct Decline {};
Integer bounded(Wide value) {
    if (value < -coefficient_limit || value > coefficient_limit) throw Decline{};
    return static_cast<Integer>(value);
}
Integer number(std::size_t value) {
    if (Wide{value} > coefficient_limit) throw Decline{};
    return static_cast<Integer>(value);
}
struct Rational {
    Integer numerator = 0, denominator = 1;
    Rational() = default;
    Rational(Integer value) : numerator(value) {}
    Rational(Wide top, Wide bottom) {
        if (bottom == 0) throw Decline{};
        if (bottom < 0) { top = -top; bottom = -bottom; }
        // Products involve bounded operands, so the Euclidean computation fits.
        Wide a = top < 0 ? -top : top, b = bottom;
        while (b) { const Wide next = a % b; a = b; b = next; }
        numerator = bounded(top / a);
        denominator = bounded(bottom / a);
    }
};
Rational operator-(Rational a, Rational b) {
    return {Wide{a.numerator} * b.denominator - Wide{b.numerator} * a.denominator,
            Wide{a.denominator} * b.denominator};
}
Rational operator*(Rational a, Rational b) {
    return {Wide{a.numerator} * b.numerator, Wide{a.denominator} * b.denominator};
}
Rational operator/(Rational a, Rational b) {
    return {Wide{a.numerator} * b.denominator, Wide{a.denominator} * b.numerator};
}
using Mask = std::bitset<256>;
bool valid_ast(const Ast& ast) {
    if (ast.root >= ast.nodes.size()) return false;
    std::vector<unsigned char> parents(ast.nodes.size());
    for (const auto& n : ast.nodes) {
        if (n.count_path != no_id) return false;
        for (NodeId child : n.children)
            if (child >= ast.nodes.size() || ++parents[child] > 1) return false;
        if ((n.bind_count != no_id || n.use_count != no_id) && n.kind != Kind::Repeat) return false;
        if (n.bind_count != no_id && n.use_count != no_id) return false;
        if ((n.bind_count != no_id || n.use_count != no_id) && (n.lower != 0 || n.upper)) return false;
        if (n.bind_count != no_id && n.bind_count >= ast.count_names.size()) return false;
        if (n.use_count != no_id && n.use_count >= ast.count_names.size()) return false;
        if (n.forward && n.kind != Kind::Reference) return false;
        switch (n.kind) {
        case Kind::Empty:
        case Kind::CharSet:
            if (!n.children.empty() || n.capture_id != no_id) return false;
            break;
        case Kind::Reference:
            if (!n.children.empty() || n.capture_id == 0 || n.capture_id >= ast.capture_names.size()) return false;
            break;
        case Kind::Capture:
            if (n.children.size() != 1 || n.capture_id == 0 || n.capture_id >= ast.capture_names.size()) return false;
            break;
        case Kind::Repeat:
            if (n.children.size() != 1 || n.capture_id != no_id || (n.upper && *n.upper < n.lower)) return false;
            break;
        case Kind::Concat:
        case Kind::Alt:
            if (n.capture_id != no_id) return false;
            break;
        default: return false;
        }
    }
    // Also reject unreachable cycles: Extractor inventories all capture nodes,
    // and callers of this public API need not have used the source parser.
    std::vector<unsigned char> color(ast.nodes.size());
    std::vector<std::size_t> heights(ast.nodes.size());
    std::vector<std::pair<NodeId, std::size_t>> stack;
    for (NodeId root = 0; root < ast.nodes.size(); ++root) {
        if (color[root]) continue;
        color[root] = 1;
        stack.emplace_back(root, 0);
        while (!stack.empty()) {
            auto& [id, child_index] = stack.back();
            if (child_index == ast.nodes[id].children.size()) {
                std::size_t height = 1;
                for (NodeId child : ast.nodes[id].children) height = std::max(height, heights[child] + 1);
                if (height > 1024) return false;
                heights[id] = height;
                color[id] = 2; stack.pop_back(); continue;
            }
            const auto child = ast.nodes[id].children[child_index++];
            if (color[child] == 1) return false;
            if (color[child] == 2) continue;
            if (stack.size() >= 1024) return false;
            color[child] = 1;
            stack.emplace_back(child, 0);
        }
    }
    return true;
}
bool regular(const Ast& ast, NodeId id) {
    const auto& n = ast.nodes[id];
    if (n.kind == Kind::Reference || n.kind == Kind::Call || n.kind == Kind::Define ||
        n.bind_count != no_id || n.use_count != no_id || n.count_path != no_id) return false;
    return std::all_of(n.children.begin(), n.children.end(), [&](NodeId c) { return regular(ast, c); });
}
Mask alphabet(const Ast& ast, NodeId id) {
    const auto& n = ast.nodes[id];
    if (n.kind == Kind::CharSet) return n.characters;
    if (n.kind == Kind::Repeat && n.upper && *n.upper == 0) return {};
    Mask result;
    for (NodeId c : n.children) result |= alphabet(ast, c);
    return result;
}
// An exact invariant: every word accepted by the regular node has this many
// bytes in the mask. Failure means unknown, never a guessed coefficient.
std::optional<Integer> invariant(const Ast& ast, NodeId id, const Mask& mask) {
    const auto& n = ast.nodes[id];
    if (n.bind_count != no_id || n.use_count != no_id || n.count_path != no_id) return {};
    switch (n.kind) {
    case Kind::Empty: return 0;
    case Kind::Reference:
    case Kind::Call:
    case Kind::Define: return {};
    case Kind::CharSet:
        if ((n.characters & ~mask).none()) return 1;
        if ((n.characters & mask).none()) return 0;
        return {};
    case Kind::Capture: return invariant(ast, n.children.front(), mask);
    case Kind::Concat: {
        Integer result = 0;
        for (NodeId c : n.children) {
            const auto part = invariant(ast, c, mask);
            if (!part) return {};
            result = bounded(Wide{result} + *part);
        }
        return result;
    }
    case Kind::Alt: {
        if (n.children.empty()) return 0;
        const auto result = invariant(ast, n.children.front(), mask);
        if (!result) return {};
        for (NodeId c : n.children)
            if (invariant(ast, c, mask) != result) return {};
        return result;
    }
    case Kind::Repeat: {
        if (n.upper && *n.upper == 0) return 0;
        const auto body = invariant(ast, n.children.front(), mask);
        if (!body) return {};
        if (*body == 0) return 0;
        if (!n.upper || n.lower != *n.upper) return {};
        return bounded(Wide{*body} * number(n.lower));
    }
    }
    return {};
}
Mask all_bytes() { Mask result; result.set(); return result; }
// Interval hull of a regular fragment's lengths. The hull is necessary only;
// holes and alternative correlations remain the regular validator's job.
std::pair<Integer, std::optional<Integer>> length_bounds(const Ast& ast, NodeId id) {
    const auto& node = ast.nodes[id];
    switch (node.kind) {
    case Kind::Empty: return {0, 0};
    case Kind::CharSet: return {1, 1};
    case Kind::Capture: return length_bounds(ast, node.children.front());
    case Kind::Concat: {
        Integer low = 0;
        std::optional<Integer> high = 0;
        for (NodeId child : node.children) {
            const auto [child_low, child_high] = length_bounds(ast, child);
            low = bounded(Wide{low} + child_low);
            if (high && child_high) high = bounded(Wide{*high} + *child_high);
            else high.reset();
        }
        return {low, high};
    }
    case Kind::Alt: {
        if (node.children.empty()) return {0, 0};
        auto [low, high] = length_bounds(ast, node.children.front());
        for (NodeId child : node.children) {
            const auto [child_low, child_high] = length_bounds(ast, child);
            low = std::min(low, child_low);
            if (high && child_high) high = std::max(*high, *child_high);
            else high.reset();
        }
        return {low, high};
    }
    case Kind::Repeat: {
        const auto [low, high] = length_bounds(ast, node.children.front());
        const auto minimum = bounded(Wide{low} * number(node.lower));
        if ((node.upper && *node.upper == 0) || (high && *high == 0)) return {minimum, 0};
        if (node.upper && high) return {minimum, bounded(Wide{*high} * number(*node.upper))};
        return {minimum, {}};
    }
    case Kind::Reference:
    case Kind::Call:
    case Kind::Define: throw Decline{};
    }
    throw Decline{};
}

// A small number of exact range observations is cheaper than a general
// histogram. Fixed-size integer reductions permit vectorization at -O2 while
// retaining portable unsigned-byte comparisons and a scalar tail.
std::size_t count_range(std::string_view subject, unsigned low, unsigned span) {
    constexpr std::size_t block = 32;
    std::size_t position = 0, result = 0;
    while (subject.size() - position >= block) {
        unsigned count = 0;
        for (std::size_t j = 0; j < block; ++j)
            count += unsigned(static_cast<unsigned char>(subject[position + j])) - low <= span;
        result += count;
        position += block;
    }
    for (; position < subject.size(); ++position)
        result += unsigned(static_cast<unsigned char>(subject[position])) - low <= span;
    return result;
}
void saturated_accumulate(std::size_t& target, std::size_t amount) {
    target += std::min(amount, std::numeric_limits<std::size_t>::max() - target);
}
enum class SegmentKind { Fixed, Span, Count, Reference };
struct SegmentSpec {
    SegmentKind kind;
    NodeId body = no_id;
    std::size_t variable = no_id;
    Integer width = 0;
};
struct Variable {
    std::string name;
    NodeId body = no_id;
    bool count = false;
    Mask alphabet;
    std::optional<Integer> fixed_width;
    Integer minimum = 0;
    std::optional<Integer> maximum;
};
struct Row {
    Mask mask;
    std::vector<Integer> coefficients;
    Integer offset = 0;
    bool constant = false;
    Integer value = 0;
    std::optional<unsigned char> delimiter;
    std::size_t ordinal = 0;
};
struct Extractor {
    const Ast& ast;
    std::vector<bool> targets, seen_captures, seen_counts;
    std::vector<std::size_t> capture_variables, count_variables;
    std::vector<std::size_t> reference_counts, flat_reference_counts, capture_nodes;
    std::vector<Variable> variables;
    std::vector<SegmentSpec> segments;

    explicit Extractor(const Ast& ast) : ast(ast), targets(ast.capture_names.size()),
        seen_captures(ast.capture_names.size()), seen_counts(ast.count_names.size()),
        capture_variables(ast.capture_names.size(), no_id), count_variables(ast.count_names.size(), no_id),
        reference_counts(ast.capture_names.size()), flat_reference_counts(ast.capture_names.size()),
        capture_nodes(ast.capture_names.size()) {
        for (const auto& n : ast.nodes) {
            if (n.kind == Kind::Reference) {
                if (n.capture_id == 0 || n.capture_id >= targets.size()) throw Decline{};
                targets[n.capture_id] = true;
                ++reference_counts[n.capture_id];
            }
            if (n.kind == Kind::Capture) {
                if (n.capture_id == 0 || n.capture_id >= capture_nodes.size()) throw Decline{};
                ++capture_nodes[n.capture_id];
            }
        }
    }
    std::size_t variable(std::string name, NodeId body, bool count) {
        if (variables.size() == variable_limit) throw Decline{};
        variables.push_back({std::move(name), body, count, {}, {}, 0, {}});
        return variables.size() - 1;
    }
    bool contains_target(NodeId id) const {
        const auto& n = ast.nodes[id];
        if (n.kind == Kind::Capture && targets[n.capture_id]) return true;
        return std::any_of(n.children.begin(), n.children.end(), [&](NodeId c) { return contains_target(c); });
    }
    std::size_t capture_variable(std::size_t slot) {
        if (capture_variables[slot] == no_id) {
            const auto& name = ast.capture_names[slot];
            capture_variables[slot] = variable(name.empty() ? "capture " + std::to_string(slot) : "capture " + name,
                                               no_id, false);
        }
        return capture_variables[slot];
    }
    void visit(NodeId id) {
        const auto& n = ast.nodes[id];
        if (n.kind == Kind::Concat) {
            for (NodeId c : n.children) visit(c);
            return;
        }
        if (n.kind == Kind::Capture) {
            if (!targets[n.capture_id]) { visit(n.children.front()); return; }
            if (seen_captures[n.capture_id] || capture_nodes[n.capture_id] != 1 ||
                !regular(ast, n.children.front()) || contains_target(n.children.front())) throw Decline{};
            seen_captures[n.capture_id] = true;
            const auto slot = capture_variable(n.capture_id);
            variables[slot].body = n.children.front();
            segments.push_back({SegmentKind::Span, n.children.front(), slot, 0});
            return;
        }
        if (n.kind == Kind::Reference) {
            if (n.forward == seen_captures[n.capture_id]) throw Decline{};
            ++flat_reference_counts[n.capture_id];
            segments.push_back({SegmentKind::Reference, no_id, capture_variable(n.capture_id), 0});
            return;
        }
        if (n.kind == Kind::Repeat && (n.bind_count != no_id || n.use_count != no_id)) {
            const bool binding = n.bind_count != no_id;
            const auto count = binding ? n.bind_count : n.use_count;
            if (count >= seen_counts.size() || (binding ? seen_counts[count] : !seen_counts[count]) ||
                !regular(ast, n.children.front()) || contains_target(n.children.front())) throw Decline{};
            const auto width = invariant(ast, n.children.front(), all_bytes());
            if (!width || *width <= 0) throw Decline{};
            if (binding) {
                seen_counts[count] = true;
                count_variables[count] = variable("count " + ast.count_names[count], n.children.front(), true);
            }
            segments.push_back({SegmentKind::Count, n.children.front(), count_variables[count], *width});
            return;
        }
        if (!regular(ast, id) || contains_target(id)) throw Decline{};
        const auto width = invariant(ast, id, all_bytes());
        if (width) segments.push_back({SegmentKind::Fixed, id, no_id, *width});
        else segments.push_back({SegmentKind::Span, id, variable("span " + std::to_string(variables.size()), id, false), 0});
    }
    void finish() {
        visit(ast.root);
        if (variables.empty()) throw Decline{};
        for (std::size_t i = 0; i < targets.size(); ++i)
            if (targets[i] && (!seen_captures[i] || reference_counts[i] != flat_reference_counts[i])) throw Decline{};
        for (auto& v : variables) {
            if (v.body == no_id) throw Decline{};
            v.alphabet = alphabet(ast, v.body);
            if (!v.count) {
                v.fixed_width = invariant(ast, v.body, all_bytes());
                const auto bounds = length_bounds(ast, v.body);
                v.minimum = bounds.first; v.maximum = bounds.second;
            }
        }
    }
    std::optional<Row> row(const Mask& mask) const {
        Row result{mask, std::vector<Integer>(variables.size()), 0, false, 0, {}, 0};
        for (const auto& s : segments) {
            if (s.kind == SegmentKind::Fixed || s.kind == SegmentKind::Count) {
                const auto value = invariant(ast, s.body, mask);
                if (!value) return {};
                if (s.kind == SegmentKind::Fixed) result.offset = bounded(Wide{result.offset} + *value);
                else result.coefficients[s.variable] = bounded(Wide{result.coefficients[s.variable]} + *value);
            } else {
                const auto& a = variables[s.variable].alphabet;
                if ((a & ~mask).none())
                    result.coefficients[s.variable] = bounded(Wide{result.coefficients[s.variable]} + 1);
                else if ((a & mask).any()) return {};
            }
        }
        return result;
    }
};

struct Formula {
    std::vector<Integer> weights;
    Integer denominator = 1;
    // Only actual observations enter these terms; the residual parameter is
    // handled separately. Sparse rows avoid work on compile-time zeroes.
    std::vector<std::pair<std::size_t, Integer>> terms;
};
Integer remainder(Wide value, Integer modulus) {
    const auto rem = value % modulus;
    return static_cast<Integer>(rem < 0 ? rem + modulus : rem);
}
Integer inverse_modulo(Integer value, Integer modulus) {
    Wide old_r = modulus, r = remainder(value, modulus), old_t = 0, t = 1;
    while (r != 0) {
        const Wide quotient = old_r / r;
        const Wide next_r = old_r - quotient * r;
        const Wide next_t = old_t - quotient * t;
        old_r = r; r = next_r; old_t = t; t = next_t;
    }
    if (old_r != 1) throw Decline{};
    return remainder(old_t, modulus);
}
Wide floor_divide(Wide numerator, Wide positive_denominator) {
    const Wide quotient = numerator / positive_denominator;
    return quotient - (numerator % positive_denominator < 0);
}
Wide ceil_divide(Wide numerator, Wide positive_denominator) {
    return -floor_divide(-numerator, positive_denominator);
}
bool bounded_quotient(Wide numerator, Integer denominator, std::size_t limit,
                      std::size_t& result) {
    if (numerator < 0) return false;
    Wide quotient;
    if (denominator == 1) quotient = numerator;
    else if (numerator <= Wide{std::numeric_limits<std::uint64_t>::max()}) {
        // Most inputs never need the wide division helper: retain its exact
        // result while using native unsigned division when representable.
        const auto value = static_cast<std::uint64_t>(numerator);
        const auto divisor = static_cast<std::uint64_t>(denominator);
        if (value % divisor != 0) return false;
        quotient = value / divisor;
    } else {
        if (numerator % denominator != 0) return false;
        quotient = numerator / denominator;
    }
    if (quotient > Wide{limit}) return false;
    result = static_cast<std::size_t>(quotient);
    return true;
}
struct Congruence {
    Integer gcd = 1, modulus = 1, inverse = 0;
    Integer merge_gcd = 1, merge_inverse = 0, period = 1;
};

// Retain independent rows without ever using floating-point rank decisions.
struct Basis {
    std::vector<std::vector<Rational>> rows;
    std::vector<std::size_t> pivots;
    bool add(const std::vector<Integer>& integers) {
        std::vector<Rational> row;
        for (Integer value : integers) row.emplace_back(value);
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const auto factor = row[pivots[i]];
            for (std::size_t j = pivots[i]; j < row.size(); ++j) row[j] = row[j] - factor * rows[i][j];
        }
        const auto p = std::find_if(row.begin(), row.end(), [](Rational value) { return value.numerator != 0; });
        if (p == row.end()) return false;
        const auto pivot = static_cast<std::size_t>(p - row.begin());
        const Rational divisor = row[pivot];
        for (std::size_t j = pivot; j < row.size(); ++j) row[j] = row[j] / divisor;
        rows.push_back(std::move(row)); pivots.push_back(pivot);
        return true;
    }
};
std::vector<Formula> invert(const std::vector<Row>& rows) {
    const std::size_t n = rows.size();
    std::vector<std::vector<Rational>> matrix(n, std::vector<Rational>(2 * n));
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = 0; j < n; ++j) matrix[i][j] = Rational(rows[i].coefficients[j]);
        matrix[i][n + i] = Rational(1);
    }
    for (std::size_t c = 0; c < n; ++c) {
        std::size_t pivot = c;
        while (pivot < n && matrix[pivot][c].numerator == 0) ++pivot;
        if (pivot == n) throw Decline{};
        std::swap(matrix[c], matrix[pivot]);
        const auto divisor = matrix[c][c];
        for (std::size_t j = 0; j < 2 * n; ++j) matrix[c][j] = matrix[c][j] / divisor;
        for (std::size_t r = 0; r < n; ++r) if (r != c) {
            const auto factor = matrix[r][c];
            for (std::size_t j = 0; j < 2 * n; ++j) matrix[r][j] = matrix[r][j] - factor * matrix[c][j];
        }
    }
    std::vector<Formula> result(n);
    for (std::size_t i = 0; i < n; ++i) {
        auto& formula = result[i];
        for (std::size_t j = 0; j < n; ++j) {
            const auto d = matrix[i][n + j].denominator;
            formula.denominator = bounded(Wide{formula.denominator / std::gcd(formula.denominator, d)} * d);
        }
        for (std::size_t j = 0; j < n; ++j) {
            const auto entry = matrix[i][n + j];
            formula.weights.push_back(bounded(Wide{entry.numerator} * (formula.denominator / entry.denominator)));
        }
    }
    return result;
}
}

struct AffinePlan::Impl {
    struct Segment {
        SegmentSpec spec;
        std::optional<RegularFragment> program;
        bool byte_class = false;
    };
    std::vector<Variable> variables;
    std::vector<Segment> segments;
    std::vector<Row> rows;
    std::vector<Formula> formulas;
    std::size_t free_variable = no_id;
    std::vector<Congruence> congruences;
    Integer residual_period = 1;
    std::vector<Wide> residual_increments;
    std::array<Wide, variable_limit> span_increments{};
    struct DirectObservation { std::size_t row; unsigned low, span; };
    std::vector<DirectObservation> direct_observations;
    bool histogram = false;
    struct DelimiterEvents {
        unsigned char byte;
        std::vector<std::pair<std::size_t, std::size_t>> events;
    };
    std::vector<DelimiterEvents> delimiter_events;
};

std::optional<AffinePlan> make_affine_plan(const Ast& ast, RegularOptions options) {
    if (ast.has_extended || ast.has_occurrence_paths || ast.regular() || !valid_ast(ast)) return {};
    try {
        Extractor extracted(ast);
        extracted.finish();
        auto impl = std::make_shared<AffinePlan::Impl>();
        impl->variables = extracted.variables;
        Basis basis;
        const auto append = [&](Row row) {
            if (impl->rows.size() < extracted.variables.size() && basis.add(row.coefficients))
                impl->rows.push_back(std::move(row));
        };
        // Length is always present, including for empty-language components.
        const auto length = extracted.row(all_bytes());
        if (!length) return {};
        append(*length);
        for (std::size_t i = 0; i < extracted.variables.size(); ++i) {
            if (extracted.variables[i].fixed_width) {
                Row row{{}, std::vector<Integer>(extracted.variables.size()), 0, true,
                        *extracted.variables[i].fixed_width, {}, 0};
                row.coefficients[i] = 1;
                append(std::move(row));
            }
        }
        // A fixed literal delimiter supplies a prefix-length observation when
        // all earlier variable spans forbid that byte. Earlier fixed fragments
        // may contain it, provided their occurrence counts are exact. The
        // selected ordinal is then invariant even if earlier fixed fragments
        // have ambiguous internal parses. Suffix alphabets need no restriction.
        for (std::size_t end = 0; end < extracted.segments.size() &&
             impl->rows.size() < extracted.variables.size(); ++end) {
            const auto& terminal = extracted.segments[end];
            if (terminal.kind != SegmentKind::Fixed || terminal.width != 1) continue;
            const auto terminal_alphabet = alphabet(ast, terminal.body);
            if (terminal_alphabet.count() != 1) continue;
            std::size_t delimiter = 0;
            while (!terminal_alphabet[delimiter]) ++delimiter;
            Row row{{}, std::vector<Integer>(extracted.variables.size()), 0, false, 0, {}, 0};
            row.delimiter = static_cast<unsigned char>(delimiter);
            row.ordinal = 1;
            bool eligible = true;
            for (std::size_t i = 0; i < end; ++i) {
                const auto& part = extracted.segments[i];
                if (part.kind == SegmentKind::Fixed) {
                    const auto count = invariant(ast, part.body, terminal_alphabet);
                    if (!count) { eligible = false; break; }
                    const Integer ordinal = bounded(Wide{row.ordinal} + *count);
                    if (Wide{ordinal} > std::numeric_limits<std::size_t>::max()) throw Decline{};
                    row.ordinal = static_cast<std::size_t>(ordinal);
                    row.offset = bounded(Wide{row.offset} + part.width);
                } else if (part.kind == SegmentKind::Count) {
                    const auto count = invariant(ast, part.body, terminal_alphabet);
                    if (!count || *count != 0) { eligible = false; break; }
                    row.coefficients[part.variable] = bounded(Wide{row.coefficients[part.variable]} + part.width);
                } else {
                    if (extracted.variables[part.variable].alphabet[delimiter]) { eligible = false; break; }
                    row.coefficients[part.variable] = bounded(Wide{row.coefficients[part.variable]} + 1);
                }
            }
            if (eligible) append(std::move(row));
        }
        std::vector<Mask> masks;
        const auto candidate = [&](const Mask& mask) {
            if (impl->rows.size() == extracted.variables.size() || mask.none() || mask.all() ||
                std::find(masks.begin(), masks.end(), mask) != masks.end()) return;
            masks.push_back(mask);
            if (auto row = extracted.row(mask)) append(std::move(*row));
        };
        for (const auto& v : extracted.variables) candidate(v.alphabet);
        for (const auto& node : ast.nodes) if (node.kind == Kind::CharSet) candidate(node.characters);
        for (std::size_t byte = 0; byte < 256; ++byte) { Mask mask; mask.set(byte); candidate(mask); }
        if (impl->rows.size() == extracted.variables.size()) {
            impl->formulas = invert(impl->rows);
        } else if (impl->rows.size() + 1 == extracted.variables.size()) {
            // Add t = x_free solely for symbolic inversion. It is not an input
            // observation, so rows/statistics keep the original exact rank.
            std::size_t free_variable = 0;
            while (std::find(basis.pivots.begin(), basis.pivots.end(), free_variable) != basis.pivots.end())
                ++free_variable;
            impl->free_variable = free_variable;
            auto augmented = impl->rows;
            Row free_row{{}, std::vector<Integer>(extracted.variables.size()), 0, true, 0, {}, 0};
            free_row.coefficients[free_variable] = 1;
            augmented.push_back(std::move(free_row));
            impl->formulas = invert(augmented);
            Integer period = 1;
            for (const auto& formula : impl->formulas) {
                Congruence constraint;
                const Integer slope = formula.weights.back();
                constraint.gcd = std::gcd(slope, formula.denominator);
                constraint.modulus = formula.denominator / constraint.gcd;
                if (constraint.modulus != 1)
                    constraint.inverse = inverse_modulo(slope / constraint.gcd, constraint.modulus);
                constraint.merge_gcd = std::gcd(period, constraint.modulus);
                const Integer reduced_modulus = constraint.modulus / constraint.merge_gcd;
                if (reduced_modulus != 1)
                    constraint.merge_inverse = inverse_modulo(period / constraint.merge_gcd, reduced_modulus);
                period = bounded(Wide{period} * reduced_modulus);
                if (Wide{period} > std::numeric_limits<std::size_t>::max()) throw Decline{};
                constraint.period = period;
                impl->congruences.push_back(constraint);
            }
            impl->residual_period = period;
            for (const auto& formula : impl->formulas) {
                // The common period solves every homogeneous congruence, so
                // this division is exact. Two bounded coefficients multiply
                // into at most 80 bits; retain a signed wide increment.
                const Wide numerator = Wide{formula.weights.back()} * period;
                if (numerator % formula.denominator != 0) throw Decline{};
                impl->residual_increments.push_back(numerator / formula.denominator);
            }
        } else return {};
        for (auto& formula : impl->formulas)
            for (std::size_t row = 0; row < impl->rows.size(); ++row)
                if (formula.weights[row]) formula.terms.emplace_back(row, formula.weights[row]);
        bool direct_eligible = true;
        for (std::size_t i = 0; i < impl->rows.size(); ++i) {
            const auto& row = impl->rows[i];
            if (!row.constant && !row.delimiter && !row.mask.all()) {
                impl->histogram = true;
                unsigned low = 256, high = 0;
                for (unsigned byte = 0; byte < 256; ++byte) if (row.mask[byte]) {
                    low = std::min(low, byte); high = byte;
                }
                if (low != 256 && row.mask.count() == high - low + 1)
                    impl->direct_observations.push_back({i, low, high - low});
                else direct_eligible = false;
            }
            if (row.delimiter) {
                auto found = std::find_if(impl->delimiter_events.begin(), impl->delimiter_events.end(),
                    [&](const auto& group) { return group.byte == *row.delimiter; });
                if (found == impl->delimiter_events.end()) {
                    impl->delimiter_events.push_back({*row.delimiter, {}});
                    found = impl->delimiter_events.end() - 1;
                }
                found->events.emplace_back(row.ordinal, i);
            }
        }
        // Once a general histogram is necessary, derive every mask from it
        // instead of performing additional input scans. At most four direct
        // range passes bound the number of input scans independently of N.
        if (direct_eligible && impl->direct_observations.size() <= 4) impl->histogram = false;
        else impl->direct_observations.clear();
        // Preserve the prior byte-order traversal for diagnostic accounting,
        // but visit only delimiters selected by the independent row basis.
        std::sort(impl->delimiter_events.begin(), impl->delimiter_events.end(),
                  [](const auto& a, const auto& b) { return a.byte < b.byte; });
        for (auto& group : impl->delimiter_events) std::sort(group.events.begin(), group.events.end());
        std::size_t nfa = 0, dfa = 0;
        // Each prefix coefficient is bounded by the positive length-row
        // coefficient (2^40). At most eight increments of magnitude 2^80
        // contribute, keeping the signed sum strictly below 2^124.
        Wide position_increment = 0;
        for (const auto& spec : extracted.segments) {
            std::optional<RegularFragment> program;
            if (spec.kind != SegmentKind::Reference) {
                program.emplace(ast, spec.body, options);
                const auto stats = program->compiled_statistics();
                if (stats.nfa_states > options.max_nfa_states - nfa ||
                    stats.dfa_states > options.max_dfa_states - dfa) return {};
                nfa += stats.nfa_states; dfa += stats.dfa_states;
            }
            const bool byte_class = spec.kind == SegmentKind::Span && program->byte_class_repetition();
            impl->segments.push_back({spec, std::move(program), byte_class});
            if (impl->free_variable != no_id) {
                if (spec.kind == SegmentKind::Span)
                    impl->span_increments[spec.variable] = position_increment;
                if (spec.kind != SegmentKind::Fixed)
                    position_increment += impl->residual_increments[spec.variable] *
                        (spec.kind == SegmentKind::Count ? Wide{spec.width} : Wide{1});
            }
        }
        if (position_increment != 0) throw Decline{};
        return AffinePlan(std::move(impl));
    } catch (const Decline&) { return {}; }
      catch (const std::length_error&) { return {}; }
}

bool AffinePlan::match(std::string_view subject, AffineStatistics* statistics,
                       AffineMatchOptions options) const {
    AffineStatistics stats;
    const std::size_t n = impl_->variables.size();
    stats.variables = n; stats.equations = impl_->rows.size();
    const bool residual_search = impl_->free_variable != no_id;
    stats.free_variables = residual_search ? 1 : 0;
    std::array<std::size_t, variable_limit> values{};
    std::size_t solved = 0;
    const auto finish = [&](bool answer) {
        stats.search_complete = !stats.resource_limited;
        if (statistics) {
            stats.observed_bytes = stats.histogram_bytes;
            saturated_accumulate(stats.observed_bytes, stats.direct_count_bytes);
            saturated_accumulate(stats.observed_bytes, stats.delimiter_bytes);
            stats.values.assign(values.begin(), values.begin() + solved);
            *statistics = std::move(stats);
        }
        return answer;
    };
    std::array<std::size_t, 256> histogram;
    std::array<std::size_t, variable_limit> observations{};
    std::array<std::size_t, variable_limit> delimiter_positions;
    delimiter_positions.fill(no_id);
    if (impl_->histogram) {
        histogram.fill(0);
        for (unsigned char byte : subject) ++histogram[byte];
        stats.histogram_bytes = subject.size();
    }
    for (const auto& observation : impl_->direct_observations) {
        observations[observation.row] = count_range(subject, observation.low, observation.span);
        saturated_accumulate(stats.direct_count_bytes, subject.size());
    }
    // Delimiter rows take priority over histogram rows at compile time. Locate
    // each selected ordinal with the library's byte-search kernel, monotonically
    // advancing for all rows sharing a delimiter, without a per-byte event loop.
    for (const auto& group : impl_->delimiter_events) {
        std::size_t ordinal = 0, next_start = 0, found = no_id;
        for (const auto& [target, row] : group.events) {
            while (ordinal < target) {
                found = subject.find(static_cast<char>(group.byte), next_start);
                ++stats.delimiter_searches;
                const auto examined = found == std::string_view::npos
                    ? subject.size() - next_start : found - next_start + 1;
                saturated_accumulate(stats.delimiter_bytes, examined);
                if (found == std::string_view::npos) { stats.arithmetic_rejected = true; return finish(false); }
                ++ordinal;
                next_start = found + 1;
            }
            delimiter_positions[row] = found;
        }
    }
    std::array<Wide, variable_limit> rhs{};
    for (std::size_t i = 0; i < impl_->rows.size(); ++i) {
        const auto& row = impl_->rows[i];
        Wide value;
        if (row.delimiter) {
            if (delimiter_positions[i] == no_id) { stats.arithmetic_rejected = true; return finish(false); }
            value = delimiter_positions[i];
        } else if (row.constant) value = row.value;
        else if (row.mask.all()) value = subject.size();
        else if (!impl_->histogram) value = observations[i];
        else {
            value = 0;
            for (std::size_t byte = 0; byte < 256; ++byte) if (row.mask[byte]) value += histogram[byte];
        }
        rhs[i] = value - row.offset;
    }
    const auto limit = [&](const char* reason) -> void {
        stats.resource_limited = true;
        finish(false);
        throw ResourceLimit(reason);
    };
    std::size_t validation_budget = options.max_validation_bytes;
    const auto charge = [&](std::size_t width) {
        if (!residual_search) return;
        if (width > validation_budget)
            limit("affine residual matcher: validation byte limit exceeded");
        validation_budget -= width;
    };
    std::array<std::pair<std::size_t, std::size_t>, variable_limit> spans{};
    bool layout_ready = false;
    struct ValidatedInterval { std::size_t begin = 0, end = 0; bool known = false; };
    std::array<ValidatedInterval, variable_limit> class_intervals{};
    // Compute all capture locations once so forward obligations are checked
    // against the sole later capture exactly as backward references are.
    // Later lattice points advance those locations by exact signed increments.
    const auto validate = [&]() {
        const auto segment_length = [&](const SegmentSpec& segment) -> Wide {
            return segment.kind == SegmentKind::Fixed ? Wide{segment.width} :
                segment.kind == SegmentKind::Count ? Wide{segment.width} * values[segment.variable]
                                                   : Wide{values[segment.variable]};
        };
        std::size_t position = 0;
        if (!layout_ready) {
            for (const auto& segment : impl_->segments) {
                const auto& spec = segment.spec;
                const Wide length = segment_length(spec);
                if (length > Wide{subject.size() - position}) return false;
                const auto width = static_cast<std::size_t>(length);
                if (spec.kind == SegmentKind::Span) spans[spec.variable] = {position, width};
                position += width;
            }
            if (position != subject.size()) return false;
            layout_ready = true;
        }
        if (residual_search) {
            // Wrong splits often fail a copy comparison after only a few bytes.
            // Check these dependencies before scanning candidate regular spans.
            // Charge the whole compared span even if memcmp exits early.
            position = 0;
            for (const auto& segment : impl_->segments) {
                const auto& spec = segment.spec;
                const auto width = static_cast<std::size_t>(segment_length(spec));
                if (spec.kind == SegmentKind::Reference) {
                    const auto [capture_start, capture_width] = spans[spec.variable];
                    charge(width);
                    saturated_accumulate(stats.equality_bytes, width);
                    if (subject.substr(position, width) != subject.substr(capture_start, capture_width))
                        return false;
                }
                position += width;
            }
        }
        position = 0;
        for (const auto& segment : impl_->segments) {
            const auto& spec = segment.spec;
            const auto width = static_cast<std::size_t>(segment_length(spec));
            if (residual_search && spec.kind == SegmentKind::Reference) {
                position += width;
                continue;
            }
            const auto piece = subject.substr(position, width);
            charge(width);
            if (spec.kind == SegmentKind::Reference) {
                const auto [capture_start, capture_width] = spans[spec.variable];
                saturated_accumulate(stats.equality_bytes, width);
                if (piece != subject.substr(capture_start, capture_width)) return false;
            } else if (spec.kind == SegmentKind::Count) {
                saturated_accumulate(stats.validated_bytes, width);
                if (segment.program->matching_copies(piece, static_cast<std::size_t>(spec.width),
                                                    values[spec.variable]) != values[spec.variable]) return false;
            } else {
                saturated_accumulate(stats.validated_bytes, width);
                if (residual_search && segment.byte_class) {
                    // A width-one Cartesian repetition has only its exact
                    // length bounds and a per-byte predicate. Residual bounds
                    // already enforce the former. Reuse the successfully
                    // validated interval, scanning just newly exposed bytes.
                    // Full-span budget charges above remain conservative.
                    auto& interval = class_intervals[spec.variable];
                    const std::size_t end = position + width;
                    const auto check = [&](std::size_t begin, std::size_t length) {
                        return segment.program->byte_class_prefix(subject.substr(begin, length)) == length;
                    };
                    if (!interval.known || end < interval.begin || position > interval.end) {
                        if (!check(position, width)) return false;
                        interval = {position, end, true};
                    } else {
                        if (position < interval.begin && !check(position, interval.begin - position)) return false;
                        if (end > interval.end && !check(interval.end, end - interval.end)) return false;
                        interval.begin = std::min(position, interval.begin);
                        interval.end = std::max(end, interval.end);
                    }
                } else if (!segment.program->full_match(piece)) return false;
            }
            position += width;
        }
        return true;
    };
    if (!residual_search) {
        // Preserve the single-assignment path without residual interval or
        // congruence work, including its independence from search budgets.
        for (std::size_t i = 0; i < n; ++i) {
            const auto& formula = impl_->formulas[i];
            Wide numerator = 0;
            for (const auto& [row, weight] : formula.terms) numerator += Wide{weight} * rhs[row];
            if (!bounded_quotient(numerator, formula.denominator, subject.size(), values[i])) {
                stats.arithmetic_rejected = true;
                return finish(false);
            }
            ++solved;
        }
        stats.candidate_assignments = 1;
        return finish(validate());
    }

    // For x_i = (constant_i + slope_i*t) / denominator_i, intersect
    // nonnegativity, regular length bounds, and the total-length upper bounds.
    // Every length-row coefficient is positive: each variable consumes at
    // least one byte per unit somewhere in the once-executed sequence.
    std::array<Wide, variable_limit> constants{};
    Wide lower = 0, upper = subject.size();
    const Wide available = Wide{subject.size()} - impl_->rows.front().offset;
    if (available < 0) { stats.arithmetic_rejected = true; return finish(false); }
    Integer residue = 0, period = 1;
    for (std::size_t i = 0; i < n; ++i) {
        const auto& formula = impl_->formulas[i];
        Wide constant = 0;
        for (const auto& [row, weight] : formula.terms) constant += Wide{weight} * rhs[row];
        constants[i] = constant;
        const auto& variable = impl_->variables[i];
        const Wide minimum = variable.minimum;
        const Integer length_coefficient = impl_->rows.front().coefficients[i];
        const Wide maximum = variable.maximum
            ? std::min(available / length_coefficient, Wide{*variable.maximum})
            : available / length_coefficient;
        if (minimum > maximum) { stats.arithmetic_rejected = true; return finish(false); }
        const Wide min_numerator = minimum * formula.denominator;
        const Wide max_numerator = maximum * formula.denominator;
        const Integer slope = formula.weights.back();
        if (slope > 0) {
            lower = std::max(lower, ceil_divide(min_numerator - constant, slope));
            upper = std::min(upper, floor_divide(max_numerator - constant, slope));
        } else if (slope < 0) {
            lower = std::max(lower, ceil_divide(constant - max_numerator, -slope));
            upper = std::min(upper, floor_divide(constant - min_numerator, -slope));
        } else if (constant < min_numerator || constant > max_numerator) {
            stats.arithmetic_rejected = true; return finish(false);
        }
        if (lower > upper) { stats.arithmetic_rejected = true; return finish(false); }

        // slope*t = -constant (mod denominator), followed by generalized CRT.
        // Reduce before multiplying, keeping wide arithmetic safely bounded.
        const auto& congruence = impl_->congruences[i];
        if (constant % congruence.gcd != 0) {
            stats.arithmetic_rejected = true; return finish(false);
        }
        const Integer target = congruence.modulus == 1 ? 0 : remainder(
            Wide{remainder(-constant / congruence.gcd, congruence.modulus)} * congruence.inverse,
            congruence.modulus);
        const Integer difference = target - residue;
        if (difference % congruence.merge_gcd != 0) {
            stats.arithmetic_rejected = true; return finish(false);
        }
        const Integer reduced_modulus = congruence.modulus / congruence.merge_gcd;
        const Integer increment = reduced_modulus == 1 ? 0 : remainder(
            Wide{difference / congruence.merge_gcd} * congruence.merge_inverse, reduced_modulus);
        residue = remainder(Wide{residue} + Wide{period} * increment, congruence.period);
        period = congruence.period;
    }
    const Wide first = lower + remainder(Wide{residue} - lower, period);
    stats.candidate_stride = static_cast<std::size_t>(period);
    if (first > upper) { stats.arithmetic_rejected = true; return finish(false); }
    const Wide last = first + (upper - first) / period * period;
    stats.candidate_lower = static_cast<std::size_t>(first);
    stats.candidate_upper = static_cast<std::size_t>(last);
    // One exact division initializes each variable. Consecutive lattice points
    // then differ by a compile-time integer, without repeated multiply/divide.
    for (std::size_t i = 0; i < n; ++i) {
        const auto& formula = impl_->formulas[i];
        values[i] = static_cast<std::size_t>((constants[i] +
            Wide{formula.weights.back()} * first) / formula.denominator);
    }
    for (Wide candidate = first; candidate <= last; candidate += period) {
        if (stats.candidate_assignments == options.max_candidates)
            limit("affine residual matcher: candidate limit exceeded");
        if (candidate != first)
            for (std::size_t i = 0; i < n; ++i) {
                values[i] = static_cast<std::size_t>(Wide{values[i]} + impl_->residual_increments[i]);
                if (layout_ready && !impl_->variables[i].count) {
                    spans[i].first = static_cast<std::size_t>(Wide{spans[i].first} + impl_->span_increments[i]);
                    spans[i].second = values[i];
                }
            }
        solved = n;
        ++stats.candidate_assignments;
        if (validate()) return finish(true);
    }
    return finish(false);
}

bool AffinePlan::residual() const { return impl_->free_variable != no_id; }

std::string AffinePlan::describe() const {
    std::ostringstream result;
    result << "Affine span plan: " << impl_->variables.size() << " variables, " << impl_->rows.size()
           << " independent exact equations (length, character counts, fixed widths, delimiter offsets); ";
    if (residual()) result << "one bounded free integer variable ("
                           << impl_->variables[impl_->free_variable].name << "); congruence stride "
                           << impl_->residual_period
                           << "; exact residual search with incremental values and span positions";
    else result << "one assignment";
    result << "; ordered regular validation and exact copies";
    if (!impl_->direct_observations.empty())
        result << "; " << impl_->direct_observations.size() << " direct byte-range observations";
    else if (impl_->histogram) result << "; shared byte histogram";
    if (residual() && std::any_of(impl_->segments.begin(), impl_->segments.end(),
            [](const auto& segment) { return segment.byte_class; }))
        result << "; overlapping byte-class validation reuse";
    for (std::size_t r = 0; r < impl_->rows.size(); ++r) {
        const auto& row = impl_->rows[r];
        result << "\n  ";
        if (row.delimiter) result << "offset-of-byte[" << static_cast<unsigned>(*row.delimiter)
                                  << ", occurrence " << row.ordinal << ']';
        else if (row.constant) result << row.value;
        else if (row.mask.all()) result << "N";
        else result << "bytes-in-mask[" << row.mask.count() << ']';
        result << " = " << row.offset;
        for (std::size_t i = 0; i < row.coefficients.size(); ++i)
            if (row.coefficients[i]) result << " + " << row.coefficients[i] << "*(" << impl_->variables[i].name << ')';
    }
    return result.str();
}
}

#else
namespace crank {
struct AffinePlan::Impl {};
std::optional<AffinePlan> make_affine_plan(const Ast&, RegularOptions) { return {}; }
bool AffinePlan::match(std::string_view, AffineStatistics*, AffineMatchOptions) const {
    throw std::logic_error("affine arithmetic requires a compiler with 128-bit integers");
}
bool AffinePlan::residual() const { return false; }
std::string AffinePlan::describe() const { return "Affine span plan: unavailable on this compiler"; }
}
#endif
