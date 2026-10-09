#include "fragment_matcher.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <stdexcept>
#include <utility>

namespace crank {
namespace {

struct ByteSet {
    std::array<unsigned char, 256> allowed{};
    unsigned low = 0, high = 0, span = 0;
    bool range = false, pair = false;

    explicit ByteSet(const std::bitset<256>& bits) {
        low = 256;
        for (unsigned byte = 0; byte < 256; ++byte) {
            allowed[byte] = bits[byte];
            if (bits[byte]) { low = std::min(low, byte); high = byte; }
        }
        range = low != 256 && bits.count() == high - low + 1;
        pair = bits.count() == 2;
        span = range ? high - low : 0;
    }

    bool operator==(const ByteSet& other) const { return allowed == other.allowed; }
    bool accepts(unsigned char byte) const {
        return range ? unsigned(byte) - low <= span : allowed[byte] != 0;
    }
};

// Fixed-size blocks let the compiler vectorize the ordinary contiguous and
// small-stride range scans. Only a failing block needs a scalar first-error
// search. This remains portable C++ and requires no CPU feature dispatch.
template<std::size_t Stride>
std::size_t range_failure(std::string_view subject, std::size_t phase,
                          unsigned low, unsigned span) {
    constexpr std::size_t block = 32;
    std::size_t i = phase;
    const auto count = subject.size();
    while (i < count && count - i > (block - 1) * Stride) {
        unsigned invalid = 0;
        for (std::size_t j = 0; j < block; ++j)
            invalid |= unsigned(static_cast<unsigned char>(subject[i + j * Stride])) - low > span;
        if (invalid) break;
        i += block * Stride;
    }
    for (; i < count; i += Stride)
        if (unsigned(static_cast<unsigned char>(subject[i])) - low > span) return i;
    return count;
}

template<std::size_t Stride>
std::size_t pair_failure(std::string_view subject, std::size_t phase,
                         unsigned low, unsigned high) {
    constexpr std::size_t block = 32;
    std::size_t i = phase;
    const auto count = subject.size();
    while (i < count && count - i > (block - 1) * Stride) {
        unsigned invalid = 0;
        for (std::size_t j = 0; j < block; ++j) {
            const auto byte = static_cast<unsigned char>(subject[i + j * Stride]);
            invalid |= (unsigned(byte) != low) & (unsigned(byte) != high);
        }
        if (invalid) break;
        i += block * Stride;
    }
    for (; i < count; i += Stride) {
        const auto byte = static_cast<unsigned char>(subject[i]);
        if (unsigned(byte) != low && unsigned(byte) != high) return i;
    }
    return count;
}

template<std::size_t Stride>
std::size_t small_failure(std::string_view subject, std::size_t phase, const ByteSet& set) {
    if (set.range) return range_failure<Stride>(subject, phase, set.low, set.span);
    return pair_failure<Stride>(subject, phase, set.low, set.high);
}

std::size_t first_failure(std::string_view subject, const std::vector<ByteSet>& sequence) {
    const auto width = sequence.size();
    if (width == 0) return subject.size();
    if (width <= 8) {
        std::size_t first = subject.size();
        for (std::size_t phase = 0; phase < width; ++phase) {
            const auto& set = sequence[phase];
            std::size_t failure = first;
            // Once an earlier failure is known, later phases only need the
            // prefix preceding it. This also preserves first-failing-body stats.
            const auto prefix = subject.substr(0, first);
            if (set.range || set.pair) {
                switch (width) {
                case 1: failure = small_failure<1>(prefix, phase, set); break;
                case 2: failure = small_failure<2>(prefix, phase, set); break;
                case 3: failure = small_failure<3>(prefix, phase, set); break;
                case 4: failure = small_failure<4>(prefix, phase, set); break;
                case 5: failure = small_failure<5>(prefix, phase, set); break;
                case 6: failure = small_failure<6>(prefix, phase, set); break;
                case 7: failure = small_failure<7>(prefix, phase, set); break;
                case 8: failure = small_failure<8>(prefix, phase, set); break;
                }
            } else {
                for (std::size_t i = phase; i < first; i += width) {
                    if (!set.allowed[static_cast<unsigned char>(subject[i])]) {
                        failure = i;
                        break;
                    }
                }
            }
            first = std::min(first, failure);
        }
        return first;
    }
    // Large fixed bodies generally become a single literal comparison below.
    // Retain a compact direct loop for the remaining Cartesian products.
    for (std::size_t begin = 0; begin < subject.size(); begin += width)
        for (std::size_t phase = 0; phase < width && phase < subject.size() - begin; ++phase)
            if (!sequence[phase].accepts(static_cast<unsigned char>(subject[begin + phase])))
                return begin + phase;
    return subject.size();
}

// The executed graph remains shared. This bounded postorder view describes
// language boundaries and transparent aliases; it never copies a call body.
// Pair hashes overapproximate correlation: collisions may miss a rejection,
// but an absent bit is a proof. Multi-byte alternatives are not Cartesianized.
struct Boundary {
    bool nullable = false, productive = false;
    std::bitset<256> first, last, single;
    std::uint64_t first_pairs = 0, last_pairs = 0;
    std::uint64_t first_residues = 0, last_residues = 0, single_residues = 0;
    std::optional<std::size_t> width;
    NodeId alias = no_id;
};

struct AnalysisDeclined {};
class StructuralView {
    const Ast& ast_;
    std::size_t remaining_, period_limit_;
    std::size_t& used_;
    std::vector<Boundary> summaries_;

