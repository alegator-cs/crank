#pragma once

#include "regex_ast.hpp"

#include <cstdint>
#include <memory>

namespace crank {

enum class ExternalRegularPolicy { Native, Auto, Pcre2 };
inline constexpr std::size_t external_auto_min_bytes = 256;
inline constexpr std::uint32_t external_default_match_limit = 1000000;

// Optional Boolean-only PCRE2 JIT adapter. It serializes the parsed byte
// language, never the user's original syntax. No captures, runtime references,
// count bindings cross this boundary. Pure acyclic calls are boundedly inlined;
// no subroutine calls remain in the serialized pattern. Instances share immutable
// compiled code; writable matching scratch belongs to the calling thread.
class ExternalRegularProgram {
public:
    explicit ExternalRegularProgram(const Ast&, NodeId root);
    [[nodiscard]] static bool compiled_with_pcre2();
    [[nodiscard]] bool available() const;
    [[nodiscard]] std::string_view serialized_pattern() const;

    // An absent result means unavailable, declined, or a runtime error/limit.
    // It NEVER means nonmembership. The caller must use its exact fallback.
    [[nodiscard]] std::optional<bool> try_full_match(
        std::string_view,
        std::uint32_t match_limit = external_default_match_limit) const;

private:
    struct Impl;
    std::shared_ptr<const Impl> impl_;
};

} // namespace crank
