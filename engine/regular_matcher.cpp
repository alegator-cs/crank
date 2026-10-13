#include "regular_matcher.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <variant>

namespace crank {
namespace {

using StateId = std::size_t;
using Bits = std::vector<std::uint64_t>;

struct Edge {
    std::bitset<256> characters;
    StateId target;
};
struct NfaState {
    std::vector<StateId> epsilon;
    std::vector<Edge> consuming;
};
struct Fragment { StateId entry, exit; };

class NfaBuilder {
public:
    explicit NfaBuilder(const Ast& ast, RegularOptions options)
        : ast_(ast), limit_(options.max_nfa_states),
          depth_limit_(options.max_expansion_depth), building_(ast.nodes.size(), false) {}

    Fragment build(NodeId id, std::size_t depth = 0) {
        if (depth >= depth_limit_)
            throw std::length_error("regular NFA expansion depth limit exceeded");
        if (id >= ast_.nodes.size())
            throw std::invalid_argument("regular AST has an invalid child index");
        if (building_[id])
            throw std::invalid_argument("regular AST contains a cycle");
        const Node& node = ast_.nodes[id];
        if (node.kind == Kind::Reference || node.bind_count != no_id ||
            node.use_count != no_id || node.count_path != no_id)
            throw std::invalid_argument("regular matcher requires a pure acyclic executed call graph");
        building_[id] = true;
        // Captures have no effect on Boolean membership.
        if (node.kind == Kind::Capture || node.kind == Kind::Call) {
            if (node.kind == Kind::Capture) require_unary(node);
            else require_leaf(node);
            // A separate fragment per occurrence prevents one call from returning
            // to another call site. Shared AST targets are never mutated/copied.
            Fragment result = build(node.kind == Kind::Call ? node.call_target
                                                           : node.children.front(), depth + 1);
            building_[id] = false;
            return result;
        }
        Fragment result{add_state(), add_state()};
        switch (node.kind) {
        case Kind::Define: // Declarations execute as epsilon; bodies are called explicitly.
            epsilon(result.entry, result.exit);
            break;
        case Kind::Empty:
            require_leaf(node);
            epsilon(result.entry, result.exit);
            break;
        case Kind::CharSet:
            require_leaf(node);
            states[result.entry].consuming.push_back({node.characters, result.exit});
            break;
        case Kind::Concat: {
            StateId previous = result.entry;
            for (NodeId child : node.children) {
                Fragment part = build(child, depth + 1);
                epsilon(previous, part.entry);
                previous = part.exit;
            }
            epsilon(previous, result.exit);
            break;
        }
        case Kind::Alt:
            for (NodeId child : node.children) {
                Fragment part = build(child, depth + 1);
                epsilon(result.entry, part.entry);
                epsilon(part.exit, result.exit);
            }
            break;
        case Kind::Repeat: {
            require_unary(node);
            if (node.upper && *node.upper < node.lower)
                throw std::invalid_argument("regular AST has reversed repetition bounds");
            StateId previous = result.entry;
            for (std::size_t count = 0; count < node.lower; ++count) {
                Fragment part = build(node.children.front(), depth + 1);
                epsilon(previous, part.entry);
                previous = part.exit;
            }
            if (node.upper) {
                for (std::size_t count = node.lower; count < *node.upper; ++count) {
                    // Each optional occurrence independently chooses its body path.
                    epsilon(previous, result.exit);
                    Fragment part = build(node.children.front(), depth + 1);
                    epsilon(previous, part.entry);
                    previous = part.exit;
                }
                epsilon(previous, result.exit);
            } else {
                epsilon(previous, result.exit);
                Fragment part = build(node.children.front(), depth + 1);
                epsilon(previous, part.entry);
                epsilon(part.exit, previous);
            }
            break;
        }
        default:
            throw std::invalid_argument("unsupported regular AST node");
        }
        building_[id] = false;
        return result;
    }

    std::vector<NfaState> states;

private:
    const Ast& ast_;
    std::size_t limit_;
    std::size_t depth_limit_;
    std::vector<bool> building_;

