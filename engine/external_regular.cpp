#include "external_regular.hpp"

#include <limits>
#include <utility>

#if defined(CRANK_WITH_PCRE2) && CRANK_WITH_PCRE2
#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>
#endif

namespace crank {
namespace {

#if defined(CRANK_WITH_PCRE2) && CRANK_WITH_PCRE2
// A bounded, syntax-independent translation. Hex escapes have exactly two
// digits, and every set is explicit, so locale, Unicode, newline conventions,
// and metacharacters in input patterns cannot alter the byte language.
class Serializer {
public:
    explicit Serializer(const Ast& ast) : ast_(ast) {
        if (ast.nodes.size() <= maximum_nodes) visiting_.resize(ast.nodes.size());
    }

    std::string run(NodeId root) {
        if (ast_.nodes.size() > maximum_nodes) return {};
        if (!append("\\A(?:") || !node(root, 0) || !append(")\\z")) return {};
        return std::move(pattern_);
    }

private:
    static constexpr std::size_t maximum_nodes = 16384;
    static constexpr std::size_t maximum_visits = 65536;
    static constexpr std::size_t maximum_bytes = 256 * 1024;
    static constexpr std::size_t maximum_depth = 512;
    const Ast& ast_;
    std::vector<bool> visiting_;
    std::string pattern_;
    std::size_t visits_ = 0;

    bool append(std::string_view text) {
        if (text.size() > maximum_bytes - pattern_.size()) return false;
        pattern_.append(text);
        return true;
    }

    bool byte(unsigned value) {
        constexpr char digits[] = "0123456789abcdef";
        const char escaped[]{'\\', 'x', digits[value >> 4], digits[value & 15]};
        return append(std::string_view(escaped, sizeof escaped));
    }

    bool character_set(const std::bitset<256>& set) {
        if (set.none()) return append("(?!)");
        if (set.count() == 1) {
            for (unsigned i = 0; i < 256; ++i)
                if (set[i]) return byte(i);
        }
        if (!append("[")) return false;
        for (unsigned low = 0; low < 256; ++low) {
            if (!set[low]) continue;
            unsigned high = low;
            while (high + 1 < 256 && set[high + 1]) ++high;
            if (!byte(low)) return false;
            if (high > low && (!append("-") || !byte(high))) return false;
            low = high;
        }
        return append("]");
    }

    bool node(NodeId id, std::size_t depth) {
        if (id >= ast_.nodes.size() || depth > maximum_depth || visiting_[id] ||
            visits_ == maximum_visits) return false;
        ++visits_;
        const auto& n = ast_.nodes[id];
        if (n.bind_count != no_id || n.use_count != no_id || n.count_path != no_id) return false;
        visiting_[id] = true;
        const bool accepted = [&] {
            switch (n.kind) {
            case Kind::Empty: return n.children.empty() && append("(?:)");
            case Kind::CharSet: return n.children.empty() && character_set(n.characters);
            case Kind::Capture:
                return n.children.size() == 1 && node(n.children.front(), depth + 1);
            case Kind::Concat:
                for (const auto child : n.children)
                    if (!node(child, depth + 1)) return false;
                return true;
            case Kind::Alt:
                if (n.children.empty()) return append("(?!)");
                if (!append("(?:")) return false;
                for (std::size_t i = 0; i < n.children.size(); ++i) {
                    if (i != 0 && !append("|")) return false;
                    if (!node(n.children[i], depth + 1)) return false;
                }
                return append(")");
            case Kind::Repeat:
                // PCRE2 10.42 uses 16-bit bounded repeat counts. Declining
                // larger counts preserves the native compiler's own policy.
                if (n.children.size() != 1 || n.lower > 65535 ||
                    (n.upper && (*n.upper > 65535 || *n.upper < n.lower))) return false;
                if (!append("(?:") || !node(n.children.front(), depth + 1) || !append(")"))
                    return false;
                if (!n.upper && n.lower == 0) return append("*");
                if (!n.upper && n.lower == 1) return append("+");
                if (n.upper && n.lower == 0 && *n.upper == 1) return append("?");
                if (!append("{") || !append(std::to_string(n.lower))) return false;
                if (!n.upper || *n.upper != n.lower) {
                    if (!append(",")) return false;
                    if (n.upper && !append(std::to_string(*n.upper))) return false;
                }
                return append("}");
            case Kind::Define:
                // Definitions do not execute at the declaration. Their bodies
                // are visited only from an actual call, as in the native AST.
                return append("(?:)");
            case Kind::Call:
                // Pure acyclic calls are inlined with their own continuation.
                // The active-node guard rejects recursion; the visits/output
                // bounds reject exponential DAG expansion without large work.
                return n.children.empty() && node(n.call_target, depth + 1);
            case Kind::Reference: return false;
            default: return false;
            }
        }();
        visiting_[id] = false;
        return accepted;
    }
};

struct MatchScratch {
    pcre2_match_data* data = pcre2_match_data_create(1, nullptr);
    pcre2_match_context* context = pcre2_match_context_create(nullptr);
    pcre2_jit_stack* stack = pcre2_jit_stack_create(32 * 1024, 512 * 1024, nullptr);