    void charge(std::size_t count = 1) {
        if (count > remaining_) throw AnalysisDeclined{};
        remaining_ -= count;
        used_ += count;
    }
    static std::uint64_t residues(const std::bitset<256>& bytes) {
        const std::bitset<256> low(~std::uint64_t{0});
        return (bytes & low).to_ullong() | ((bytes >> 64) & low).to_ullong() |
               ((bytes >> 128) & low).to_ullong() | ((bytes >> 192) & low).to_ullong();
    }
    std::uint64_t pairs(std::uint64_t left, std::uint64_t right) {
        if (!right) return 0;
        // Cached byte residues make correlation composition at most 64 rotates,
        // usually one. Broad classes saturate and stop before enumerating pairs.
        std::uint64_t result = 0;
        while (left) {
            charge();
            const auto shift = (17U * std::countr_zero(left)) & 63U;
            result |= std::rotl(right, static_cast<int>(shift));
            left &= left - 1;
            if (result == ~std::uint64_t{0}) break;
        }
        return result;
    }
    Boundary concat(const Boundary& a, const Boundary& b) {
        Boundary r;
        if (!a.productive || !b.productive) return r;
        r.productive = true;
        r.nullable = a.nullable && b.nullable;
        r.first = a.first | (a.nullable ? b.first : std::bitset<256>{});
        r.last = b.last | (b.nullable ? a.last : std::bitset<256>{});
        r.single = (a.nullable ? b.single : std::bitset<256>{}) |
                   (b.nullable ? a.single : std::bitset<256>{});
        r.first_residues = a.first_residues | (a.nullable ? b.first_residues : 0);
        r.last_residues = b.last_residues | (b.nullable ? a.last_residues : 0);
        r.single_residues = (a.nullable ? b.single_residues : 0) |
                            (b.nullable ? a.single_residues : 0);
        r.first_pairs = a.first_pairs | (a.nullable ? b.first_pairs : 0) |
                        pairs(a.single_residues, b.first_residues);
        r.last_pairs = b.last_pairs | (b.nullable ? a.last_pairs : 0) |
                       pairs(a.last_residues, b.single_residues);
        if (a.width && b.width && *b.width <= period_limit_ - *a.width)
            r.width = *a.width + *b.width;
        return r;
    }
    static Boundary epsilon() {
        Boundary r; r.productive = r.nullable = true; r.width = 0; return r;
    }
public:
    StructuralView(const Ast& ast, const std::vector<NodeId>& order,
                   std::size_t budget, std::size_t period_limit, std::size_t& used)
        : ast_(ast), remaining_(budget), period_limit_(period_limit), used_(used) {
        // Bound even the temporary memo representation before allocating it.
        if (ast.nodes.size() > budget) throw AnalysisDeclined{};
        summaries_.resize(ast.nodes.size());
        for (const auto id : order) {
            charge();
            const auto& node = ast.nodes[id];
            Boundary r;
            switch (node.kind) {
            case Kind::Empty: case Kind::Define: r = epsilon(); break;
            case Kind::CharSet:
                r.productive = node.characters.any();
                r.first = r.last = r.single = node.characters;
                r.first_residues = r.last_residues = r.single_residues = residues(node.characters);
                if (period_limit_) r.width = 1;
                break;
            case Kind::Capture: r = summaries_[node.children.front()]; break;
            case Kind::Call: r = summaries_[node.call_target]; break;
            case Kind::Concat: {
                r = epsilon();
                NodeId sole = no_id; bool multiple = false;
                for (const auto child : node.children) {
                    charge();
                    const auto& part = summaries_[child];
                    r = concat(r, part);
                    if (!(part.productive && part.nullable && part.first.none())) {
                        if (sole != no_id) multiple = true;
                        sole = part.alias;
                    }
                }
                if (!multiple) r.alias = sole;
                break;
            }
            case Kind::Alt: {
                bool first = true;
                for (const auto child : node.children) {
                    charge();
                    const auto& part = summaries_[child];
                    if (!part.productive) continue;
                    r.productive = true; r.nullable |= part.nullable;
                    r.first |= part.first; r.last |= part.last; r.single |= part.single;
                    r.first_residues |= part.first_residues;
                    r.last_residues |= part.last_residues;
                    r.single_residues |= part.single_residues;
                    r.first_pairs |= part.first_pairs; r.last_pairs |= part.last_pairs;
                    if (first) r.width = part.width;
                    else if (r.width != part.width) r.width.reset();
                    first = false;
                }
                break;
            }
            case Kind::Repeat: {
                const auto& part = summaries_[node.children.front()];
                if (node.upper && *node.upper == 0) { r = epsilon(); break; }
                if (!part.productive) {
                    if (node.lower == 0) r = epsilon();
                    break;
                }
                r = part; r.alias = no_id;
                r.nullable = node.lower == 0 || part.nullable;
                if (node.lower > 1 && !part.nullable) {
                    r.single.reset(); r.single_residues = 0;
                }
                if (!node.upper || *node.upper >= 2) {
                    r.first_pairs |= pairs(part.single_residues, part.first_residues);
                    r.last_pairs |= pairs(part.last_residues, part.single_residues);
                }
                r.width.reset();
                if (part.width && *part.width == 0) r.width = 0;
                else if (part.width && node.upper && node.lower == *node.upper &&
                         node.lower <= period_limit_ / *part.width)
                    r.width = node.lower * *part.width;
                break;
            }
            default: throw AnalysisDeclined{};
            }
            if (r.alias == no_id) r.alias = id;
            summaries_[id] = std::move(r);
        }
    }
    const Boundary& at(NodeId id) const { return summaries_[id]; }
    bool emit(NodeId id, std::vector<ByteSet>& result) {
        charge();
        const auto& summary = at(id);
        if (summary.productive && summary.nullable && summary.first.none()) return true;
        if (summary.width == 1 && summary.productive) {
            if (result.size() == period_limit_) return false;
            result.emplace_back(summary.single); return true;
        }
        if (summary.alias != id) return emit(summary.alias, result);
        const auto& node = ast_.nodes[id];
        if (node.kind == Kind::Concat) {
            for (const auto child : node.children) if (!emit(child, result)) return false;
            return true;
        }
        if (node.kind == Kind::Repeat && node.upper && node.lower == *node.upper) {
            if (node.lower == 0) return true;
            std::vector<ByteSet> body;
            if (!emit(node.children.front(), body)) return false;
            if (body.empty()) return true;
            if (node.lower > (period_limit_ - result.size()) / body.size()) return false;
            charge(node.lower * body.size());
            for (std::size_t i = 0; i < node.lower; ++i)
                result.insert(result.end(), body.begin(), body.end());
            return true;
        }
        // Preserve correlation in alternatives wider than one byte.
        return false;
    }
};

std::optional<std::size_t> short_fixed_width(const Ast& ast, NodeId id) {
    constexpr std::size_t maximum_width = 2;
    const auto& node = ast.nodes[id];
    switch (node.kind) {
    case Kind::Empty: return 0;
    case Kind::CharSet: return 1;
    case Kind::Capture: return short_fixed_width(ast, node.children.front());
    case Kind::Concat: {
        std::size_t total = 0;
        for (const auto child : node.children) {
            const auto width = short_fixed_width(ast, child);
            if (!width || *width > maximum_width - total) return {};
            total += *width;
        }
        return total;
    }
    case Kind::Alt: {
        if (node.children.empty()) return {};
        const auto width = short_fixed_width(ast, node.children.front());
        if (!width) return {};
        for (std::size_t i = 1; i < node.children.size(); ++i)
            if (short_fixed_width(ast, node.children[i]) != width) return {};
        return width;
    }
    case Kind::Repeat: {
        const auto width = short_fixed_width(ast, node.children.front());
        if (!width) return {};
        if (*width == 0) return 0;
        if (!node.upper || node.lower != *node.upper || node.lower > maximum_width / *width)
            return {};
        return node.lower * *width;
    }
    default: return {};
    }
}

bool short_repeated_alternatives(const Ast& ast) {
    auto transparent = [&](NodeId id) {
        while (ast.nodes[id].kind == Kind::Capture) id = ast.nodes[id].children.front();
        return id;
    };
    const auto& root = ast.nodes[transparent(ast.root)];
    if (root.kind != Kind::Repeat || root.upper) return false;
    const auto& body = ast.nodes[transparent(root.children.front())];
    if (body.kind != Kind::Alt || body.children.empty()) return false;
    const auto width = short_fixed_width(ast, body.children.front());
    if (!width || *width == 0) return false;
    for (std::size_t i = 1; i < body.children.size(); ++i)
        if (short_fixed_width(ast, body.children[i]) != width) return false;
    return true;
}

std::vector<NodeId> validate_component(const Ast& ast) {
    // NFA expansion does not visit the body of {0}. Scanner analysis can visit
    // that body, so validate every structural edge in the selected component
    // before either implementation sees it. A postorder walk also bounds the
    // recursive scanner helpers without using the C++ stack for validation.
    constexpr std::size_t maximum_depth = 2048;
    struct Frame { NodeId id; std::size_t next = 0; };
    std::vector<unsigned char> state(ast.nodes.size(), 0);
    std::vector<std::size_t> heights(ast.nodes.size(), 0);
    std::vector<Frame> stack;
    std::vector<NodeId> order;
    auto enter = [&](NodeId id) {
        if (id >= ast.nodes.size())
            throw std::invalid_argument("regular fragment has an invalid child index");
        if (state[id] == 1)
            throw std::invalid_argument("regular fragment contains a structural cycle");
        if (state[id] == 2) return;
        const auto& node = ast.nodes[id];
        if (node.bind_count != no_id || node.use_count != no_id || node.count_path != no_id)
            throw std::invalid_argument("regular fragment does not support count bindings or occurrence paths");
        switch (node.kind) {
        case Kind::Empty:
        case Kind::CharSet:
        case Kind::Call:
            if (!node.children.empty())
                throw std::invalid_argument("regular fragment leaf has children");
            break;
        case Kind::Capture:
        case Kind::Repeat:
            if (node.children.size() != 1)
                throw std::invalid_argument("regular fragment unary node has invalid arity");
            if (node.kind == Kind::Repeat && node.upper && *node.upper < node.lower)
                throw std::invalid_argument("regular fragment has reversed repetition bounds");
            break;
        case Kind::Concat:
        case Kind::Alt:
        case Kind::Define:
            break;
        default:
            throw std::invalid_argument("regular fragment contains a nonregular node");
        }
        if (stack.size() == maximum_depth)
            throw std::length_error("regular fragment structural depth limit exceeded");
        state[id] = 1;
        stack.push_back({id});
    };
    enter(ast.root);
    while (!stack.empty()) {
        const auto id = stack.back().id;
        const auto& node = ast.nodes[id];
        const auto edges = node.kind == Kind::Define ? 0 :
                           node.kind == Kind::Call ? 1 : node.children.size();
        if (stack.back().next < edges) {
            const auto child = node.kind == Kind::Call ? node.call_target
                                                      : node.children[stack.back().next];
            ++stack.back().next;
            enter(child);
            continue;
        }
        std::size_t height = 1;
        for (std::size_t edge = 0; edge < edges; ++edge) {
            const auto child = node.kind == Kind::Call ? node.call_target : node.children[edge];
            height = std::max(height, heights[child] + 1);
        }
        if (height > maximum_depth)
            throw std::length_error("regular fragment structural depth limit exceeded");
        heights[id] = height;
        state[id] = 2;
        order.push_back(id);
        stack.pop_back();
    }
    return order;
}

Ast component_ast(const Ast& ast, const std::vector<NodeId>& roots) {
    Ast component = ast;
    if (roots.size() == 1) {
        component.root = roots.front();
    } else {
        Node concat;
        concat.kind = Kind::Concat;
        concat.children = roots;
        component.root = component.nodes.size();
        component.nodes.push_back(std::move(concat));
    }
    // Only the reachable selected component was certified regular. Unreachable
    // source nodes and path registries retain their metadata in this copy.
    component.has_references = false;
    component.has_counts = false;
    // Keep has_calls for optional external-adapter eligibility.
    component.has_calls = ast.has_calls;
    component.has_extended = false;
    component.has_occurrence_paths = false;
    return component;
}

} // namespace

struct RegularFragment::Impl {
    std::optional<RegularProgram> fallback;
    std::optional<ExternalRegularProgram> external;
    RegularStatistics compiled;
    std::vector<ByteSet> sequence;
    std::string literal;
    bool direct = false;
    bool native_large_short_repeat = false;
    std::optional<Boundary> boundary;
    std::size_t lower = 1;
    std::optional<std::size_t> upper = 1;

