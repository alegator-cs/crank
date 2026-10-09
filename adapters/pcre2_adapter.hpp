#pragma once
#ifndef PCRE2_CODE_UNIT_WIDTH
#define PCRE2_CODE_UNIT_WIDTH 8
#elif PCRE2_CODE_UNIT_WIDTH != 8
#error "The comparison adapter requires the PCRE2 8-bit API"
#endif
#include <pcre2.h>
#include <string>
#include <string_view>

namespace comparison {

// Full-span byte Boolean recognition. Prepare once; calls must be sequential
// because the instance reuses its match-data, context and optional JIT stack.
class PCRE2Program {
public:
    PCRE2Program() = default;
    ~PCRE2Program();
    PCRE2Program(const PCRE2Program&) = delete;
    PCRE2Program& operator=(const PCRE2Program&) = delete;
    void prepare(std::string_view pattern, bool jit);
    [[nodiscard]] bool full_match(std::string_view subject);
    [[nodiscard]] const std::string& version() const noexcept { return version_; }
private:
    std::string version_;
    bool prepared_ = false;
    bool jit_mode_ = false;
    pcre2_code* code_ = nullptr;
    pcre2_match_data* data_ = nullptr;
    pcre2_match_context* context_ = nullptr;
    pcre2_jit_stack* jit_stack_ = nullptr;
};

} // namespace comparison