    MatchScratch() {
        if (context && stack) pcre2_jit_stack_assign(context, nullptr, stack);
    }
    ~MatchScratch() {
        pcre2_match_data_free(data);
        pcre2_match_context_free(context);
        pcre2_jit_stack_free(stack);
    }
    MatchScratch(const MatchScratch&) = delete;
    MatchScratch& operator=(const MatchScratch&) = delete;
};
#endif

} // namespace

struct ExternalRegularProgram::Impl {
    std::string pattern;
#if defined(CRANK_WITH_PCRE2) && CRANK_WITH_PCRE2
    pcre2_code* code = nullptr;

    Impl(const Ast& ast, NodeId root) : pattern(Serializer(ast).run(root)) {
        if (pattern.empty()) return;
        int error = 0;
        PCRE2_SIZE offset = 0;
        code = pcre2_compile(reinterpret_cast<PCRE2_SPTR>(pattern.data()), pattern.size(),
            PCRE2_NEVER_UTF | PCRE2_NEVER_UCP, &error, &offset, nullptr);
        if (!code) return;
        if (pcre2_jit_compile(code, PCRE2_JIT_COMPLETE) != 0) {
            pcre2_code_free(code);
            code = nullptr;
        }
    }
    ~Impl() { pcre2_code_free(code); }
#else
    Impl(const Ast&, NodeId) {}
#endif
};

ExternalRegularProgram::ExternalRegularProgram(const Ast& ast, NodeId root)
    : impl_(std::make_shared<Impl>(ast, root)) {}

bool ExternalRegularProgram::compiled_with_pcre2() {
#if defined(CRANK_WITH_PCRE2) && CRANK_WITH_PCRE2
    return true;
#else
    return false;
#endif
}

bool ExternalRegularProgram::available() const {
#if defined(CRANK_WITH_PCRE2) && CRANK_WITH_PCRE2
    return impl_->code != nullptr;
#else
    return false;
#endif
}

std::string_view ExternalRegularProgram::serialized_pattern() const { return impl_->pattern; }

std::optional<bool> ExternalRegularProgram::try_full_match(
    std::string_view subject, std::uint32_t match_limit) const {
#if defined(CRANK_WITH_PCRE2) && CRANK_WITH_PCRE2
    if (!impl_->code || subject.size() == std::numeric_limits<PCRE2_SIZE>::max()) return {};
    // There are no generated callouts, so one call cannot recursively re-enter
    // this scratch. Each thread has independent data, context and JIT stack.
    thread_local MatchScratch scratch;
    if (!scratch.data || !scratch.context || !scratch.stack) return {};
    if (pcre2_set_match_limit(scratch.context, match_limit) != 0) return {};
    const auto* bytes = reinterpret_cast<PCRE2_SPTR>(subject.empty() ? "" : subject.data());
    const int result = pcre2_jit_match(impl_->code, bytes, subject.size(), 0, 0,
        scratch.data, scratch.context);
    if (result == PCRE2_ERROR_NOMATCH) return false;
    if (result < 0) return {};
    const auto* offsets = pcre2_get_ovector_pointer(scratch.data);
    if (offsets[0] != 0 || offsets[1] != subject.size()) return {};
    return true;
#else
    (void)subject;
    (void)match_limit;
    return {};
#endif
}

} // namespace crank
