#include "optimizations.hpp"
#include "integer_row.hpp"

#include <algorithm>
#include <numeric>
#include <sstream>

namespace crank {
namespace {
constexpr auto limit = std::numeric_limits<std::size_t>::max();

std::optional<std::size_t> add(std::size_t a, std::size_t b) {
    if (a > limit - b) return {};
    return a + b;
}
std::optional<std::size_t> multiply(std::size_t a, std::size_t b) {
    if (b && a > limit / b) return {};
    return a * b;
}
struct Summary {
    std::size_t minimum = 0;
    std::optional<std::size_t> maximum = 0;
    // Every accepted length is congruent to offset modulo modulus.
    // Modulus zero means exactly offset; modulus one imposes no constraint.
    std::size_t offset = 0, modulus = 0;
};

Summary summarize(const Ast& ast, NodeId id) {
    const auto& node = ast.nodes[id];
    switch (node.kind) {
    case Kind::Empty:
    case Kind::Define: return {};
    case Kind::CharSet: return {1, 1, 1, 0};
    case Kind::Reference:
    case Kind::Call: return {0, {}, 0, 1};
    case Kind::Capture: return summarize(ast, node.children.front());
    case Kind::Concat: {
        Summary result;
        for (const auto child : node.children) {
            const auto part = summarize(ast, child);
            result.minimum = add(result.minimum, part.minimum).value_or(limit);
            result.maximum = result.maximum && part.maximum
                ? add(*result.maximum, *part.maximum) : std::nullopt;
            const auto sum = add(result.offset, part.offset);
            result.modulus = sum ? std::gcd(result.modulus, part.modulus) : 1;
            result.offset = sum.value_or(0);
        }
        return result;
    }
    case Kind::Alt: {
        if (node.children.empty()) return {};
        auto result = summarize(ast, node.children.front());
        for (std::size_t i = 1; i < node.children.size(); ++i) {
            const auto part = summarize(ast, node.children[i]);
            result.minimum = std::min(result.minimum, part.minimum);
            result.maximum = result.maximum && part.maximum
                ? std::optional{std::max(*result.maximum, *part.maximum)} : std::nullopt;
            const auto difference = result.offset >= part.offset
                ? result.offset - part.offset : part.offset - result.offset;
            result.modulus = std::gcd(std::gcd(result.modulus, part.modulus), difference);
        }
        return result;
    }
    case Kind::Repeat: {
        const auto body = summarize(ast, node.children.front());
        const bool named = node.bind_count != no_id || node.use_count != no_id ||
                           node.count_path != no_id;
        const auto lower = named ? 0 : node.lower;
        const auto upper = named ? std::optional<std::size_t>{} : node.upper;
        if (upper && *upper == 0) return {};
        Summary result;
        result.minimum = multiply(lower, body.minimum).value_or(limit);
        result.maximum = body.maximum && *body.maximum == 0
            ? std::optional<std::size_t>{0}
            : upper && body.maximum ? multiply(*upper, *body.maximum) : std::nullopt;
        const auto offset = multiply(lower, body.offset);
        result.offset = offset.value_or(0);
        // k independent occurrences retain the body's modulus, not k times it.
        result.modulus = !offset ? 1 : upper && *upper == lower
            ? body.modulus : std::gcd(body.modulus, body.offset);
        return result;
    }
    }
    return {0, {}, 0, 1};
}

bool count_free(const Ast& ast, NodeId id) {
    const auto& node = ast.nodes[id];
    if (node.kind == Kind::Reference || node.kind == Kind::Call || node.kind == Kind::Define ||
        node.bind_count != no_id || node.use_count != no_id || node.count_path != no_id)
        return false;
    return std::all_of(node.children.begin(), node.children.end(),
        [&](NodeId child) { return count_free(ast, child); });
}

// Only a straight, once-executed sequence supplies binders. No propagation
// across alternatives or repeated enclosing groups with count effects.
struct AffineExtractor {
    const Ast& ast;
    std::size_t offset = 0;
    std::vector<std::size_t> coefficients;
    std::vector<bool> bound;
    bool found = false;

