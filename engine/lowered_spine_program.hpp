#pragma once
#include "exact_end_spines.hpp"
#include <cstdint>
#include <stdexcept>

namespace crank::detail {

// Lower a strict subset of a mature exact-spine certificate to immutable rim
// instructions. State selection, attempted byte checks and transitions retain
// their original order. The caller owns allocation, semantic ticks, cancellation
// polling and publication; exceptions from Tick propagate without interception.
// Wider words or states without one-byte dispatch keep the mature interpreter.
class LoweredSpineProgram {
    using Certificate = ExactEndSpines;
    struct Declined {};
    struct Limited {};
    using Id = std::uint16_t;
    static constexpr Id absent = UINT16_MAX;
    enum class Op : std::uint8_t { AcceptEmpty, AcceptByte, Pair, Left, Right, Jump };
    struct Instruction {
        std::bitset<256> left, right;
        std::size_t minimum = 0;
        Id child = absent;
        Op op = Op::AcceptEmpty;
    };
    struct State {
        std::array<Id, 256> choice;
        Id empty = absent;
        Certificate::State::Axis axis = Certificate::State::Axis::None;
    };
    std::vector<Instruction> code_;
    std::vector<State> states_;
    bool ready_ = false, limit_reached_ = false;
    void clear() { std::vector<Instruction>{}.swap(code_); std::vector<State>{}.swap(states_); }
    std::size_t compile_steps_ = 0;
public:
    LoweredSpineProgram(const Ast& ast, const Certificate& certificate, std::size_t max_steps = 65536,
            bool enabled = true) {
        if (!enabled || !certificate.certified || certificate.states.empty()) return;
        if (ast.nodes.size() > 4096 || certificate.states.size() > 128) { limit_reached_ = true; return; }
        // Internal precondition: certificate belongs to this checked immutable
        // AST. This lowering preserves its proof; it does not certify a source.
        // Actual effect fields are checked defensively, independent of flags.
        try {
            const auto charge = [&] { if (compile_steps_ == max_steps) throw Limited{}; ++compile_steps_; };
            for (const auto& node : ast.nodes) {
                charge();
                if (node.kind == Kind::Reference || node.bind_count != no_id || node.use_count != no_id || node.count_path != no_id)
                    throw Declined{};
            }
            states_.reserve(certificate.states.size());
            std::size_t count = 0;
            for (const auto& state : certificate.states) {
                charge();
                if (state.clauses.size() > 512 - count) throw Limited{};
                count += state.clauses.size();
            }
            code_.reserve(count);
            for (const auto& source : certificate.states) {
                charge();
                // Axis None can include acyclic zero-rim transfers; preserve
                // those in the mature interpreter rather than add a second
                // clause-search implementation to this lowering.
                if (source.axis == Certificate::State::Axis::None) throw Declined{};
                State state; state.axis = source.axis;
                const auto base = code_.size();
                for (const auto& clause : source.clauses) {
                    charge();
                    if (clause.prefix.size() > 1 || clause.suffix.size() > 1 ||
                        (clause.child == no_id && clause.width() > 1)) throw Declined{};
                    if (clause.minimum < clause.width()) throw Declined{};
                    Instruction ins; ins.minimum = clause.minimum;
                    if (clause.child != no_id) {
                        if (clause.child >= certificate.states.size()) throw Declined{};
                        ins.child = static_cast<Id>(clause.child);
                    }
                    const auto copy = [&](const std::vector<NodeId>& rim, std::bitset<256>& mask) {
                        if (rim.empty()) return;
                        charge();
                        if (rim[0] >= ast.nodes.size() || ast.nodes[rim[0]].kind != Kind::CharSet) throw Declined{};
                        mask = ast.nodes[rim[0]].characters;
                    };
                    copy(clause.prefix, ins.left); copy(clause.suffix, ins.right);
                    if (clause.child == no_id) ins.op = clause.width() ? Op::AcceptByte : Op::AcceptEmpty;
                    else if (!clause.prefix.empty() && !clause.suffix.empty()) ins.op = Op::Pair;
                    else if (!clause.prefix.empty()) ins.op = Op::Left;
                    else if (!clause.suffix.empty()) ins.op = Op::Right;
                    else ins.op = Op::Jump;
                    // Mature base words occupy prefix; decline foreign layout.
                    if (clause.child == no_id && !clause.suffix.empty()) throw Declined{};
                    code_.push_back(ins);
                }
                const auto target = [&](std::size_t index) -> Id {
                    charge();
                    if (index == no_id) return absent;
                    if (index >= source.clauses.size()) throw Declined{};
                    return static_cast<Id>(base + index);
                };
                state.empty = target(source.empty);
                for (std::size_t byte = 0; byte < 256; ++byte) state.choice[byte] = target(source.choice[byte]);
                states_.push_back(state);
            }
            ready_ = true;
        } catch (const Declined&) { clear(); }
          catch (const Limited&) { clear(); limit_reached_ = true; }
          catch (const std::bad_alloc&) { clear(); limit_reached_ = true; }
    }
    [[nodiscard]] bool ready() const { return ready_; }
    [[nodiscard]] bool limit_reached() const { return limit_reached_; }
    [[nodiscard]] std::size_t compilation_steps() const { return compile_steps_; }
    [[nodiscard]] std::size_t code_bytes() const { return code_.size() * sizeof(Instruction); }
    [[nodiscard]] std::size_t retained_bytes() const { return sizeof(*this) + code_.capacity() * sizeof(Instruction) + states_.capacity() * sizeof(State); }
    template<class Tick>
    bool execute(std::string_view input, std::size_t left, std::size_t right,
                 Tick&& tick, std::size_t& boundary_bytes, std::size_t& transitions) const {
        if (!ready_ || left > right || right > input.size())
            throw std::invalid_argument("lowered spine requires a ready program and valid span");
        Id current = 0;
        for (;;) {
            tick();
            const auto& state = states_[current];
            const auto id = left == right ? state.empty : state.choice[static_cast<unsigned char>(
                input[state.axis == Certificate::State::Axis::First ? left : right - 1])];
            if (id == absent) return false;
            const auto& ins = code_[id];
            const auto span = right - left;
            if (span < ins.minimum) return false;
            const auto test = [&](const std::bitset<256>& mask, std::size_t at) {
                tick(); ++boundary_bytes;
                return mask[static_cast<unsigned char>(input[at])];
            };
            bool ok = true;
            switch (ins.op) {
            case Op::AcceptEmpty: return span == 0;
            case Op::AcceptByte: return span == 1 && test(ins.left, left);
            case Op::Pair:
                ok = test(ins.left, left) && test(ins.right, right - 1);
                if (ok) { ++left; --right; }
                break;
            case Op::Left: ok = test(ins.left, left); if (ok) ++left; break;
            case Op::Right: ok = test(ins.right, right - 1); if (ok) --right; break;
            case Op::Jump: break;
            }
            if (!ok) return false;
            current = ins.child; ++transitions;
        }
    }
};
} // namespace crank::detail
