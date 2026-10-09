#include "equality_matcher.hpp"
#include "fragment_matcher.hpp"

#include <algorithm>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace crank {
namespace {

constexpr auto maximum = std::numeric_limits<std::size_t>::max();

bool add_checked(std::size_t& total, std::size_t value) {
    if (value > maximum - total) return false;
    total += value;
    return true;
}

enum class SegmentKind { Fixed, Capture, Reference };
struct Segment {
    SegmentKind kind = SegmentKind::Fixed;
    std::size_t width = 0;
    std::optional<RegularFragment> program;
};

class Analyzer {
public:
    explicit Analyzer(const Ast& ast)
        : ast(ast), active(ast.nodes.size(), false), capture_occurrences(ast.capture_names.size()) {}

    bool inspect(NodeId id) {
        if (id >= ast.nodes.size() || active[id]) return false;
        active[id] = true;
        const auto& node = ast.nodes[id];
        if (node.bind_count != no_id || node.use_count != no_id || node.count_path != no_id)
            return false;
        switch (node.kind) {
        case Kind::Empty:
        case Kind::CharSet:
            if (!node.children.empty()) return false;
            break;
        case Kind::Capture:
            if (node.children.size() != 1 || node.capture_id == 0 ||
                node.capture_id >= capture_occurrences.size()) return false;
            // Saturation at two suffices to detect rebinding of the target.
            capture_occurrences[node.capture_id] =
                std::min<std::size_t>(2, capture_occurrences[node.capture_id] + 1);
            break;
        case Kind::Reference:
            if (!node.children.empty() || node.capture_id == 0 ||
                node.capture_id >= capture_occurrences.size()) return false;
            if (target != no_id && target != node.capture_id) return false;
            target = node.capture_id;
            if (!add_checked(references, 1)) return false;
            break;
        case Kind::Repeat:
            if (node.children.size() != 1 || (node.upper && *node.upper < node.lower)) return false;
            break;
        case Kind::Concat:
        case Kind::Alt:
            break;
        default:
            return false;
        }
        for (NodeId child : node.children)
            if (!inspect(child)) return false;
        active[id] = false;
        return true;
    }

    void flatten(NodeId id, std::vector<NodeId>& parts) const {
        const auto& node = ast.nodes[id];
        if (node.kind == Kind::Concat) {
            for (NodeId child : node.children) flatten(child, parts);
        } else if (node.kind == Kind::Capture && node.capture_id != target) {
            // Its outcome is unobserved by every reference in this expression.
            flatten(node.children.front(), parts);
        } else {
            parts.push_back(id);
        }
    }

    bool regular(NodeId id) const {
        const auto& node = ast.nodes[id];
        if (node.kind == Kind::Reference || node.kind == Kind::Call || node.kind == Kind::Define ||
            node.bind_count != no_id || node.use_count != no_id || node.count_path != no_id)
            return false;
        for (NodeId child : node.children)
            if (!regular(child)) return false;
        return true;
    }

    std::optional<std::size_t> fixed_width(NodeId id) const {
        const auto& node = ast.nodes[id];
        if (node.bind_count != no_id || node.use_count != no_id || node.count_path != no_id)
            return std::nullopt;
        switch (node.kind) {
        case Kind::Empty: return 0;
        case Kind::CharSet: return 1;
        case Kind::Reference:
        case Kind::Call:
        case Kind::Define: return std::nullopt;
        case Kind::Capture: return fixed_width(node.children.front());
        case Kind::Concat: {
            std::size_t result = 0;
            for (NodeId child : node.children) {
                const auto width = fixed_width(child);
                if (!width || !add_checked(result, *width)) return std::nullopt;
            }
            return result;
        }
        case Kind::Alt: {
            if (node.children.empty()) return 0; // Empty language, rejected by its DFA.
            const auto width = fixed_width(node.children.front());
            if (!width) return std::nullopt;
            for (NodeId child : node.children)
                if (fixed_width(child) != width) return std::nullopt;
            return width;
        }
        case Kind::Repeat: {
            const auto width = fixed_width(node.children.front());
            if (!width) return std::nullopt;
            if (*width == 0) return 0;
            if (!node.upper || *node.upper != node.lower || node.lower > maximum / *width)
                return std::nullopt;
            return node.lower * *width;
        }
        }
        return std::nullopt;
    }

    const Ast& ast;
    std::vector<bool> active;
    std::vector<std::size_t> capture_occurrences;
    std::size_t target = no_id;
    std::size_t references = 0;
};

RegularFragment compile_component(const Ast& ast, const std::vector<NodeId>& roots,
                                 RegularOptions options) {
    return RegularFragment(ast, roots, options);
}

} // namespace

struct EqualityPlan::Impl {
    std::vector<Segment> segments;
    std::size_t fixed_total = 0;
    std::size_t copies = 0;
    std::size_t capture_segment = no_id;
    std::size_t capture_id = no_id;
    std::string capture_name;
    RegularStatistics compiled;
};

EqualityPlan::EqualityPlan(std::shared_ptr<const Impl> implementation)
    : impl_(std::move(implementation)) {}

