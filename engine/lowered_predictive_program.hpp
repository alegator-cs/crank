#pragma once
#include "predictive_regions.hpp"
#include "bounded_word_regions.hpp"
#include <limits>

namespace crank::detail {

// The proof remains in PredictiveRegion / BoundedWordRegion. This lowering
// copies their already resolved choices into a source-independent executable
// layout. It neither admits a new grammar nor changes interruption boundaries.
// Programs are owned by the same immutable AstMachineProgram as certificates.
class PredictiveProgramLayout {
protected:
    using Index = std::uint16_t;
    static constexpr Index absent = 65535, secondary = 32768;
    enum class Op : std::uint8_t { Empty, Byte, Choice, Sequence, Repeat };
    struct Instruction {
        std::bitset<256> mask;
        std::size_t lower = 0, upper = no_id;
        std::uint32_t children = 0;
        Index count = 0, decision = absent;
        Op op = Op::Empty;
        // A successful repeat FIRST guard proves the entire one-byte body.
        // Established only after every target instruction has been emitted.
        bool repeated_byte = false;
    };
    using Decision = std::array<Index, 257>;
    std::vector<Instruction> code_;
    std::vector<Index> children_;
    std::vector<Decision> decisions_;
    Index entry_ = absent;
    std::size_t work_ = 0;
    struct Declined {};