    explicit Impl(const std::bitset<256>& characters) : sequence{ByteSet(characters)}, direct(true) {
        finish_sequence();
    }

    Impl(const Ast& ast, RegularOptions options) {
        const auto order = validate_component(ast);
        fallback.emplace(ast, options);
        (void)fallback->full_match("", &compiled);
        try {
            StructuralView view(ast, order, options.max_fragment_analysis_steps,
                                options.max_nfa_states, compiled.fragment_analysis_steps);
            boundary = view.at(ast.root);
            NodeId body = boundary->alias;
            const auto& root = ast.nodes[body];
            // Only the outer repetition is exposed. A correlated multi-byte
            // alternative is never replaced by independent position classes.
            if (root.kind == Kind::Repeat) {
                lower = root.lower; upper = root.upper;
                body = root.children.front();
            }
            direct = view.emit(body, sequence);
        } catch (const AnalysisDeclined&) {
            direct = false;
            // A completed boundary remains valid if only period extraction ran
            // out of budget. Partial boundary analyses publish nothing.
        }
        if (!direct) {
            sequence.clear();
            if (ExternalRegularProgram::compiled_with_pcre2()) {
                external.emplace(ast, ast.root);
                native_large_short_repeat = short_repeated_alternatives(ast);
            }
        }
        else finish_sequence();
    }

