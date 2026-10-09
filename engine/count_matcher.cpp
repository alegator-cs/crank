#include "count_matcher.hpp"
#include "fragment_matcher.hpp"

#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace crank {
namespace {
constexpr auto limit = std::numeric_limits<std::size_t>::max();
std::optional<std::size_t> checked_add(std::size_t a, std::size_t b) {
    return a > limit - b ? std::nullopt : std::optional{a + b};
}
std::optional<std::size_t> checked_multiply(std::size_t a, std::size_t b) {
    return b && a > limit / b ? std::nullopt : std::optional{a * b};
}
bool effect_free(const Ast& ast, NodeId id) {
    const auto& node = ast.nodes[id];
    if (node.kind == Kind::Reference || node.kind == Kind::Call || node.kind == Kind::Define ||
        node.bind_count != no_id || node.use_count != no_id || node.count_path != no_id)
        return false;
    return std::all_of(node.children.begin(), node.children.end(),
        [&](NodeId child) { return effect_free(ast, child); });
}

// A successful result proves every accepted word has this width. Empty
// languages may also have a width; actual matching still rejects their words.
std::optional<std::size_t> fixed_width(const Ast& ast, NodeId id) {
    const auto& node = ast.nodes[id];
    if (node.bind_count != no_id || node.use_count != no_id || node.count_path != no_id) return {};
    switch (node.kind) {
    case Kind::Empty: return 0;
    case Kind::CharSet: return 1;
    case Kind::Reference:
    case Kind::Call:
    case Kind::Define: return {};
    case Kind::Capture: return fixed_width(ast, node.children.front());
    case Kind::Concat: {
        std::size_t width = 0;
        for (const auto child : node.children) {
            const auto part = fixed_width(ast, child);
            if (!part) return {};
            const auto total = checked_add(width, *part);
            if (!total) return {};
            width = *total;
        }
        return width;
    }
    case Kind::Alt: {
        if (node.children.empty()) return {};
        const auto width = fixed_width(ast, node.children.front());
        if (!width) return {};
        for (std::size_t i = 1; i < node.children.size(); ++i)
            if (fixed_width(ast, node.children[i]) != width) return {};
        return width;
    }
    case Kind::Repeat: {
        if (node.upper && *node.upper == 0)
            return effect_free(ast, node.children.front()) ? std::optional<std::size_t>{0} : std::nullopt;
        const auto width = fixed_width(ast, node.children.front());
        if (!width) return {};
        if (*width == 0) return 0;
        if (!node.upper || node.lower != *node.upper) return {};
        return checked_multiply(node.lower, *width);
    }
    }
    return {};
}

std::optional<std::bitset<256>> byte_class(const Ast& ast, NodeId id) {
    const auto& node = ast.nodes[id];
    if (node.kind == Kind::CharSet) return node.characters;
    if (node.kind == Kind::Capture) return byte_class(ast, node.children.front());
    if (node.kind != Kind::Alt) return {};
    std::bitset<256> result;
    for (const auto child : node.children) {
        const auto part = byte_class(ast, child);
        if (!part) return {};
        result |= *part;
    }
    return result;
}

struct FragmentSpec {
    NodeId body;
    std::size_t width;
    bool shared;
};
struct Extractor {
    const Ast& ast;
    std::size_t offset = 0, coefficient = 0;
    bool bound = false;
    std::vector<FragmentSpec> fragments;