    // Construction is bounded independently of query resources. Both callers
    // have stronger certificate caps, but enforce the cap here as well.
    std::vector<NodeId> sources(const Ast& ast, const std::vector<NodeId>& targets,
            NodeId entry, std::size_t root_children, std::vector<Index>& map) {
        if (ast.nodes.size() > 4096 || targets.size() != ast.nodes.size() || entry >= targets.size()) throw Declined{};
        std::vector<NodeId> result, pending{entry}; map.assign(ast.nodes.size(), absent);
        std::size_t edge_count = 0;
        while (!pending.empty()) {
            ++work_; const auto source = pending.back(); pending.pop_back();
            if (source >= targets.size()) throw Declined{};
            const auto id = targets[source];
            if (id >= ast.nodes.size()) throw Declined{};
            if (map[id] != absent) continue;
            map[id] = static_cast<Index>(result.size()); result.push_back(id);
            const auto& node = ast.nodes[id];
            if (node.bind_count != no_id || node.use_count != no_id || node.count_path != no_id) throw Declined{};
            if (node.children.size() > 16384 - edge_count) throw Declined{};
            edge_count += node.children.size();
            if (node.kind == Kind::Repeat && (node.children.size() != 1 || (node.upper && *node.upper < node.lower))) throw Declined{};
            if (node.kind == Kind::Alt || node.kind == Kind::Concat ||
                (node.kind == Kind::Repeat && (!node.upper || *node.upper))) {
                const auto count = id == entry && root_children != no_id ? root_children : node.children.size();
                if (count > node.children.size()) throw Declined{};
                for (std::size_t i = 0; i < count; ++i) { ++work_; pending.push_back(node.children[i]); }
            } else if (node.kind != Kind::Empty && node.kind != Kind::Define &&
                       node.kind != Kind::CharSet && node.kind != Kind::Repeat) throw Declined{};
        }
        code_.resize(result.size());
        entry_ = map[targets[entry]];
        return result;
    }
    static Index target(NodeId id, const std::vector<NodeId>& targets, const std::vector<Index>& map) {
        if (id == no_id) return absent;
        if (id >= targets.size() || targets[id] >= map.size() || map[targets[id]] == absent) throw Declined{};
        return map[targets[id]];
    }
    void instruction(const Ast& ast, NodeId id, Index pc, NodeId entry,
            std::size_t root_children, const std::vector<NodeId>& targets,
            const std::vector<Index>& map) {
        ++work_; auto& output = code_[pc]; const auto& node = ast.nodes[id];
        switch (node.kind) {
        case Kind::Empty: case Kind::Define: output.op = Op::Empty; break;
        case Kind::CharSet: output.op = Op::Byte; output.mask = node.characters; break;
        case Kind::Alt: output.op = Op::Choice; break;
        case Kind::Concat: {
            output.op = Op::Sequence;
            const auto count = id == entry && root_children != no_id ? root_children : node.children.size();
            if (count > 16384 || children_.size() > 16384 - count) throw Declined{};
            output.children = static_cast<std::uint32_t>(children_.size()); output.count = static_cast<Index>(count);
            for (std::size_t i = 0; i < count; ++i) children_.push_back(target(node.children[i], targets, map));
            break;
        }
        case Kind::Repeat:
            output.op = Op::Repeat; output.lower = node.lower; output.upper = node.upper.value_or(no_id);
            if (!node.upper || *node.upper) {
                output.children = target(node.children.front(), targets, map);
                output.count = 1;
            }
            break;
        default: throw Declined{};
        }
    }
    void clear() {
        std::vector<Instruction>().swap(code_); std::vector<Index>().swap(children_);
        std::vector<Decision>().swap(decisions_); entry_ = absent;
    }
    void compact() { code_.shrink_to_fit(); children_.shrink_to_fit(); decisions_.shrink_to_fit(); }
    void span(std::string_view input, std::size_t begin, std::size_t end) const {
        if (!certified) throw std::logic_error("uncertified lowered predictor");
        if (begin > end || end > input.size()) throw std::out_of_range("lowered predictive demand");
    }
public:
    static constexpr unsigned compiler_version = 2;
    bool certified = false, limit_reached = false;
    std::size_t source_nodes = 0;
    [[nodiscard]] bool ready() const { return certified; }
    [[nodiscard]] std::size_t compilation_steps() const { return work_; }
    [[nodiscard]] std::size_t code_bytes() const { return payload_bytes(); }
    [[nodiscard]] std::size_t instructions() const { return code_.size(); }
    [[nodiscard]] std::size_t decision_rows() const { return decisions_.size(); }
    [[nodiscard]] std::size_t payload_bytes() const {
        return code_.capacity() * sizeof(Instruction) + children_.capacity() * sizeof(Index) + decisions_.capacity() * sizeof(Decision);
    }
    [[nodiscard]] std::size_t retained_bytes() const { return sizeof(*this) + payload_bytes(); }
};

class LoweredPredictiveProgram final : public PredictiveProgramLayout {
public:
    // Counts preserve the old physical representation: logical frames can be
    // coalesced into counted runs. Tick and batch callbacks perform metering
    // and cancellation; runtime records only callbacks that completed.
    struct Runtime {
        std::size_t steps = 0, frames = 0, maximum_frames = 0;
        std::size_t frame_runs = 0, maximum_frame_runs = 0, terminal_run_bytes = 0;
        // Populated only by execute<true>; no hot counter cost by default.
        std::size_t dispatches = 0, byte_tests = 0, decisions = 0, continuations = 0;
        std::size_t reused_byte_checks = 0;
    };
    template<class Analysis>
    LoweredPredictiveProgram(const Ast& ast, const Analysis& analysis, const PredictiveRegion& certificate) {
        if (!certificate.certified) return;
        source_nodes = ast.nodes.size();
        try {
            std::vector<Index> map;
            const auto source = sources(ast, certificate.targets, certificate.entry, certificate.root_child_count, map);
            for (const auto id : source) {
                const auto pc = map[id]; instruction(ast, id, pc, certificate.entry, certificate.root_child_count, certificate.targets, map);
                auto& output = code_[pc];
                if (output.op == Op::Choice) {
                    if (id >= certificate.choice_index.size() || certificate.choice_index[id] >= certificate.choices.size()) throw Declined{};
                    const auto& choice = certificate.choices[certificate.choice_index[id]];
                    if (decisions_.size() >= 256) throw Declined{};
                    output.decision = static_cast<Index>(decisions_.size()); Decision row;
                    const auto empty = target(choice.empty, certificate.targets, map);
                    for (unsigned byte = 0; byte < 256; ++byte)
                        row[byte] = choice.byte[byte] == no_id ? empty : target(choice.byte[byte], certificate.targets, map);
                    row[256] = empty; decisions_.push_back(row); work_ += 257;
                } else if (output.op == Op::Repeat && output.count)
                    output.mask = analysis.boundary(ast.nodes[id].children.front()).first;
            }
            for (auto& instruction : code_) {
                ++work_;
                if (instruction.op == Op::Repeat && instruction.count) {
                    const auto& body = code_[instruction.children];
                    instruction.repeated_byte = body.op == Op::Byte && instruction.mask == body.mask;
                }
            }
            compact(); certified = true;
        } catch (const Declined&) { clear(); }
          catch (const std::bad_alloc&) { clear(); limit_reached = true; }
    }
    template<bool Profile = false, bool ReuseObservations = true, class Tick, class FrameCharge, class BatchCharge>
    [[nodiscard]] std::optional<std::size_t> execute(std::string_view input, std::size_t position,
            std::size_t end, bool exact, bool continuation_runs, Tick&& tick,
            FrameCharge&& frame_charge, BatchCharge&& batch_charge, Runtime* statistics = nullptr) const {
        span(input, position, end);
        Runtime local; auto& stats = statistics ? *statistics : local; stats = {};
        struct Frame { Index pc; std::size_t next, copies = 1; };
        std::vector<Frame> stack;
        std::size_t depth = 0;
        const auto step = [&] { tick(); ++stats.steps; };
        const auto push = [&](Index pc, std::size_t next, bool logical = true) {
            stats.frames += logical; ++depth; stats.maximum_frames = std::max(stats.maximum_frames, depth);
            if (continuation_runs && !stack.empty() && stack.back().pc == pc && stack.back().next == next) {
                ++stack.back().copies; return;
            }
            frame_charge(); ++stats.frame_runs; stack.push_back({pc, next});
            stats.maximum_frame_runs = std::max(stats.maximum_frame_runs, stack.size());
        };
        const auto pop = [&](std::size_t count = 1) {
            depth -= count;
            if (stack.back().copies == count) stack.pop_back(); else stack.back().copies -= count;
        };
        const auto first = [&](const Instruction& instruction) {
            if constexpr (Profile) ++stats.byte_tests;
            return position < end && instruction.mask[static_cast<unsigned char>(input[position])];
        };
        const auto prefix = [&](const Instruction& instruction, std::size_t& boundary) {
            while (boundary < instruction.count) {
                const auto& terminal = code_[children_[instruction.children + boundary]];
                if (terminal.op != Op::Byte && terminal.op != Op::Empty) break;
                step();
                if (terminal.op == Op::Byte) {
                    if constexpr (Profile) ++stats.byte_tests;
                    if (position == end || !terminal.mask[static_cast<unsigned char>(input[position])]) return false;
                    ++position;
                }
                ++boundary;
            }
            return true;
        };
        Index current = entry_;
        for (;;) {
            step(); const auto& instruction = code_[current];
            if constexpr (Profile) ++stats.dispatches;
            switch (instruction.op) {
            case Op::Empty: break;
            case Op::Byte:
                if constexpr (Profile) ++stats.byte_tests;
                if (position == end || !instruction.mask[static_cast<unsigned char>(input[position])]) return {};
                ++position; break;
            case Op::Choice:
                if constexpr (Profile) ++stats.decisions;
                current = decisions_[instruction.decision][position < end ? static_cast<unsigned char>(input[position]) : 256U];
                if (current == absent) return {};
                continue;
            case Op::Sequence: {
                std::size_t boundary = 0;
                if (continuation_runs && !prefix(instruction, boundary)) return {};
                if (boundary == instruction.count) break;
                if (boundary + 1 < instruction.count) push(current, boundary + 1);
                current = children_[instruction.children + boundary]; continue;
            }
            case Op::Repeat:
                if (!instruction.upper) break;
                if (!first(instruction)) { if (instruction.lower) return {}; break; }
                push(current, 1);
                if constexpr (ReuseObservations) {
                    if (instruction.repeated_byte) {
                        // Keep the terminal tick after the historical frame
                        // charge. The guard already proved its immutable byte.
                        step(); ++position;
                        if constexpr (Profile) ++stats.reused_byte_checks;
                        break;
                    }
                }
                current = static_cast<Index>(instruction.children); continue;
            }
            bool resumed = false;
            while (!stack.empty()) {
                step();
                if constexpr (Profile) ++stats.continuations;
                auto& frame = stack.back(); const auto& parent = code_[frame.pc];
                if (parent.op == Op::Sequence) {
                    if (continuation_runs && frame.copies > 1 && frame.next + 1 == parent.count) {
                        const auto& terminal = code_[children_[parent.children + frame.next]];
                        if (terminal.op == Op::Byte) {
                            const auto count = std::min<std::size_t>(frame.copies, 64);
                            if (end - position < count) return {};
                            batch_charge(count); stats.steps += count; stats.terminal_run_bytes += count;
                            for (std::size_t i = 0; i < count; ++i) {
                                if constexpr (Profile) ++stats.byte_tests;
                                if (!terminal.mask[static_cast<unsigned char>(input[position + i])]) return {};
                            }
                            position += count; pop(count); continue;
                        }
                    }
                    auto boundary = frame.next;
                    if (continuation_runs && !prefix(parent, boundary)) return {};
                    if (boundary == parent.count) { pop(); continue; }
                    current = children_[parent.children + boundary++];
                    if (boundary == parent.count) pop();
                    else if (frame.copies == 1) frame.next = boundary;
                    else { const auto owner = frame.pc; pop(); push(owner, boundary, false); }
                    resumed = true; break;
                }
                if (frame.next < parent.upper && first(parent)) {
                    current = static_cast<Index>(parent.children);
                    if (frame.copies == 1) ++frame.next;
                    else { const auto owner = frame.pc; const auto count = frame.next + 1; pop(); push(owner, count, false); }
                    if constexpr (ReuseObservations) {
                        if (parent.repeated_byte) {
                            step(); ++position;
                            if constexpr (Profile) ++stats.reused_byte_checks;
                            continue;
                        }
                    }
                    resumed = true; break;
                }
                if (frame.next < parent.lower) return {};
                pop();
            }
            if (!resumed) return !exact || position == end ? std::optional<std::size_t>{position} : std::nullopt;
        }
    }
};

class LoweredBoundedWordProgram final : public PredictiveProgramLayout {
public:
    struct Work {
        std::size_t dispatches = 0, byte_tests = 0, decisions = 0, continuations = 0;
        std::size_t lookahead_bytes = 0;
    };
private:
    template<bool Profile>
    Index choose(const Instruction& instruction, std::string_view input, std::size_t position, std::size_t end, Work& work) const {
        if constexpr (Profile) { ++work.decisions; work.lookahead_bytes += position < end; }
        const auto a = position < end ? static_cast<unsigned char>(input[position]) : 256U;
        auto value = decisions_[instruction.decision][a];
        if (value >= secondary && value != absent) {
            if constexpr (Profile) { ++work.decisions; work.lookahead_bytes += position < end && end - position > 1; }
            const auto b = position < end && end - position > 1 ? static_cast<unsigned char>(input[position + 1]) : 256U;
            value = decisions_[value - secondary][b];
        }
        return value;
    }
public:
    using Runtime = BoundedWordRegion::Runtime;
    LoweredBoundedWordProgram(const Ast& ast, const BoundedWordRegion& certificate) {
        if (!certificate.certified) return;
        source_nodes = ast.nodes.size();
        try {
            std::vector<Index> map;
            const auto source = sources(ast, certificate.targets_, ast.root, no_id, map);
            for (const auto id : source) {
                const auto pc = map[id]; instruction(ast, id, pc, ast.root, no_id, certificate.targets_, map);
                auto& output = code_[pc];
                if (output.op != Op::Choice && output.op != Op::Repeat) continue;
                const bool repeat = output.op == Op::Repeat;
                if (repeat && (!output.upper || output.upper == output.lower)) continue;
                if (id >= certificate.indices_.size() || certificate.indices_[id] >= certificate.choices_.size()) throw Declined{};
                if (decisions_.size() >= 512) throw Declined{};
                output.decision = static_cast<Index>(decisions_.size()); decisions_.emplace_back();
                Decision primary;
                for (unsigned a = 0; a <= 256; ++a) {
                    ++work_; const auto value = certificate.choices_[certificate.indices_[id]].token[a];
                    if (value == BoundedWordRegion::none) primary[a] = absent;
                    else if (value >= BoundedWordRegion::row_base) {
                        if (static_cast<std::size_t>(value - BoundedWordRegion::row_base) >= certificate.rows_.size() || decisions_.size() >= secondary) throw Declined{};
                        primary[a] = static_cast<Index>(secondary + decisions_.size());
                        Decision row;
                        for (unsigned b = 0; b <= 256; ++b) {
                            ++work_; const auto selected = certificate.rows_[value - BoundedWordRegion::row_base].token[b];
                            row[b] = selected == BoundedWordRegion::none ? absent :
                                repeat ? selected : target(selected, certificate.targets_, map);
                        }
                        decisions_.push_back(row);
                    } else primary[a] = repeat ? value : target(value, certificate.targets_, map);
                }
                decisions_[output.decision] = primary;
            }
            compact(); certified = true;
        } catch (const Declined&) { clear(); }
          catch (const std::bad_alloc&) { clear(); limit_reached = true; }
    }
    template<bool Profile = false, class Tick, class FrameCharge>
    [[nodiscard]] std::optional<std::size_t> execute(std::string_view input, std::size_t begin,
            std::size_t end, Tick&& tick, FrameCharge&& frame_charge, Runtime* statistics = nullptr, Work* profiling = nullptr) const {
        span(input, begin, end);
        Runtime local; auto& stats = statistics ? *statistics : local; stats = {};
        Work local_work; auto& work = profiling ? *profiling : local_work; if constexpr (Profile) work = {};
        struct Frame { Index pc; std::size_t next; };
        std::vector<Frame> stack;
        const auto step = [&] { tick(); ++stats.steps; };
        const auto push = [&](Index pc, std::size_t next) {
            frame_charge(); stack.push_back({pc, next}); ++stats.frames;
            stats.maximum_frames = std::max(stats.maximum_frames, stack.size());
        };
        auto position = begin; Index current = entry_;
        const auto again = [&](const Instruction& instruction, std::size_t count) {
            if (count == instruction.upper) return false;
            if (count < instruction.lower) return true;
            return choose<Profile>(instruction, input, position, end, work) == 1;
        };
        for (;;) {
            step(); const auto& instruction = code_[current];
            if constexpr (Profile) ++work.dispatches;
            switch (instruction.op) {
            case Op::Empty: break;
            case Op::Byte:
                if constexpr (Profile) ++work.byte_tests;
                if (position == end || !instruction.mask[static_cast<unsigned char>(input[position])]) return {};
                ++position; break;
            case Op::Choice:
                current = choose<Profile>(instruction, input, position, end, work);
                if (current == absent) return {};
                continue;
            case Op::Sequence:
                if (!instruction.count) break;
                if (instruction.count > 1) push(current, 1);
                current = children_[instruction.children]; continue;
            case Op::Repeat:
                if (!again(instruction, 0)) break;
                push(current, 1); current = static_cast<Index>(instruction.children); continue;
            }
            bool resumed = false;
            while (!stack.empty()) {
                step(); if constexpr (Profile) ++work.continuations; auto& frame = stack.back(); const auto& parent = code_[frame.pc];
                if (parent.op == Op::Sequence) {
                    current = children_[parent.children + frame.next++];
                    if (frame.next == parent.count) stack.pop_back();
                    resumed = true; break;
                }
                if (again(parent, frame.next)) {
                    ++frame.next; current = static_cast<Index>(parent.children); resumed = true; break;
                }
                if (frame.next < parent.lower) return {};
                stack.pop_back();
            }
            if (!resumed) return position == end ? std::optional<std::size_t>{position} : std::nullopt;
        }
    }
};

} // namespace crank::detail
