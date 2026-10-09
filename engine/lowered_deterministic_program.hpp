#pragma once
#include "deterministic_program.hpp"

namespace crank::detail {

// A physical lowering of an immutable, already certified SLR machine. The LR
// stack remains authoritative. A closure retains its exposed base state and folds
// only reductions that pop its known suffix. No top-state equality is used
// to merge distinct LR stacks. Every original tick/allocation callback remains in order.
class LoweredDeterministicProgram {
    using State = std::uint16_t;
    static constexpr State absent = std::numeric_limits<State>::max();
    struct Limit {};
    struct Step { State suffix_depth; };
    struct Closure { std::uint32_t step = 0, tail = 0; State count = 0, pushes = 0; };
    const DeterministicProgram& source_;
    std::vector<std::int16_t> actions_;
    std::vector<Closure> closures_;
    std::vector<std::uint32_t> contexts_, shift_contexts_;
    std::vector<Step> steps_;
    std::vector<State> tails_;
    std::vector<std::int16_t> byte_actions_;
    static constexpr std::int16_t accept = 32767;
public:
    struct Preparation {
        bool certified = false, limited = false;
        std::size_t states = 0, contexts = 0;
        std::size_t closures = 0, closure_steps = 0, maximum_closure = 0;
        std::size_t analysis_steps = 0, retained_bytes = 0, byte_table_bytes = 0;
    };
    struct Physical {
        std::size_t classifications = 0, action_lookups = 0, dynamic_gotos = 0;
        std::size_t constant_gotos = 0, closures = 0, fused_reductions = 0;
        std::size_t stack_writes = 0, growths = 0;
    };
private:
    Preparation statistics_;
    void build(bool local_bytes) {
        if (!source_.certified()) return;
        const auto states = source_.statistics_.states, columns = source_.terminals_ + 1;
        statistics_.states = states;
        actions_.reserve(source_.actions_.size());
        for (const auto action : source_.actions_)
            actions_.push_back(action == DeterministicProgram::accept_action ? accept : static_cast<std::int16_t>(action));
        constexpr std::size_t maximum_steps = 65'536, maximum_chain = 32;
        constexpr std::size_t maximum_entries = 32'768, maximum_tails = 65'536, maximum_work = 1'048'576;
        contexts_.resize(source_.gotos_.size(), std::numeric_limits<std::uint32_t>::max());
        shift_contexts_.resize(actions_.size(), std::numeric_limits<std::uint32_t>::max());
        std::map<std::pair<State, State>, std::uint32_t> intern;
        const auto compile_context = [&](State base, State initial) {
            const auto key = std::pair{base, initial};
            const auto found = intern.find(key); if (found != intern.end()) return found->second;
            const auto offset = static_cast<std::uint32_t>(closures_.size());
            if (columns > maximum_entries - closures_.size()) throw Limit{};
            intern.emplace(key, offset); closures_.resize(closures_.size() + columns);
            for (std::size_t token = 0; token < columns; ++token) {
                std::array<Step, maximum_chain> chain; std::size_t count = 0;
                std::array<State, maximum_chain + 1> tail; tail[0] = initial; std::size_t tail_size = 1;
                while (count < maximum_chain && steps_.size() + count < maximum_steps) {
                    const auto action = actions_[tail[tail_size - 1] * columns + token];
                    if (++statistics_.analysis_steps > maximum_work) throw Limit{};
                    if (action >= 0) break;
                    const auto reduction = source_.reductions_[-action];
                    if (reduction.length > tail_size) break;
                    const auto remaining = tail_size - reduction.length;
                    const auto exposed = remaining == 0 ? base : tail[remaining - 1];
                    const auto next = source_.gotos_[exposed * source_.nonterminals_ + reduction.lhs];
                    if (next == absent) break;
                    tail_size = remaining; tail[tail_size++] = next; chain[count++] = {static_cast<State>(tail_size)};
                }
                auto& closure = closures_[offset + token];
                if (!count) continue;
                if (tail_size > maximum_tails - tails_.size()) throw Limit{};
                closure.step = static_cast<std::uint32_t>(steps_.size());
                closure.tail = static_cast<std::uint32_t>(tails_.size());
                closure.count = static_cast<State>(count);
                closure.pushes = static_cast<State>(tail_size);
                steps_.insert(steps_.end(), chain.begin(), chain.begin() + count); tails_.insert(tails_.end(), tail.begin(), tail.begin() + tail_size);
                ++statistics_.closures; statistics_.closure_steps += count;
                statistics_.maximum_closure = std::max(statistics_.maximum_closure, count);
            }
            return offset;
        };
        for (std::size_t base = 0; base < states; ++base) {
            for (std::size_t nt = 0; nt < source_.nonterminals_; ++nt) {
                const auto initial = source_.gotos_[base * source_.nonterminals_ + nt];
                if (initial != absent) contexts_[base * source_.nonterminals_ + nt] = compile_context(base, initial);
            }
            for (std::size_t token = 0; token < columns; ++token) {
                const auto slot = base * columns + token; const auto action = actions_[slot];
                if (action > 0 && action != accept) shift_contexts_[slot] = compile_context(base, static_cast<State>(action - 1));
            }
        }
        closures_.shrink_to_fit(); steps_.shrink_to_fit(); tails_.shrink_to_fit();
        statistics_.contexts = intern.size();
        if (local_bytes) {
            byte_actions_.resize(states * 257);
            for (std::size_t s = 0; s < states; ++s) {
                for (std::size_t byte = 0; byte < 256; ++byte)
                    byte_actions_[s * 257 + byte] = actions_[s * columns + source_.byte_class_[byte]];
                byte_actions_[s * 257 + 256] = actions_[s * columns + source_.terminals_];
            }
        }
        statistics_.certified = true;
        statistics_.byte_table_bytes = byte_actions_.capacity() * sizeof(std::int16_t);
        statistics_.retained_bytes = sizeof(*this) + actions_.capacity() * sizeof(std::int16_t) +
            (contexts_.capacity() + shift_contexts_.capacity()) * sizeof(std::uint32_t) + closures_.capacity() * sizeof(Closure) +
            steps_.capacity() * sizeof(Step) + tails_.capacity() * sizeof(State) + statistics_.byte_table_bytes;
    }
public:
    explicit LoweredDeterministicProgram(const DeterministicProgram& source, bool local_bytes = false) : source_(source) {
        try { build(local_bytes); }
        catch (const Limit&) { statistics_.limited = true; }
        catch (const std::bad_alloc&) { statistics_.limited = true; }
        if (!statistics_.certified) {
            decltype(actions_){}.swap(actions_); decltype(contexts_){}.swap(contexts_);
            decltype(shift_contexts_){}.swap(shift_contexts_); decltype(closures_){}.swap(closures_);
            decltype(steps_){}.swap(steps_); decltype(tails_){}.swap(tails_); decltype(byte_actions_){}.swap(byte_actions_);
            statistics_.retained_bytes = sizeof(*this);
        }
    }
    [[nodiscard]] bool certified() const { return statistics_.certified; }
    [[nodiscard]] bool ready() const { return certified(); }
    [[nodiscard]] std::size_t compilation_steps() const { return statistics_.analysis_steps; }
    [[nodiscard]] std::size_t retained_bytes() const { return statistics_.retained_bytes; }
    [[nodiscard]] std::size_t code_bytes() const { return statistics_.retained_bytes - sizeof(*this); }
    [[nodiscard]] const Preparation& statistics() const { return statistics_; }