    bool visit(NodeId id) {
        const auto& node = ast.nodes[id];
        if (node.count_path != no_id) return false;
        if (node.kind == Kind::Capture) return visit(node.children.front());
        if (node.kind == Kind::Repeat && (node.bind_count != no_id || node.use_count != no_id)) {
            if (node.lower != 0 || node.upper) return false;
            const bool binding = node.bind_count != no_id;
            if ((binding ? node.bind_count : node.use_count) != 0 ||
                (binding ? bound : !bound)) return false;
            const auto body = node.children.front();
            const auto width = fixed_width(ast, body);
            if (!width || *width == 0) return false;
            const auto total = checked_add(coefficient, *width);
            if (!total) return false;
            coefficient = *total;
            bound = true;
            fragments.push_back({body, *width, true});
            return true;
        }
        // Keep a whole fixed-width regular fragment together, including a
        // pure alternation. Count effects inside an alternation decline below.
        if (const auto width = fixed_width(ast, id)) {
            const auto total = checked_add(offset, *width);
            if (!total) return false;
            offset = *total;
            fragments.push_back({id, *width, false});
            return true;
        }
        if (node.kind != Kind::Concat) return false;
        return std::all_of(node.children.begin(), node.children.end(),
            [&](NodeId child) { return visit(child); });
    }
};
}

struct CountPlan::Impl {
    struct Fragment {
        std::size_t width;
        bool shared;
        RegularFragment matcher;
    };
    std::size_t offset, coefficient;
    std::string name;
    std::vector<Fragment> fragments;

    Impl(const Ast& ast, const Extractor& extracted, RegularOptions options)
        : offset(extracted.offset), coefficient(extracted.coefficient), name(ast.count_names.front()) {
        for (const auto& spec : extracted.fragments) {
            auto characters = spec.width == 1 ? byte_class(ast, spec.body) : std::nullopt;
            auto matcher = characters ? RegularFragment::byte_set(*characters)
                                     : RegularFragment(ast, spec.body, options);
            fragments.push_back({spec.width, spec.shared, std::move(matcher)});
        }
    }
};

std::optional<CountPlan> make_count_plan(const Ast& ast, RegularOptions options) {
    if (ast.has_extended || ast.has_occurrence_paths || ast.has_calls || ast.has_references ||
        !ast.has_counts || ast.count_names.size() != 1) return {};
    Extractor extracted{ast, 0, 0, false, {}};
    if (!extracted.visit(ast.root) || !extracted.bound || extracted.coefficient == 0) return {};
    try {
        return CountPlan(std::make_shared<const CountPlan::Impl>(ast, extracted, options));
    } catch (const std::length_error&) {
        // A compile cap only disables this optimization; it is not a syntax
        // error and must not prevent the general state matcher from running.
        return {};
    }
}

bool CountPlan::match(std::string_view subject, CountStatistics* statistics) const {
    CountStatistics stats;
    const auto finish = [&](bool result) {
        if (statistics) *statistics = stats;
        return result;
    };
    if (subject.size() < impl_->offset ||
        (subject.size() - impl_->offset) % impl_->coefficient != 0) {
        stats.length_rejected = true;
        return finish(false);
    }
    const auto count = (subject.size() - impl_->offset) / impl_->coefficient;
    stats.count_value = count;
    stats.candidate_assignments = 1;
    std::size_t position = 0;
    for (const auto& fragment : impl_->fragments) {
        const auto repetitions = fragment.shared ? count : 1;
        // The solved length equation bounds this product by subject.size().
        // Adjacent occurrences share one direct scan instead of repeated DFA
        // calls. Correlated regular bodies retain their per-occurrence DFA.
        const auto length = repetitions * fragment.width;
        const auto accepted = fragment.matcher.matching_copies(
            subject.substr(position, length), fragment.width, repetitions);
        const auto attempted = accepted < repetitions ? accepted + 1 : repetitions;
        stats.body_matches += attempted;
        stats.transition_steps += attempted * fragment.width;
        if (accepted < repetitions) return finish(false);
        position += length;
    }
    return finish(position == subject.size());
}

std::string CountPlan::describe() const {
    std::ostringstream out;
    out << "exact shared-count plan: length = " << impl_->offset << " + " << impl_->coefficient
        << '*' << impl_->name << "; " << impl_->fragments.size()
        << " fixed-width fragments; one solved count, ordered Boolean matching";
    return out.str();
}
}