    bool visit(NodeId id) {
        const auto& node = ast.nodes[id];
        if (node.count_path != no_id) return false;
        if (node.kind == Kind::Capture) return visit(node.children.front());
        if (node.kind == Kind::Concat) {
            return std::all_of(node.children.begin(), node.children.end(),
                [&](NodeId child) { return visit(child); });
        }
        if (node.kind == Kind::Alt || node.kind == Kind::Reference) return false;
        if (node.kind == Kind::Repeat && (node.bind_count != no_id || node.use_count != no_id)) {
            if (node.lower != 0 || node.upper) return false;
            const auto body_id = node.children.front();
            if (!count_free(ast, body_id)) return false;
            const auto body = summarize(ast, body_id);
            if (!body.maximum || body.minimum != *body.maximum || body.minimum == 0) return false;
            const bool binding = node.bind_count != no_id;
            const auto slot = binding ? node.bind_count : node.use_count;
            if (slot >= bound.size() || (binding ? bound[slot] : !bound[slot])) return false;
            bound[slot] = true;
            const auto total = add(coefficients[slot], body.minimum);
            if (!total) return false;
            coefficients[slot] = *total;
            found = true;
            return true;
        }
        if (!count_free(ast, id)) return false;
        const auto constant = summarize(ast, id);
        if (!constant.maximum || constant.minimum != *constant.maximum) return false;
        const auto total = add(offset, constant.minimum);
        if (!total) return false;
        offset = *total;
        return true;
    }
};

// A once-executed concatenation contributes constants and independently chosen
// repetition counts. Fixed-width alternatives are constants too. Variable
// alternatives, repeated variable forms and call/effect boundaries decline;
// their length unions need a richer certificate, not a guessed integer row.
// The descriptor is only a necessary condition: impossible/empty byte bodies
// may still contribute lengths here, which can only miss a rejection.
struct IntegerLengthExtractor {
    struct Form {
        IntegerLengthRow row;
        std::size_t variables = 0;
    };
    const Ast& ast;
    std::size_t remaining = 4096;

    std::optional<Form> visit(NodeId id, std::size_t depth = 0) {
        if (!remaining || depth >= 128 || id >= ast.nodes.size()) return {};
        --remaining;
        const auto& node = ast.nodes[id];
        if (node.bind_count != no_id || node.use_count != no_id || node.count_path != no_id)
            return {};
        switch (node.kind) {
        case Kind::Empty: return Form{};
        case Kind::CharSet: {
            Form result;
            result.row.offset = 1;
            return result;
        }
        case Kind::Capture:
            return node.children.size() == 1 ? visit(node.children.front(), depth + 1)
                                             : std::nullopt;
        case Kind::Concat: {
            Form result;
            for (const auto child : node.children) {
                const auto part = visit(child, depth + 1);
                if (!part || result.variables + part->variables > 2) return {};
                const auto offset = add(result.row.offset, part->row.offset);
                if (!offset) return {};
                result.row.offset = *offset;
                for (std::size_t i = 0; i < part->variables; ++i)
                    result.row.terms[result.variables++] = part->row.terms[i];
            }
            return result;
        }
        case Kind::Alt: {
            std::optional<Form> result;
            for (const auto child : node.children) {
                auto part = visit(child, depth + 1);
                if (!part || part->variables ||
                    (result && result->row.offset != part->row.offset)) return {};
                result = std::move(part);
            }
            return result;
        }
        case Kind::Repeat: {
            if (node.children.size() != 1 || (node.upper && *node.upper < node.lower))
                return {};
            // A syntactically dead repetition contributes zero regardless of
            // its body; no body traversal or multiplication is required.
            if (node.upper && *node.upper == 0) return Form{};
            auto body = visit(node.children.front(), depth + 1);
            if (!body) return {};
            if (node.upper && node.lower == *node.upper) {
                if (node.lower == 1) return body;
                if (body->variables) return {};
                const auto offset = multiply(body->row.offset, node.lower);
                if (!offset) return {};
                body->row.offset = *offset;
                return body;
            }
            if (body->variables) return {};
            if (body->row.offset == 0) return Form{};
            Form result;
            result.variables = 1;
            result.row.terms[0] = {body->row.offset, node.lower, node.upper};
            return result;
        }
        case Kind::Reference:
        case Kind::Call:
        case Kind::Define: return {};
        }
        return {};
    }
};

std::optional<IntegerLengthRow> integer_length_row(const Ast& ast) {
    if (ast.has_counts || ast.has_references || ast.has_calls ||
        ast.has_extended || ast.has_occurrence_paths) return {};
    // Admission is cheaper than constructing a Form at every literal leaf.
    // Two independent variables require at least two syntactically variable
    // repeat sites. The scan inspects at most 4096 existing node records and
    // stops on the second candidate; the structural traversal has its own cap.
    if (ast.nodes.size() > 4096) return {};
    std::size_t candidates = 0;
    for (const auto& node : ast.nodes) {
        if (node.kind == Kind::Repeat && (!node.upper || node.lower != *node.upper) &&
            ++candidates == 2) break;
    }
    if (candidates != 2) return {};
    IntegerLengthExtractor extractor{ast};
    auto form = extractor.visit(ast.root);
    if (!form || form->variables != 2) return {};
    const auto& a = form->row.terms[0];
    const auto& b = form->row.terms[1];
    // For two unbounded variables, when one width divides the other, range
    // plus gcd already gives the exact length set. Avoid a useless query-time
    // solver on ordinary a*b*, whitespace/identifier and equal-width forms.
    if (!a.upper && !b.upper && (a.width % b.width == 0 || b.width % a.width == 0))
        return {};
    return form->row;
}

struct PlanBuilder {
    const Ast& ast;
    std::size_t simplifications = 0;

