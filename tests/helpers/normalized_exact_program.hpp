#pragma once
#include "deterministic_program.hpp"

namespace crank::detail {

// A bounded retry for an already-validated, closed pure Boolean source whose
// original SLR candidate declined. The enclosing matcher owns the observation
// contract. Neither transformation is valid for capture/history/count output.
// This class retains the original node identities and an internal transformed
// copy; it never mutates the caller's AST. Its output is an ordinary existing
// SLR certificate, so its executor and every physical/native lowering reuse
// the same deterministic-program proof and resource callbacks.
class NormalizedExactProgram {
public:
    struct Options { bool repetition_quotient = true, transparent_calls = true; };
    using Preparation = NormalizedProgramPreparationStatistics;
private:
    static constexpr std::size_t max_nodes = 1024, max_edges = 8192;
    static constexpr std::size_t max_work = 131072, max_metadata_bytes = 65536;
    struct Limit {};
    struct Decline {};
    Ast normalized_;
    std::unique_ptr<DeterministicProgram> certificate_;
    Preparation statistics_;
    void charge(std::size_t n = 1, bool nullable = false) {
        const auto used = statistics_.normalization_steps + statistics_.nullable_steps;
        if (n > max_work - used) throw Limit{};
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
                    body = normalized_.nodes.size(); normalized_.nodes.push_back(std::move(replacement));
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
    // SLR admission only consumes nullable facts. Computing these directly
    // avoids repeating the unrelated length/congruence/residue analyses.
    // Iteration starts at false and reaches the exact least fixed point. An
    // exhausted budget declines the retry; an incomplete false fact is never
    // passed to the deterministic compiler as a nonnullability proof.
    struct NullableFacts {
        struct Boundary { bool nullable = false; };
        std::vector<Boundary> facts;
        const Boundary& boundary(NodeId id) const { return facts.at(id); }
    };
    NullableFacts nullability() {
        NullableFacts result; result.facts.resize(normalized_.nodes.size());
        bool changed = true;
        while (changed) {
            changed = false;
            for (NodeId id = 0; id < normalized_.nodes.size(); ++id) {
                charge(1, true); const auto& node = normalized_.nodes[id]; bool nullable = false;
                switch (node.kind) {
                case Kind::Empty: case Kind::Define: nullable = true; break;
                case Kind::CharSet: break;
                case Kind::Capture: nullable = result.facts[node.children.front()].nullable; break;
                case Kind::Call: nullable = result.facts[node.call_target].nullable; break;
                case Kind::Repeat: nullable = node.lower == 0 || result.facts[node.children.front()].nullable; break;
                case Kind::Alt:
                    for (auto child : node.children) { charge(1, true); nullable |= result.facts[child].nullable; }
                    break;
                case Kind::Concat:
                    nullable = true;
                    for (auto child : node.children) { charge(1, true); nullable &= result.facts[child].nullable; }
                    break;
                default: throw Decline{};
                }
                if (nullable && !result.facts[id].nullable) { result.facts[id].nullable = true; changed = true; }
            }
        }
        for (NodeId id = 0; id < normalized_.nodes.size(); ++id)
            normalized_.nodes[id].nullable = result.facts[id].nullable;
        statistics_.nullable_complete = true;
        return result;
    }
    void build(const Ast& source, Options options) {
        statistics_.source_nodes = source.nodes.size(); validate(source);
        // Charge source copying, including all edge arrays, before allocation.
        charge(source.nodes.size());
        for (const auto& node : source.nodes) charge(node.children.size());
        normalized_ = source;
        if (options.repetition_quotient) repetition_quotient();
        if (options.transparent_calls) redirect_calls();
        statistics_.normalized_nodes = normalized_.nodes.size();
        statistics_.changed = statistics_.repetition_rewrites || statistics_.redirected_calls;
        if (!statistics_.changed) return;
        auto analysis = nullability();
        certificate_ = std::make_unique<DeterministicProgram>(normalized_, analysis);
        statistics_.slr = certificate_->statistics();
        statistics_.certified = certificate_->certified();
        statistics_.limited |= statistics_.slr.limited;
    }
public:
    explicit NormalizedExactProgram(const Ast& source) : NormalizedExactProgram(source, Options{}) {}
    NormalizedExactProgram(const Ast& source, Options options) {
        statistics_.attempted = true;
        try { build(source, options); }
        catch (const Decline&) {}
        catch (const Limit&) { statistics_.limited = true; }
        catch (const std::bad_alloc&) { statistics_.limited = true; }
        statistics_.retained_bytes = retained_bytes();
    }
    [[nodiscard]] bool ready() const { return statistics_.certified; }
    [[nodiscard]] const Preparation& statistics() const { return statistics_; }
    [[nodiscard]] const DeterministicProgram* certificate() const { return ready() ? certificate_.get() : nullptr; }
    // This copy is for compiler diagnostics/research. After transparent-call
    // quotienting, call targets may be non-Capture nodes; it is not a public
    // parser AST to feed back through checked_ast validation.
    [[nodiscard]] const Ast& normalized_ast() const { return normalized_; }
    [[nodiscard]] std::size_t compilation_steps() const {
        return statistics_.normalization_steps + statistics_.nullable_steps + statistics_.slr.analysis_steps;
    }
    [[nodiscard]] std::size_t code_bytes() const {
        return certificate_ ? certificate_->statistics().retained_bytes : 0;
    }
    [[nodiscard]] std::size_t retained_bytes() const {
        std::size_t bytes = sizeof(*this) + code_bytes() + normalized_.nodes.capacity() * sizeof(Node);
        for (const auto& node : normalized_.nodes) bytes += node.children.capacity() * sizeof(NodeId);
        bytes += normalized_.capture_nodes.capacity() * sizeof(NodeId) + normalized_.capture_names.capacity() * sizeof(std::string);
        for (const auto& name : normalized_.capture_names) bytes += name.capacity();
        bytes += normalized_.count_names.capacity() * sizeof(std::string);
        for (const auto& name : normalized_.count_names) bytes += name.capacity();
        return bytes;
    }
};
} // namespace crank::detail