    template<bool Fuse = true, bool LocalBytes = false, bool Profile = false, class Tick, class Allocate>
    bool execute_variant(std::string_view subject, std::size_t begin, std::size_t end,
                         Tick&& tick, Allocate&& allocate, DeterministicProgram::Runtime& runtime,
                         Physical* physical = nullptr) const {
        if (!certified() || begin > end || end > subject.size() || (LocalBytes && byte_actions_.empty()))
            throw std::invalid_argument("invalid lowered exact-program demand");
        runtime = {}; if constexpr (Profile) *physical = {};
        // Inline storage is physical only: every push retains its source
        // allocation callback, including pushes removed by a fused closure.
        std::array<State, 64> local;
        std::vector<State> spill; State* stack = local.data();
        std::size_t size = 0, capacity = local.size();
        const auto grow = [&](std::size_t needed) {
            if (needed <= capacity) return;
            std::vector<State> next(std::max(needed, capacity * 2));
            std::copy_n(stack, size, next.data()); spill.swap(next); stack = spill.data(); capacity = spill.size();
            if constexpr (Profile) ++physical->growths;
        };
        allocate(); stack[size++] = 0; runtime.peak_stack = 1;
        if constexpr (Profile) ++physical->stack_writes;
        std::size_t position = begin;
        const auto columns = source_.terminals_ + 1;
        auto token = position == end ? source_.terminals_ : source_.byte_class_[static_cast<unsigned char>(subject[position])];
        if constexpr (Profile) ++physical->classifications;
        const auto finish_push = [&](State initial, std::uint32_t context) {
            if constexpr (Fuse) {
                const auto& closure = closures_[context + token];
                if (closure.count) {
                    for (std::size_t i = 0; i < closure.count; ++i) {
                        tick(); ++runtime.actions;
                        const auto& step = steps_[closure.step + i];
                        allocate(); runtime.peak_stack = std::max(runtime.peak_stack, size + step.suffix_depth); ++runtime.reductions;
                    }
                    grow(size + closure.pushes);
                    std::copy_n(tails_.data() + closure.tail, closure.pushes, stack + size); size += closure.pushes;
                    if constexpr (Profile) {
                        ++physical->closures; physical->fused_reductions += closure.count;
                        physical->constant_gotos += closure.count; physical->stack_writes += closure.pushes;
                    }
                    return;
                }
            }
            grow(size + 1); stack[size++] = initial;
            if constexpr (Profile) ++physical->stack_writes;
        };
        for (;;) {
            tick(); ++runtime.actions;
            const auto slot = stack[size - 1] * columns + token;
            auto action = actions_[slot];
            if constexpr (LocalBytes) action = byte_actions_[stack[size - 1] * 257 + (position == end ? 256 : static_cast<unsigned char>(subject[position]))];
            if constexpr (Profile) ++physical->action_lookups;
            if (!action) return false;
            if (action == accept) return position == end;
            if (action > 0) {
                allocate(); runtime.peak_stack = std::max(runtime.peak_stack, size + 1); ++runtime.shifts; ++position;
                token = position == end ? source_.terminals_ : source_.byte_class_[static_cast<unsigned char>(subject[position])];
                if constexpr (Profile) ++physical->classifications;
                finish_push(static_cast<State>(action - 1), shift_contexts_[slot]);
            } else {
                const auto reduction = source_.reductions_[-action];
                if (reduction.length >= size) throw std::logic_error("invalid lowered reduction");
                size -= reduction.length;
                const auto goto_slot = stack[size - 1] * source_.nonterminals_ + reduction.lhs;
                const auto next = source_.gotos_[goto_slot];
                if constexpr (Profile) ++physical->dynamic_gotos;
                if (next == absent) throw std::logic_error("invalid lowered goto");
                allocate(); runtime.peak_stack = std::max(runtime.peak_stack, size + 1); ++runtime.reductions;
                finish_push(next, contexts_[goto_slot]);
            }
        }
    }
    template<class Tick, class Allocate>
    bool execute(std::string_view subject, std::size_t begin, std::size_t end,
                 Tick&& tick, Allocate&& allocate, DeterministicProgram::Runtime& runtime) const {
        return execute_variant(subject, begin, end, std::forward<Tick>(tick), std::forward<Allocate>(allocate), runtime);
    }
};
} // namespace crank::detail
