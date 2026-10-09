#pragma once
#include "occurrence_analysis.hpp"
#include "source_portfolio_statistics.hpp"
#include <algorithm>
#include <array>
#include <memory>
#include <stdexcept>

namespace crank::detail {

// An immutable, bounded internal Boolean source view. The admitted transformations
// preserve the least fixed-point root language, not capture/history structure.
// No representation builder or executor participates in view construction.
class BooleanSourceView {
public:
    static constexpr std::size_t version = 2;
    struct Options {
        bool repetition_quotient = true, transparent_calls = true;
        bool identity_before_repetition = false; // isolated composition control
        bool arithmetic_chains = false;
    };
    using Preparation = SourceViewPreparationStatistics;
    struct Facts {
        struct Boundary { bool nullable = false; std::bitset<256> first; };
        struct Length { bool productive = false; std::size_t minimum = no_id; };
        std::vector<Boundary> boundaries;
        std::vector<Length> lengths;
        bool nullable_complete = false, first_complete = false;
        bool productive_complete = false, minimum_complete = false;
        const Boundary& boundary(NodeId id) const {
            if (!nullable_complete || !first_complete) throw std::logic_error("incomplete source-view boundary facts");
            return boundaries.at(id);
        }
        const Length& length(NodeId id) const {
            if (!productive_complete || !minimum_complete) throw std::logic_error("incomplete source-view length facts");
            return lengths.at(id);
        }
    };
private:
    static constexpr std::size_t max_nodes = 1024, max_edges = 8192;
    static constexpr std::size_t max_work = 131072, max_metadata_bytes = 65536;
    struct Limit {};
    struct Decline {};
    Ast normalized_;
    Facts facts_;
    std::vector<NodeId> provenance_;
    Preparation statistics_;
    std::size_t work_cap_ = max_work;
    void charge(std::size_t n = 1, bool nullable = false) {
        const auto used = statistics_.normalization_steps + statistics_.nullable_steps;
        if (n > work_cap_ - used) throw Limit{};
        (nullable ? statistics_.nullable_steps : statistics_.normalization_steps) += n;
    }
    void validate(const Ast& ast) {
        if (ast.nodes.size() > max_nodes / 2) throw Limit{};
        if (ast.root >= ast.nodes.size()) throw Decline{};
        std::size_t edges = 0, metadata = 0;
        const auto metadata_bytes = [&](std::size_t count) {
            if (count > max_metadata_bytes - metadata) throw Limit{};
            metadata += count; charge();
        };
        for (const auto& name : ast.capture_names) metadata_bytes(name.size() + sizeof(std::string));
        for (const auto& name : ast.count_names) metadata_bytes(name.size() + sizeof(std::string));
        if (ast.capture_nodes.size() > max_metadata_bytes / sizeof(NodeId) || !ast.count_nodes.empty() || !ast.count_paths.empty()) throw Decline{};
        metadata_bytes(ast.capture_nodes.size() * sizeof(NodeId));
        for (const auto& node : ast.nodes) {
            charge();
            if (node.children.size() > max_edges - edges) throw Limit{};
            edges += node.children.size(); charge(node.children.size());
            switch (node.kind) {
            case Kind::Empty: case Kind::CharSet: case Kind::Concat: case Kind::Alt:
            case Kind::Repeat: case Kind::Capture: case Kind::Call: case Kind::Define: break;
            default: throw Decline{};
            }
            // Public summary flags cannot override actual effects, including
            // effects on unreachable nodes. Ordinary captures are unobserved
            // only because the owner admitted the pure Boolean contract.
            if (node.bind_count != no_id || node.use_count != no_id || node.count_path != no_id || node.forward) throw Decline{};
            if ((node.kind == Kind::Capture || node.kind == Kind::Repeat) && node.children.size() != 1) throw Decline{};
            if (node.kind == Kind::Repeat && node.upper && *node.upper < node.lower) throw Decline{};
            if (node.kind == Kind::Capture && (node.capture_id == no_id || node.capture_id >= ast.capture_names.size())) throw Decline{};
            if (node.kind == Kind::Call && (node.call_target >= ast.nodes.size() ||
                (node.call_target != ast.root && ast.nodes[node.call_target].kind != Kind::Capture))) throw Decline{};
            for (auto child : node.children) if (child >= ast.nodes.size()) throw Decline{};
        }
    }
    // Transparent chains are functional. A cycle is retained unchanged, not
    // replaced by epsilon or claimed to denote an empty language.
    NodeId transparent(NodeId initial) {
        auto id = initial;
        for (std::size_t depth = 0; depth < normalized_.nodes.size(); ++depth) {
            charge(); const auto& node = normalized_.nodes[id];
            if (node.kind == Kind::Call) id = node.call_target;
            else if (node.kind == Kind::Capture ||
                     ((node.kind == Kind::Concat || node.kind == Kind::Alt) && node.children.size() == 1)) id = node.children.front();
            else return id;
        }
        return initial;
    }
    void repetition_quotient() {
        const auto source_size = normalized_.nodes.size();
        for (NodeId id = 0; id < source_size; ++id) {
            charge();
            const auto& outer = normalized_.nodes[id];
            if (outer.kind != Kind::Repeat || outer.lower != 0 || outer.upper) continue;
            auto body = transparent(outer.children.front());
            bool changed = false;
            std::vector<bool> seen(normalized_.nodes.size());
            // Nested-star/plus flattening uses the same Boolean language law.
            for (;;) {
                if (seen[body]) break;
                seen[body] = true;
                const auto& node = normalized_.nodes[body];
                if (node.kind != Kind::Repeat || node.upper || node.lower > 1) break;
                const auto next = transparent(node.children.front());
                if (next == body || seen[next]) break;
                body = next; changed = true; ++statistics_.repetition_rewrites;
            }
            if (normalized_.nodes[body].kind == Kind::Alt) {
                // Copy only this star's body occurrence. A shared alternative
                // or called B+ rule may still be used outside the star where
                // replacing it by B would change the language.
                Node replacement = normalized_.nodes[body];
                for (auto& child : replacement.children) {
                    auto candidate = transparent(child);
                    std::fill(seen.begin(), seen.end(), false);
                    while (!seen[candidate]) {
                        seen[candidate] = true;
                        const auto& node = normalized_.nodes[candidate];
                        if (node.kind != Kind::Repeat || node.upper || node.lower > 1) break;
                        const auto next = transparent(node.children.front());
                        if (next == candidate || seen[next]) break;
                        candidate = next; child = candidate; changed = true;
                        ++statistics_.repetition_rewrites;
                    }
                }
                if (changed) {
                    charge(replacement.children.size() + 1);
                    if (normalized_.nodes.size() == max_nodes) throw Limit{};
                    const auto origin = provenance_[body];
                    body = normalized_.nodes.size(); normalized_.nodes.push_back(std::move(replacement));
                    provenance_.push_back(origin);
                    ++statistics_.cloned_nodes;
                }
            }
            if (changed) normalized_.nodes[id].children.front() = body;
        }
    }
    void redirect_calls() {
        for (auto& node : normalized_.nodes) {
            charge(); if (node.kind != Kind::Call) continue;
            const auto target = transparent(node.call_target);
            if (target != node.call_target) { node.call_target = target; ++statistics_.redirected_calls; }
        }
    }
    // Boolean language law E = E O E | open E close | I becomes
    // E = A (O A)*, A = open E close | I. Retain every old node: outside
    // uses remain language-correct, but can still prevent predictor admission.
    // Capture identities survive; capture histories and witnesses do not.
    void arithmetic_chains() {
        const auto source_size = normalized_.nodes.size();
        for (NodeId expression = 0; expression < source_size; ++expression) {
            charge();
            const auto& capture = normalized_.nodes[expression];
            if (capture.kind != Kind::Capture) continue;
            const auto body = capture.children.front();
            charge();
            const auto& alternative = normalized_.nodes[body];
            if (alternative.kind != Kind::Alt || alternative.children.size() != 4) continue;
            std::array<NodeId, 2> binaries{no_id, no_id}, operators{no_id, no_id};
            NodeId bracket = no_id, identifier = no_id, identifier_class = no_id;
            NodeId open = no_id, close = no_id;
            std::size_t binary_count = 0;
            bool matches = true;
            const auto operand = [&](NodeId id) {
                charge(); const auto& node = normalized_.nodes[id];
                return node.kind == Kind::Call && node.children.empty() && node.call_target == expression;
            };
            const auto singleton = [&](NodeId id) {
                charge(); const auto& node = normalized_.nodes[id];
                return node.kind == Kind::CharSet && node.children.empty() && node.characters.count() == 1;
            };
            for (auto branch : alternative.children) {
                charge(); const auto& node = normalized_.nodes[branch];
                if (node.kind == Kind::Concat && node.children.size() == 3) {
                    charge(3);
                    const auto left = node.children[0], middle = node.children[1], right = node.children[2];
                    if (operand(left) && singleton(middle) && operand(right)) {
                        if (binary_count == binaries.size()) { matches = false; break; }
                        binaries[binary_count] = branch; operators[binary_count++] = middle;
                    } else if (singleton(left) && operand(middle) && singleton(right)) {
                        if (bracket != no_id) { matches = false; break; }
                        bracket = branch; open = left; close = right;
                    } else { matches = false; break; }
                } else if (node.kind == Kind::Repeat && node.lower == 1 && !node.upper) {
                    // validate() already checked Repeat arity. Its identifier
                    // must be nonempty and consume precisely one byte per item.
                    charge(); const auto child = node.children.front();
                    const auto& character = normalized_.nodes[child];
                    if (identifier != no_id || character.kind != Kind::CharSet ||
                        !character.children.empty() || character.characters.none()) { matches = false; break; }
                    identifier = branch; identifier_class = child;
                } else { matches = false; break; }
            }
            if (!matches || binary_count != 2 || bracket == no_id || identifier == no_id) continue;
            charge(5);
            const auto punctuation = normalized_.nodes[operators[0]].characters |
                normalized_.nodes[operators[1]].characters | normalized_.nodes[open].characters |
                normalized_.nodes[close].characters;
            if (punctuation.count() != 4 ||
                (punctuation & normalized_.nodes[identifier_class].characters).any()) continue;

            // Preflight total stored nodes/child edges, including unreachable
            // old definitions. This law is not composed with other rewrites.
            charge();
            if (source_size > max_nodes - 5) throw Limit{};
            std::size_t edges = 0;
            for (const auto& node : normalized_.nodes) {
                charge(1 + node.children.size());
                if (node.children.size() > max_edges - edges) throw Limit{};
                edges += node.children.size();
            }
            if (edges > max_edges - 9) throw Limit{};
            // Five helper nodes, nine child edges, five attribution entries,
            // and the one replaced edge all consume normalization work.
            charge(5 + 9 + 5 + 1);
            normalized_.nodes.reserve(source_size + 5);
            provenance_.reserve(source_size + 5);
            const auto append = [&](Kind kind, std::initializer_list<NodeId> children, NodeId origin) {
                Node node; node.kind = kind; node.children = children;
                const auto id = normalized_.nodes.size();
                normalized_.nodes.push_back(std::move(node)); provenance_.push_back(origin);
                return id;
            };
            // Version 2 provenance is construction-site attribution only.
            // Helpers do not inherit the anchor's facts or observer witnesses.
            const auto atom = append(Kind::Alt, {bracket, identifier}, body);
            const auto operation = append(Kind::Alt, {operators[0], operators[1]}, body);
            const auto suffix = append(Kind::Concat, {operation, atom}, binaries[0]);
            const auto tail = append(Kind::Repeat, {suffix}, body);
            const auto chain = append(Kind::Concat, {atom, tail}, body);
            normalized_.nodes[expression].children.front() = chain;
            ++statistics_.arithmetic_chain_rewrites;
            statistics_.cloned_nodes += 5;
            return; // Deterministically rewrite only the first eligible capture.
        }
    }
    // All facts start at bottom; complete facts are published together only
    // after convergence. FIRST requires productive suffixes, so empty-language
    // children do not leak spurious terminals into choices. Saturated minima
    // remain lower bounds and never stand in for the productivity flag.
    void analyze() {
        const auto n = normalized_.nodes.size();
        facts_.boundaries.resize(n); facts_.lengths.resize(n);
        const auto add = [](std::size_t a, std::size_t b) {
            return a > no_id - b ? no_id : a + b;
        };
        const auto multiply = [](std::size_t a, std::size_t b) {
            return b && a > no_id / b ? no_id : a * b;
        };
        bool changed = true;
        while (changed) {
            changed = false;
            for (NodeId id = 0; id < n; ++id) {
                charge(1, true); const auto& node = normalized_.nodes[id];
                Facts::Boundary boundary; Facts::Length length;
                const auto copy = [&](NodeId child) {
                    boundary = facts_.boundaries[child]; length = facts_.lengths[child];
                };
                switch (node.kind) {
                case Kind::Empty: case Kind::Define:
                    boundary.nullable = length.productive = true; length.minimum = 0; break;
                case Kind::CharSet:
                    boundary.first = node.characters; length.productive = node.characters.any();
                    if (length.productive) length.minimum = 1;
                    break;
                case Kind::Capture: copy(node.children.front()); break;
                case Kind::Call: copy(node.call_target); break;
                case Kind::Repeat: {
                    const auto child = node.children.front();
                    if (node.lower == 0) {
                        length.productive = boundary.nullable = true; length.minimum = 0;
                    } else if (facts_.lengths[child].productive) {
                        length.productive = true;
                        length.minimum = multiply(facts_.lengths[child].minimum, node.lower);
                        boundary.nullable = facts_.boundaries[child].nullable;
                    }
                    if ((!node.upper || *node.upper) && facts_.lengths[child].productive)
                        boundary.first = facts_.boundaries[child].first;
                    break;
                }
                case Kind::Alt:
                    for (auto child : node.children) {
                        charge(1, true);
                        boundary.nullable |= facts_.boundaries[child].nullable;
                        boundary.first |= facts_.boundaries[child].first;
                        length.productive |= facts_.lengths[child].productive;
                        length.minimum = std::min(length.minimum, facts_.lengths[child].minimum);
                    }
                    break;
                case Kind::Concat:
                    boundary.nullable = length.productive = true; length.minimum = 0;
                    for (auto child : node.children) {
                        charge(1, true);
                        boundary.nullable &= facts_.boundaries[child].nullable;
                        length.productive &= facts_.lengths[child].productive;
                        length.minimum = add(length.minimum, facts_.lengths[child].minimum);
                    }
                    if (length.productive) {
                        for (auto child : node.children) {
                            charge(1, true); boundary.first |= facts_.boundaries[child].first;
                            if (!facts_.boundaries[child].nullable) break;
                        }
                    } else length.minimum = no_id;
                    break;
                default: throw Decline{};
                }
                auto& old_boundary = facts_.boundaries[id]; auto& old_length = facts_.lengths[id];
                if (boundary.nullable != old_boundary.nullable || boundary.first != old_boundary.first ||
                    length.productive != old_length.productive || length.minimum != old_length.minimum) {
                    old_boundary = boundary; old_length = length; changed = true;
                }
            }
        }
        for (NodeId id = 0; id < n; ++id) normalized_.nodes[id].nullable = facts_.boundaries[id].nullable;
        facts_.nullable_complete = facts_.first_complete = facts_.productive_complete = facts_.minimum_complete = true;
        statistics_.nullable_complete = statistics_.first_complete = true;
        statistics_.productive_complete = statistics_.minimum_complete = true;
    }
    void build(const Ast& source, Options options) {
        statistics_.source_nodes = source.nodes.size(); validate(source);
        if (options.arithmetic_chains && (options.repetition_quotient || options.transparent_calls ||
            options.identity_before_repetition)) throw Decline{};
        charge(source.nodes.size());
        for (const auto& node : source.nodes) charge(node.children.size());
        normalized_ = source;
        normalized_.has_references = normalized_.has_counts = normalized_.has_extended = false;
        normalized_.has_occurrence_paths = false;
        provenance_.resize(source.nodes.size());
        if (options.arithmetic_chains) charge(source.nodes.size()); // retained identity attribution
        for (NodeId id = 0; id < source.nodes.size(); ++id) provenance_[id] = id;
        if (options.arithmetic_chains) arithmetic_chains();
        if (options.identity_before_repetition && options.transparent_calls) redirect_calls();
        if (options.repetition_quotient) repetition_quotient();
        if (!options.identity_before_repetition && options.transparent_calls) redirect_calls();
        statistics_.normalized_nodes = normalized_.nodes.size();
        statistics_.changed = statistics_.repetition_rewrites || statistics_.redirected_calls ||
            statistics_.arithmetic_chain_rewrites;
        // View zero is an explicit research original-source candidate. Other
        // masks with no applicable law remain unavailable, avoiding duplicates.
        if (!statistics_.changed && (options.repetition_quotient || options.transparent_calls || options.arithmetic_chains)) {
            statistics_.status = SourcePreparationStatus::Unchanged; return;
        }
        analyze(); statistics_.status = SourcePreparationStatus::Ready;
    }
public:
    explicit BooleanSourceView(const Ast& source) : BooleanSourceView(source, Options{}) {}
    BooleanSourceView(const Ast& source, Options options, std::size_t work_cap = max_work)
        : work_cap_(std::min(work_cap, max_work)) {
        statistics_.attempted = true;
        try { build(source, options); }
        catch (const Decline&) { statistics_.status = SourcePreparationStatus::Inapplicable; }
        catch (const Limit&) { statistics_.limited = true; statistics_.status = SourcePreparationStatus::PreparationLimit; }
        catch (const std::bad_alloc&) { statistics_.limited = true; statistics_.status = SourcePreparationStatus::AllocationFailure; }
        statistics_.retained_bytes = retained_bytes();
    }
    [[nodiscard]] bool ready() const { return statistics_.status == SourcePreparationStatus::Ready; }
    [[nodiscard]] const Preparation& statistics() const { return statistics_; }
    // Internal compiler graph: its call targets need not be Capture nodes.
    // Never pass this graph through public checked_ast or observed APIs.
    [[nodiscard]] const Ast& ast() const { return normalized_; }
    [[nodiscard]] const Facts& analysis() const {
        if (!ready()) throw std::logic_error("source view unavailable");
        return facts_;
    }
    [[nodiscard]] const std::vector<NodeId>& provenance() const { return provenance_; }
    [[nodiscard]] std::size_t compilation_steps() const { return statistics_.normalization_steps + statistics_.nullable_steps; }
    [[nodiscard]] std::size_t retained_bytes() const {
        std::size_t bytes = sizeof(*this) + normalized_.nodes.capacity() * sizeof(Node);
        for (const auto& node : normalized_.nodes) bytes += node.children.capacity() * sizeof(NodeId);
        bytes += normalized_.capture_nodes.capacity() * sizeof(NodeId) + normalized_.capture_names.capacity() * sizeof(std::string);
        for (const auto& name : normalized_.capture_names) bytes += name.capacity();
        bytes += normalized_.count_names.capacity() * sizeof(std::string);
        for (const auto& name : normalized_.count_names) bytes += name.capacity();
        bytes += provenance_.capacity() * sizeof(NodeId) + facts_.boundaries.capacity() * sizeof(Facts::Boundary) + facts_.lengths.capacity() * sizeof(Facts::Length);
        return bytes;
    }
};
} // namespace crank::detail