    StateId add_state() {
        if (states.size() >= limit_)
            throw std::length_error("regular NFA state limit exceeded");
        states.emplace_back();
        return states.size() - 1;
    }
    void epsilon(StateId source, StateId target) {
        states[source].epsilon.push_back(target);
    }
    static void require_unary(const Node& node) {
        if (node.children.size() != 1)
            throw std::invalid_argument("regular AST requires exactly one child");
    }
    static void require_leaf(const Node& node) {
        if (!node.children.empty())
            throw std::invalid_argument("regular AST leaf has children");
    }
};

bool insert(Bits& bits, StateId state) {
    const auto mask = std::uint64_t{1} << (state % 64);
    auto& word = bits[state / 64];
    const bool fresh = (word & mask) == 0;
    word |= mask;
    return fresh;
}

bool contains(const Bits& bits, StateId state) {
    return (bits[state / 64] & (std::uint64_t{1} << (state % 64))) != 0;
}

template <class Function>
void for_each_state(const Bits& bits, Function&& function) {
    for (std::size_t word = 0; word < bits.size(); ++word) {
        std::uint64_t pending = bits[word];
        while (pending != 0) {
            const auto bit = std::countr_zero(pending);
            function(word * 64 + static_cast<std::size_t>(bit));
            pending &= pending - 1;
        }
    }
}

struct BitsHash {
    std::size_t operator()(const Bits& bits) const noexcept {
        std::size_t hash = 0;
        for (std::uint64_t word : bits)
            hash ^= std::hash<std::uint64_t>{}(word) +
                static_cast<std::size_t>(0x9e3779b97f4a7c15ULL) + (hash << 6) + (hash >> 2);
        return hash;
    }
};



} // namespace

bool RegularProgram::eligible(const Ast& ast) { return eligible(ast, ast.root); }

bool RegularProgram::eligible(const Ast& ast, NodeId entry) {
    if (entry >= ast.nodes.size()) return false;
    struct Frame { NodeId node; std::size_t next = 0; };
    std::vector<unsigned char> color(ast.nodes.size());
    std::vector<Frame> pending{{entry}};
    while (!pending.empty()) {
        auto& frame = pending.back();
        const auto id = frame.node;
        const auto& node = ast.nodes[id];
        if (color[id] == 0) {
            if (node.kind == Kind::Reference || node.bind_count != no_id ||
                node.use_count != no_id || node.count_path != no_id) return false;
            switch (node.kind) {
            case Kind::Empty: case Kind::CharSet: case Kind::Call:
                if (!node.children.empty()) return false;
                break;
            case Kind::Repeat:
                if (node.upper && *node.upper < node.lower) return false;
                [[fallthrough]];
            case Kind::Capture:
                if (node.children.size() != 1) return false;
                break;
            case Kind::Concat: case Kind::Alt: case Kind::Define: break;
            default: return false;
            }
            color[id] = 1;
        }
        const auto edges = node.kind == Kind::Define ? 0 :
                           node.kind == Kind::Call ? 1 : node.children.size();
        if (frame.next == edges) {
            color[id] = 2;
            pending.pop_back();
            continue;
        }
        const auto child = node.kind == Kind::Call ? node.call_target
                                                  : node.children[frame.next];
        ++frame.next;
        if (child >= ast.nodes.size() || color[child] == 1) return false;
        if (color[child] == 0) pending.push_back({child});
    }
    return true;
}

struct RegularProgram::Impl {
    using PackedTable = std::variant<std::vector<std::uint8_t>,
        std::vector<std::uint16_t>, std::vector<std::uint32_t>,
        std::vector<std::uint64_t>>;

    std::size_t nfa_states = 0;
    std::array<unsigned char, 256> symbol_class{};
    std::size_t alphabet_size = 0;
    std::vector<std::vector<StateId>> transitions;
    std::vector<bool> accepting;
    PackedTable packed;
    std::size_t row_shift = 0;
    std::size_t dead_offset = std::numeric_limits<std::size_t>::max();
    bool byte_columns = false;
    std::array<std::uint16_t,256> pair_first_column{};
    std::vector<std::uint16_t> paired;
    std::size_t pair_row_shift = 0, paired_analysis_steps = 0;
    std::size_t paired_dead_offset = std::numeric_limits<std::size_t>::max();