    void finish_sequence() {
        if (!sequence.empty() && std::all_of(sequence.begin(), sequence.end(),
                [](const auto& set) { return set.range && set.span == 0; })) {
            for (const auto& set : sequence) literal += static_cast<char>(set.low);
        }
    }

    bool dimensions(std::size_t bytes) const {
        if (sequence.empty()) return bytes == 0;
        if (bytes % sequence.size()) return false;
        const auto repetitions = bytes / sequence.size();
        return repetitions >= lower && (!upper || repetitions <= *upper);
    }

    std::size_t failure(std::string_view subject) const {
        // A literal period is determined by its first copy and the equality
        // s[p:] == s[:-p]. Both overlapping views are read-only. Successful
        // repetitions need two library comparisons, independent of record
        // width. Otherwise locate the first bad body with the existing scan.
        if (!literal.empty() && subject.size() >= 64 && subject.size() > literal.size() &&
            subject.substr(0, literal.size()) == literal &&
            subject.substr(literal.size()) == subject.substr(0, subject.size() - literal.size()))
            return subject.size();
        // Short literal periods benefit from the same batched strided scans
        // as byte-set records; avoid one library comparison per tiny record.
        if (literal.size() > 8) {
            for (std::size_t i = 0; i < subject.size(); i += literal.size()) {
                const auto piece = subject.substr(i, literal.size());
                if (piece != literal) {
                    std::size_t j = 0;
                    while (j < piece.size() && piece[j] == literal[j]) ++j;
                    return i + j;
                }
            }
            return subject.size();
        }
        return first_failure(subject, sequence);
    }
};

RegularFragment::RegularFragment(std::shared_ptr<const Impl> impl) : impl_(std::move(impl)) {}

RegularFragment::RegularFragment(const Ast& ast, NodeId root, RegularOptions options)
    : RegularFragment(ast, std::vector<NodeId>{root}, options) {}

RegularFragment::RegularFragment(const Ast& ast, const std::vector<NodeId>& roots,
                                 RegularOptions options)
    : impl_(std::make_shared<Impl>(component_ast(ast, roots), options)) {}

RegularFragment RegularFragment::byte_set(const std::bitset<256>& characters) {
    return RegularFragment(std::make_shared<Impl>(characters));
}

RegularStatistics RegularFragment::compiled_statistics() const { return impl_->compiled; }

bool RegularFragment::direct() const { return impl_->direct; }

bool RegularFragment::has_boundary_certificate() const { return impl_->boundary.has_value(); }

bool RegularFragment::boundary_impossible(std::string_view subject) const {
    if (!impl_->boundary) return false;
    const auto& b = *impl_->boundary;
    if (subject.empty()) return !b.nullable;
    if (!b.first[static_cast<unsigned char>(subject.front())] ||
        !b.last[static_cast<unsigned char>(subject.back())]) return true;
    if (subject.size() == 1) return !b.single[static_cast<unsigned char>(subject.front())];
    const auto bit = [&](std::size_t pos) {
        return std::uint64_t{1} << ((17U * static_cast<unsigned char>(subject[pos]) +
                                   static_cast<unsigned char>(subject[pos + 1])) & 63U);
    };
    return !(b.first_pairs & bit(0)) || !(b.last_pairs & bit(subject.size() - 2));
}

bool RegularFragment::external_available() const {
    return impl_->external && impl_->external->available();
}

bool RegularFragment::external_would_run(std::size_t bytes, ExternalRegularPolicy policy) const {
    // Repeated short alternatives can cost more in a backtracking JIT than a
    // tiny native DFA. Measurements establish that effect on long spans, while
    // short spans benefit from JIT. 4096 is a conservative routing heuristic,
    // not a measured universal crossover. Explicit Pcre2 still permits it.
    if (policy == ExternalRegularPolicy::Auto && bytes >= 4096 && impl_->native_large_short_repeat)
        return false;
    return policy != ExternalRegularPolicy::Native && external_available() &&
        (policy == ExternalRegularPolicy::Pcre2 || bytes >= external_auto_min_bytes);
}

const RegularProgram& RegularFragment::regular_program() const {
    if (!impl_->fallback) throw std::logic_error("byte-set fragment has no compiled DFA");
    return *impl_->fallback;
}

bool RegularFragment::byte_class_repetition() const {
    return impl_->direct && impl_->sequence.size() == 1;
}

std::size_t RegularFragment::byte_class_prefix(std::string_view subject) const {
    if (!byte_class_repetition())
        throw std::invalid_argument("fragment is not a repeated one-byte class");
    return impl_->failure(subject);
}

bool RegularFragment::full_match(std::string_view subject, RegularStatistics* statistics,
                                ExternalRegularPolicy policy, std::uint32_t external_match_limit) const {
    if (!impl_->direct) {
        if (external_would_run(subject.size(), policy)) {
            if (const auto result = impl_->external->try_full_match(subject, external_match_limit)) {
                if (statistics) {
                    *statistics = impl_->compiled;
                    statistics->transition_steps = subject.size();
                }
                return *result;
            }
        }
        return impl_->fallback->full_match(subject, statistics);
    }
    if (statistics) {
        *statistics = impl_->compiled;
        // Preserve the DFA's logical whole-span transition accounting even
        // when a direct kernel can stop at the first mismatch.
        statistics->transition_steps = subject.size();
    }
    return impl_->dimensions(subject.size()) && impl_->failure(subject) == subject.size();
}

std::size_t RegularFragment::matching_copies(std::string_view subject, std::size_t width,
                                            std::size_t copies) const {
    if ((width == 0 && !subject.empty()) ||
        (width != 0 && (subject.size() / width != copies || subject.size() % width != 0)))
        throw std::invalid_argument("regular fragment copy dimensions do not cover the subject");
    if (copies == 0) return 0;
    if (impl_->direct) {
        if (!impl_->dimensions(width)) return 0;
        if (width == 0) return copies;
        return impl_->failure(subject) / width;
    }
    return impl_->fallback->matching_copies(subject, width, copies);
}

} // namespace crank