    std::optional<std::bitset<256>> one_byte(NodeId id) {
        const auto& node = ast.nodes[id];
        if (node.bind_count != no_id || node.use_count != no_id || node.count_path != no_id)
            return {};
        if (node.kind == Kind::Capture) return one_byte(node.children.front());
        if (node.kind == Kind::CharSet) return node.characters;
        if (node.kind != Kind::Alt || node.children.empty()) return {};
        std::bitset<256> result;
        for (const auto child : node.children) {
            const auto part = one_byte(child);
            if (!part) return {};
            result |= *part;
        }
        ++simplifications;
        return result;
    }

    bool flatten(NodeId id, std::vector<GreedySegment>& output) {
        const auto& node = ast.nodes[id];
        if (node.bind_count != no_id || node.use_count != no_id || node.count_path != no_id)
            return false;
        if (node.kind == Kind::Empty) return true;
        if (node.kind == Kind::Capture) return flatten(node.children.front(), output);
        if (node.kind == Kind::Concat) {
            return std::all_of(node.children.begin(), node.children.end(),
                [&](NodeId child) { return flatten(child, output); });
        }
        if (const auto characters = one_byte(id)) {
            output.push_back({*characters, 1, 1});
            return true;
        }
        if (node.kind != Kind::Repeat) return false;
        if (node.upper && *node.upper == 0) return true;
        if (const auto characters = one_byte(node.children.front())) {
            output.push_back({*characters, node.lower, node.upper});
            return true;
        }
        if (node.lower == 0 && !node.upper) {
            std::vector<GreedySegment> body;
            if (!flatten(node.children.front(), body)) return false;
            std::bitset<256> characters;
            for (const auto& part : body) {
                if (part.lower != 0 || part.upper) return false;
                characters |= part.characters;
            }
            // (A* B* ...)* = (A union B union ...)* for membership,
            // even when the alphabets overlap and some groups are empty.
            ++simplifications;
            if (!body.empty()) output.push_back({characters, 0, {}});
            return true;
        }
        return false;
    }
};
} // namespace

LengthFilter::LengthFilter(const Ast& ast, bool prepare_integer_row) {
    // Occurrence-dependent lengths are analyzed by OccurrenceAnalysis. Leaving
    // this older filter unconstrained is sound for direct API callers too.
    if (ast.has_extended || ast.has_occurrence_paths) return;
    const auto summary = summarize(ast, ast.root);
    minimum_ = summary.minimum;
    maximum_ = summary.maximum;
    offset_ = summary.offset;
    modulus_ = summary.modulus;
    if (prepare_integer_row) integer_row_ = integer_length_row(ast);
    AffineExtractor extractor{ast, 0, std::vector<std::size_t>(ast.count_names.size()),
                             std::vector<bool>(ast.count_names.size())};
    if (!ast.has_references && ast.has_counts && extractor.visit(ast.root) && extractor.found) {
        affine_ = true;
        affine_offset_ = extractor.offset;
        coefficients_ = std::move(extractor.coefficients);
        names_ = ast.count_names;
        for (const auto coefficient : coefficients_) affine_gcd_ = std::gcd(affine_gcd_, coefficient);
    }
}

IntegerLengthCheck LengthFilter::check_integer_row(std::size_t length,
                                                  std::size_t max_steps) const {
    IntegerLengthCheck result;
    if (!integer_row_) return result;
    result.checked = true;
    struct Exhausted {};
    const auto step = [&] {
        if (result.steps == max_steps) throw Exhausted{};
        ++result.steps;
    };
    try {
        // Charge the offset proof too: a zero allowance always declines and
        // cannot accidentally turn an unexecuted proof into a rejection.
        step();
        if (length < integer_row_->offset) {
            result.impossible = true;
            return result;
        }
        const auto& a = integer_row_->terms[0];
        const auto& b = integer_row_->terms[1];
        const auto answer = detail::solve_integer_row(
            a.width, b.width, length - integer_row_->offset,
            a.lower, a.upper, b.lower, b.upper, step);
        result.impossible = answer.status == detail::IntegerRowStatus::None;
    } catch (const Exhausted&) {
        result.exhausted = true;
    }
    return result;
}

bool LengthFilter::accepts(std::size_t length) const {
    if (length < minimum_ || (maximum_ && length > *maximum_)) return false;
    if (modulus_ == 0 ? length != offset_ : length % modulus_ != offset_ % modulus_) return false;
    if (!affine_) return true;
    if (length < affine_offset_) return false;
    return affine_gcd_ == 0 ? length == affine_offset_
                           : (length - affine_offset_) % affine_gcd_ == 0;
}

std::string LengthFilter::describe() const {
    std::ostringstream out;
    out << "length in [" << minimum_ << ", ";
    if (maximum_) out << *maximum_; else out << "unbounded";
    out << "]";
    if (modulus_ == 0) out << "; exact length " << offset_;
    else if (modulus_ != 1) out << "; length = " << offset_ % modulus_ << " mod " << modulus_;
    if (affine_) {
        out << "; necessary shared-count equation: length = " << affine_offset_;
        for (std::size_t i = 0; i < coefficients_.size(); ++i)
            if (coefficients_[i]) out << " + " << coefficients_[i] << '*' << names_[i];
        out << " (nonnegative counts; gcd " << affine_gcd_ << ')';
    }
    if (integer_row_) {
        out << "; necessary bounded integer row: length = " << integer_row_->offset;
        for (std::size_t i = 0; i < 2; ++i) {
            const auto& term = integer_row_->terms[i];
            out << " + " << term.width << '*' << (i ? 'y' : 'x') << " [" << term.lower << ',';
            if (term.upper) out << *term.upper; else out << "unbounded";
            out << ']';
        }
    }
    return out.str();
}

std::optional<GreedyPlan> make_greedy_plan(const Ast& ast) {
    if (!ast.regular()) return {};
    GreedyPlan plan;
    PlanBuilder builder{ast};
    if (!builder.flatten(ast.root, plan.segments)) return {};
    std::bitset<256> suffix_first;
    for (auto it = plan.segments.rbegin(); it != plan.segments.rend(); ++it) {
        if (!it->upper || it->lower != *it->upper) {
            if ((it->characters & suffix_first).any()) return {};
            ++plan.certified_sites;
        }
        if (it->lower) suffix_first = it->characters;
        else suffix_first |= it->characters;
    }
    plan.simplifications = builder.simplifications;
    return plan;
}

bool GreedyPlan::match(std::string_view subject) const {
    std::size_t position = 0;
    for (const auto& segment : segments) {
        std::size_t count = 0;
        while (position < subject.size() && (!segment.upper || count < *segment.upper) &&
               segment.characters.test(static_cast<unsigned char>(subject[position]))) {
            ++position;
            ++count;
        }
        if (count < segment.lower) return false;
    }
    return position == subject.size();
}

std::string GreedyPlan::describe() const {
    return "certified Boolean greedy plan: " + std::to_string(segments.size()) +
        " segments, " + std::to_string(certified_sites) + " variable sites, " +
        std::to_string(simplifications) + " language simplifications";
}
}