    Impl(const Ast& ast, NodeId entry, RegularOptions options) {
        if (!RegularProgram::eligible(ast, entry))
            throw std::invalid_argument("regular matcher requires a pure acyclic executed call graph");
        NfaBuilder builder(ast, options);
        const Fragment root = builder.build(entry);
        const auto& nfa = builder.states;
        nfa_states = nfa.size();
        const std::size_t words = (nfa.size() + 63) / 64;

        // Characters with identical membership in every edge label are equivalent.
        // This changes storage/compile work, while all 256 byte values remain valid.
        std::vector<std::bitset<256>> alphabet{std::bitset<256>{}.set()};
        for (const auto& state : nfa) {
            for (const auto& edge : state.consuming) {
                const auto count = alphabet.size();
                for (std::size_t i = 0; i < count; ++i) {
                    const auto yes = alphabet[i] & edge.characters;
                    const auto no = alphabet[i] & ~edge.characters;
                    if (yes.any() && no.any()) {
                        alphabet[i] = yes;
                        alphabet.push_back(no);
                    }
                }
            }
        }
        alphabet_size = alphabet.size();
        std::vector<std::size_t> representative(alphabet_size);
        for (std::size_t cls = 0; cls < alphabet_size; ++cls) {
            bool first = true;
            for (std::size_t byte = 0; byte < 256; ++byte) {
                if (!alphabet[cls].test(byte)) continue;
                symbol_class[byte] = static_cast<unsigned char>(cls);
                if (first) representative[cls] = byte;
                first = false;
            }
        }

        // Cache single-state epsilon closures. Cycles introduced by nullable stars
        // terminate because each state is inserted once per closure traversal.
        std::vector<Bits> closure(nfa.size(), Bits(words));
        std::vector<StateId> pending;
        pending.reserve(nfa.size());
        for (StateId origin = 0; origin < nfa.size(); ++origin) {
            insert(closure[origin], origin);
            pending.clear();
            pending.push_back(origin);
            while (!pending.empty()) {
                const auto current = pending.back();
                pending.pop_back();
                for (StateId next : nfa[current].epsilon)
                    if (insert(closure[origin], next)) pending.push_back(next);
            }
        }

        std::unordered_map<Bits, StateId, BitsHash> known;
        std::vector<Bits> subsets;
        auto intern = [&](const Bits& subset) -> StateId {
            const auto found = known.find(subset);
            if (found != known.end()) return found->second;
            if (subsets.size() >= options.max_dfa_states)
                throw std::length_error("regular DFA state limit exceeded");
            const StateId id = subsets.size();
            known.emplace(subset, id);
            subsets.push_back(subset);
            transitions.emplace_back(alphabet_size);
            accepting.push_back(contains(subset, root.exit));
            return id;
        };
        intern(closure[root.entry]);
        {
            // intern() copies new targets; one scratch buffer serves every edge.
            Bits target(words);
            for (StateId source = 0; source < subsets.size(); ++source) {
                // intern() grows subsets, so retain a copy across those reallocations.
                const Bits subset = subsets[source];
                for (std::size_t cls = 0; cls < alphabet_size; ++cls) {
                    std::fill(target.begin(), target.end(), std::uint64_t{0});
                    for_each_state(subset, [&](StateId current) {
                        for (const auto& edge : nfa[current].consuming) {
                            if (!edge.characters.test(representative[cls])) continue;
                            const auto& epsilon = closure[edge.target];
                            for (std::size_t i = 0; i < words; ++i) target[i] |= epsilon[i];
                        }
                    });
                    const auto destination = intern(target);
                    transitions[source][cls] = destination;
                }
            }
        }
        minimize();
        pack(options);
    }