std::optional<EqualityPlan> make_equality_plan(const Ast& ast, RegularOptions options) {
    if (ast.has_extended || ast.has_occurrence_paths || ast.has_calls || ast.has_counts)
        return std::nullopt;
    Analyzer analyzer(ast);
    if (!analyzer.inspect(ast.root) || analyzer.target == no_id ||
        analyzer.references == 0 || analyzer.capture_occurrences[analyzer.target] != 1 ||
        analyzer.references == maximum) return std::nullopt;

    std::vector<NodeId> parts;
    analyzer.flatten(ast.root, parts);
    std::size_t capture_part = no_id;
    std::size_t flat_references = 0;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const auto& node = ast.nodes[parts[i]];
        if (node.kind == Kind::Capture && node.capture_id == analyzer.target) {
            if (capture_part != no_id || !analyzer.regular(node.children.front())) return std::nullopt;
            capture_part = i;
        } else if (node.kind == Kind::Reference) {
            ++flat_references;
        } else if (!analyzer.fixed_width(parts[i])) {
            return std::nullopt;
        }
    }
    if (capture_part == no_id || flat_references != analyzer.references) return std::nullopt;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const auto& node = ast.nodes[parts[i]];
        // A forward obligation must be discharged by the one later capture;
        // an ordinary reference must have that capture already available.
        if (node.kind == Kind::Reference && node.forward != (i < capture_part)) return std::nullopt;
    }

    auto implementation = std::make_shared<EqualityPlan::Impl>();
    implementation->copies = analyzer.references + 1;
    implementation->capture_id = analyzer.target;
    implementation->capture_name = ast.capture_names[analyzer.target];
    std::vector<NodeId> fixed_roots;
    std::size_t fixed_width = 0;
    auto append_compiled = [&](SegmentKind kind, std::size_t width,
                               const std::vector<NodeId>& roots) -> bool {
        auto program = compile_component(ast, roots, options);
        const auto stats = program.compiled_statistics();
        if (!add_checked(implementation->compiled.nfa_states, stats.nfa_states) ||
            !add_checked(implementation->compiled.dfa_states, stats.dfa_states) ||
            implementation->compiled.nfa_states > options.max_nfa_states ||
            implementation->compiled.dfa_states > options.max_dfa_states) return false;
        implementation->segments.push_back({kind, width, std::move(program)});
        return true;
    };
    auto flush_fixed = [&]() -> bool {
        if (fixed_roots.empty()) return true;
        if (!add_checked(implementation->fixed_total, fixed_width) ||
            !append_compiled(SegmentKind::Fixed, fixed_width, fixed_roots)) return false;
        fixed_roots.clear();
        fixed_width = 0;
        return true;
    };

    try {
        for (std::size_t i = 0; i < parts.size(); ++i) {
            const auto& node = ast.nodes[parts[i]];
            if (i == capture_part) {
                if (!flush_fixed()) return std::nullopt;
                implementation->capture_segment = implementation->segments.size();
                if (!append_compiled(SegmentKind::Capture, 0, node.children)) return std::nullopt;
            } else if (node.kind == Kind::Reference) {
                if (!flush_fixed()) return std::nullopt;
                implementation->segments.push_back({SegmentKind::Reference, 0, std::nullopt});
            } else {
                const auto width = analyzer.fixed_width(parts[i]);
                if (!width || !add_checked(fixed_width, *width)) return std::nullopt;
                fixed_roots.push_back(parts[i]);
            }
        }
        if (!flush_fixed()) return std::nullopt;
    } catch (const std::length_error&) {
        return std::nullopt;
    }
    return EqualityPlan(std::move(implementation));
}

bool EqualityPlan::match(std::string_view subject, RegularStatistics* statistics) const {
    RegularStatistics stats = impl_->compiled;
    if (statistics) *statistics = stats;
    if (subject.size() < impl_->fixed_total) return false;
    const std::size_t variable_total = subject.size() - impl_->fixed_total;
    if (variable_total % impl_->copies != 0) return false;
    const std::size_t width = variable_total / impl_->copies;

    std::size_t capture_begin = 0;
    for (std::size_t i = 0; i < impl_->capture_segment; ++i) {
        const auto& segment = impl_->segments[i];
        capture_begin += segment.kind == SegmentKind::Fixed ? segment.width : width;
    }
    const auto captured = subject.substr(capture_begin, width);
    std::size_t position = 0;
    for (const auto& segment : impl_->segments) {
        const std::size_t length = segment.kind == SegmentKind::Fixed ? segment.width : width;
        const auto piece = subject.substr(position, length);
        bool accepted;
        if (segment.kind == SegmentKind::Reference) {
            accepted = piece == captured;
        } else {
            RegularStatistics component;
            accepted = segment.program->full_match(piece, &component);
            stats.transition_steps += component.transition_steps;
        }
        if (statistics) *statistics = stats;
        if (!accepted) return false;
        position += length;
    }
    return position == subject.size();
}

std::string EqualityPlan::describe() const {
    std::ostringstream result;
    result << "Equality plan: N = " << impl_->fixed_total << " + " << impl_->copies
           << "*|capture ";
    if (impl_->capture_name.empty()) result << '#' << impl_->capture_id;
    else result << impl_->capture_name;
    result << "|; fixed-width surroundings, regular capture validation, exact byte equality"
              "; Boolean full matching only";
    return result.str();
}

} // namespace crank