    void minimize() {
        // Moore refinement identifies states with the same future language.
        // Equivalent continuation languages share a compact transition row.
        // The determinization cap applies before this reduction.
        std::vector<StateId> partition(transitions.size());
        for (StateId state = 0; state < transitions.size(); ++state)
            partition[state] = accepting[state] ? 1 : 0;
        for (;;) {
            std::unordered_map<Bits, StateId, BitsHash> signatures;
            std::vector<StateId> refined(transitions.size());
            for (StateId state = 0; state < transitions.size(); ++state) {
                Bits signature;
                signature.reserve(alphabet_size + 1);
                signature.push_back(partition[state]);
                for (StateId target : transitions[state])
                    signature.push_back(partition[target]);
                const StateId next_class = signatures.size();
                const auto [entry, inserted] = signatures.emplace(std::move(signature), next_class);
                (void)inserted;
                refined[state] = entry->second;
            }
            if (refined == partition) break;
            partition = std::move(refined);
        }

        const std::size_t count = *std::max_element(partition.begin(), partition.end()) + 1;
        std::vector<std::vector<StateId>> minimized(count);
        std::vector<bool> minimized_accepting(count);
        for (StateId state = 0; state < transitions.size(); ++state) {
            auto& row = minimized[partition[state]];
            if (!row.empty()) continue;
            row.reserve(alphabet_size);
            for (StateId target : transitions[state]) row.push_back(partition[target]);
            minimized_accepting[partition[state]] = accepting[state];
        }
        // Classes are numbered on first appearance while traversing states from
        // zero, so the original start state remains the first minimized state.
        transitions = std::move(minimized);
        accepting = std::move(minimized_accepting);
    }

    void statistics(RegularStatistics* out, std::size_t workers, std::size_t steps) const {
        if (out) {
            *out = {nfa_states, accepting.size(), workers, steps};
            out->dfa_table_bytes = std::visit([](const auto& table) {
                return table.capacity() * sizeof(typename std::decay_t<decltype(table)>::value_type);
            }, packed);
            out->paired_table_bytes = paired.capacity() * sizeof(std::uint16_t);
            out->paired_analysis_steps = paired_analysis_steps;
        }
    }

    void pack(RegularOptions options) {
        const auto count = transitions.size();
        constexpr auto maximum = std::numeric_limits<std::size_t>::max();
        auto offset_bytes = [](std::size_t offset) -> std::size_t {
            if (offset <= std::numeric_limits<std::uint8_t>::max()) return 1;
            if (offset <= std::numeric_limits<std::uint16_t>::max()) return 2;
            if (offset <= std::numeric_limits<std::uint32_t>::max()) return 4;
            return 8;
        };
        // Small DFAs can index directly by the input byte. Bound the expansion
        // so patterns with many states retain the compact character partition.
        // All table entries are row offsets, removing a multiply per byte.
        constexpr std::size_t expanded_byte_budget = 64 * 1024;
        if (count <= maximum / 256) {
            const auto width = offset_bytes((count - 1) * 256);
            byte_columns = count * 256 <= expanded_byte_budget / width;
        }
        const std::size_t stride = byte_columns ? 256 : std::bit_ceil(alphabet_size);
        if (count > maximum / stride)
            throw std::length_error("regular DFA transition table is too large");
        row_shift = static_cast<std::size_t>(std::countr_zero(stride));
        const auto largest_offset = (count - 1) * stride;
        for (StateId state = 0; state < count; ++state) {
            if (!accepting[state] && std::all_of(transitions[state].begin(),
                transitions[state].end(), [state](StateId target) { return target == state; })) {
                dead_offset = state * stride;
                break;
            }
        }
        auto fill = [&]<class Offset>() {
            std::vector<Offset> table(count * stride);
            for (StateId state = 0; state < count; ++state) {
                const auto columns = byte_columns ? 256 : alphabet_size;
                for (std::size_t column = 0; column < columns; ++column) {
                    const auto cls = byte_columns ? symbol_class[column] : column;
                    table[state * stride + column] =
                        static_cast<Offset>(transitions[state][cls] * stride);
                }
            }
            packed = std::move(table);
        };
        if (largest_offset <= std::numeric_limits<std::uint8_t>::max())
            fill.template operator()<std::uint8_t>();
        else if (largest_offset <= std::numeric_limits<std::uint16_t>::max())
            fill.template operator()<std::uint16_t>();
        else if (largest_offset <= std::numeric_limits<std::uint32_t>::max())
            fill.template operator()<std::uint32_t>();
        else fill.template operator()<std::uint64_t>();
        // Compose two transitions only from an already exact minimized DFA.
        // Each cell stores delta(delta(q,a),b); no parser/AST facts are needed.
        // Bound all extra construction/storage independently of DFA admission.
        constexpr std::size_t hard_pair_bytes = 65536;
        const auto pair_budget = std::min(options.max_paired_table_bytes, hard_pair_bytes);
        const auto pair_stride = std::bit_ceil(alphabet_size * alphabet_size);
        if (options.paired_transitions && pair_stride <= pair_budget / sizeof(std::uint16_t) &&
            count <= pair_budget / sizeof(std::uint16_t) / pair_stride) {
            try {
                std::vector<std::uint16_t> table(count * pair_stride);
                pair_row_shift = static_cast<std::size_t>(std::countr_zero(pair_stride));
                for (std::size_t state = 0; state < count; ++state)
                    for (std::size_t a = 0; a < alphabet_size; ++a)
                        for (std::size_t b = 0; b < alphabet_size; ++b) {
                            ++paired_analysis_steps;
                            table[state * pair_stride + a * alphabet_size + b] =
                                static_cast<std::uint16_t>(transitions[transitions[state][a]][b] * pair_stride);
                        }
                for (std::size_t b = 0; b < 256; ++b) {
                    ++paired_analysis_steps;
                    pair_first_column[b] = static_cast<std::uint16_t>(symbol_class[b] * alphabet_size);
                }
                if (dead_offset != maximum)
                    paired_dead_offset = (dead_offset >> row_shift) * pair_stride;
                paired = std::move(table);
            } catch (const std::bad_alloc&) {
                // Optional lowering never loses the existing exact table.
                paired_analysis_steps = 0;
            }
        }
        // Nested rows serve compilation only. Retain the original immutable
        // table for single-step/endpoint APIs and the optional Boolean lowering.
        std::vector<std::vector<StateId>>{}.swap(transitions);
    }

    template <bool Bytes, class Table>
    std::size_t scan(const Table& table, std::string_view subject,
                     std::size_t current = 0) const {
        const auto* entries = table.data();
        auto next = [&](char byte) {
            const auto value = static_cast<unsigned char>(byte);
            const auto column = Bytes ? value : symbol_class[value];
            current = entries[current + column];
        };
        std::size_t i = 0;
        // A rejecting self-loop is stable under every suffix. Checking between
        // small blocks retains early rejection without a branch for every byte.
        for (; subject.size() - i >= 8; i += 8) {
            if (current == dead_offset) return current;
            next(subject[i]); next(subject[i + 1]);
            next(subject[i + 2]); next(subject[i + 3]);
            next(subject[i + 4]); next(subject[i + 5]);
            next(subject[i + 6]); next(subject[i + 7]);
        }
        if (current == dead_offset) return current;
        for (; i < subject.size(); ++i) next(subject[i]);
        return current;
    }

    std::size_t scan(std::string_view subject, std::size_t current = 0) const {
        // Short inputs retain the direct-byte table and its cheap entry path.
        // Two-byte composition halves the serial transition-load dependency
        // for long inputs. The second byte's class load is independent of state.
        if (!paired.empty() && subject.size() >= 16) {
            auto pair_state = (current >> row_shift) << pair_row_shift;
            std::size_t i = 0;
            const auto next = [&](std::size_t at) {
                const auto a = static_cast<unsigned char>(subject[at]);
                const auto b = static_cast<unsigned char>(subject[at + 1]);
                pair_state = paired[pair_state + pair_first_column[a] + symbol_class[b]];
            };
            for (; subject.size() - i >= 8; i += 8) {
                if (pair_state == paired_dead_offset) return dead_offset;
                next(i); next(i + 2); next(i + 4); next(i + 6);
            }
            if (pair_state == paired_dead_offset) return dead_offset;
            for (; subject.size() - i >= 2; i += 2) next(i);
            current = (pair_state >> pair_row_shift) << row_shift;
            subject.remove_prefix(i);
        }
        return std::visit([&](const auto& table) {
            return byte_columns ? scan<true>(table, subject, current)
                                : scan<false>(table, subject, current);
        }, packed);
    }

    std::size_t matching_copies(std::string_view subject, std::size_t width,
                                std::size_t copies) const {
        auto run = [&]<bool Bytes>(const auto& table) {
            for (std::size_t i = 0; i < copies; ++i) {
                const auto current = scan<Bytes>(table, subject.substr(i * width, width));
                if (!accepting[current >> row_shift]) return i;
            }
            return copies;
        };
        // The representation and column mode are invariant across copies. The
        // DFA still starts at state zero for every independent fixed-width body.
        return std::visit([&](const auto& table) {
            return byte_columns ? run.template operator()<true>(table)
                                : run.template operator()<false>(table);
        }, packed);
    }

    std::vector<std::size_t> endpoints(std::string_view subject, std::size_t start) const {
        std::vector<std::size_t> result;
        if (accepting[0]) result.push_back(start);
        auto run = [&]<bool Bytes>(const auto& table) {
            std::size_t current = 0;
            for (std::size_t i = start; i < subject.size(); ++i) {
                if (current == dead_offset) break;
                const auto byte = static_cast<unsigned char>(subject[i]);
                current = table[current + (Bytes ? byte : symbol_class[byte])];
                if (accepting[current >> row_shift]) result.push_back(i + 1);
            }
        };
        std::visit([&](const auto& table) {
            if (byte_columns) run.template operator()<true>(table);
            else run.template operator()<false>(table);
        }, packed);
        return result;
    }
};

RegularProgram::RegularProgram(const Ast& ast, RegularOptions options)
    : RegularProgram(ast, ast.root, options) {}

RegularProgram::RegularProgram(const Ast& ast, NodeId entry, RegularOptions options)
    : impl_(std::make_shared<Impl>(ast, entry, options)) {}

std::size_t RegularProgram::transition(std::size_t state, unsigned char byte) const {
    const auto column = impl_->byte_columns ? byte : impl_->symbol_class[byte];
    return std::visit([&](const auto& table) -> std::size_t { return table[state + column]; }, impl_->packed);
}
bool RegularProgram::accepting(std::size_t state) const {
    return impl_->accepting[state >> impl_->row_shift];
}
bool RegularProgram::dead(std::size_t state) const { return state == impl_->dead_offset; }
RegularStatistics RegularProgram::construction_statistics() const {
    RegularStatistics result;
    impl_->statistics(&result, 1, 0);
    return result;
}

std::size_t RegularProgram::retained_bytes() const {
    std::size_t bytes = sizeof(*this) + sizeof(Impl) +
        impl_->transitions.capacity() * sizeof(std::vector<StateId>) +
        (impl_->accepting.capacity() + 7U) / 8U + impl_->paired.capacity() * sizeof(std::uint16_t);
    for (const auto& row : impl_->transitions) bytes += row.capacity() * sizeof(StateId);
    bytes += std::visit([](const auto& table) {
        return table.capacity() * sizeof(typename std::decay_t<decltype(table)>::value_type);
    }, impl_->packed);
    return bytes;
}

bool RegularProgram::full_match(std::string_view subject, RegularStatistics* statistics) const {
    const auto current = impl_->scan(subject) >> impl_->row_shift;
    impl_->statistics(statistics, 1, subject.size());
    return impl_->accepting[current];
}

std::size_t RegularProgram::matching_copies(std::string_view subject, std::size_t width,
                                           std::size_t copies) const {
    if ((width == 0 && !subject.empty()) ||
        (width != 0 && (subject.size() / width != copies || subject.size() % width != 0)))
        throw std::invalid_argument("regular DFA copy dimensions do not cover the subject");
    if (copies == 0) return 0;
    if (width == 0) return impl_->accepting[0] ? copies : 0;
    return impl_->matching_copies(subject, width, copies);
}

std::vector<std::size_t> RegularProgram::endpoints(std::string_view subject,
    std::size_t start, RegularStatistics* statistics) const {
    if (start > subject.size())
        throw std::out_of_range("regular matcher start exceeds subject length");
    auto result = impl_->endpoints(subject, start);
    impl_->statistics(statistics, 1, subject.size() - start);
    return result;
}


} // namespace crank
