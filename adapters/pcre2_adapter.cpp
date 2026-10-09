#include "pcre2_adapter.hpp"
#include "adapter_error.hpp"
#include <new>

namespace comparison {

PCRE2Program::~PCRE2Program() {
    pcre2_jit_stack_free(jit_stack_); pcre2_match_context_free(context_);
    pcre2_match_data_free(data_); pcre2_code_free(code_);
}

void PCRE2Program::prepare(std::string_view pattern, bool jit) {
    if (code_ || data_ || context_ || jit_stack_)
        throw AdapterFailure("error", "PCRE2 adapter must be prepared once");
    PCRE2_UCHAR text[128]{}; pcre2_config(PCRE2_CONFIG_VERSION, text);
    version_ = reinterpret_cast<char*>(text);
    if (!pinned_version(version_, "10.42"))
        throw AdapterFailure("unsupported", "PCRE2 runtime differs from pinned 10.42");
    int error = 0; PCRE2_SIZE offset = 0;
    // Compile anchors preserve the original (?R) recursion target.
    const auto pattern_bytes = reinterpret_cast<PCRE2_SPTR>(pattern.empty() ? "" : pattern.data());
    code_ = pcre2_compile(pattern_bytes, pattern.size(),
        PCRE2_ANCHORED | PCRE2_ENDANCHORED | PCRE2_NEVER_UTF | PCRE2_NEVER_UCP,
        &error, &offset, nullptr);
    if (!code_) throw AdapterFailure(error == PCRE2_ERROR_HEAP_FAILED ? "resource_limit" : "unsupported",
        "PCRE2 compile code " + std::to_string(error) + " at " + std::to_string(offset));
    data_ = pcre2_match_data_create_from_pattern(code_, nullptr);
    context_ = pcre2_match_context_create(nullptr);
    if (!data_ || !context_) throw std::bad_alloc();
    pcre2_set_match_limit(context_, 10000000); pcre2_set_depth_limit(context_, 1000000);
    pcre2_set_heap_limit(context_, 262144);
    jit_mode_ = jit;
    if (jit_mode_) {
        const int rc = pcre2_jit_compile(code_, PCRE2_JIT_COMPLETE);
        if (rc != 0) throw AdapterFailure(rc == PCRE2_ERROR_NOMEMORY ? "resource_limit" : "unsupported",
            "PCRE2 JIT code " + std::to_string(rc));
        jit_stack_ = pcre2_jit_stack_create(32768, 8 * 1024 * 1024, nullptr);
        if (!jit_stack_) throw std::bad_alloc();
        pcre2_jit_stack_assign(context_, nullptr, jit_stack_);
    }
    prepared_ = true;
}

bool PCRE2Program::full_match(std::string_view subject) {
    if (!prepared_) throw AdapterFailure("error", "unprepared PCRE2 adapter");
    const auto bytes = reinterpret_cast<PCRE2_SPTR>(subject.empty() ? "" : subject.data());
    const int rc = jit_mode_ ? pcre2_jit_match(code_, bytes, subject.size(), 0, 0, data_, context_)
                            : pcre2_match(code_, bytes, subject.size(), 0, PCRE2_NO_JIT, data_, context_);
    if (rc >= 0) {
        const auto* offsets = pcre2_get_ovector_pointer(data_);
        if (offsets[0] != 0 || offsets[1] != subject.size())
            throw AdapterFailure("incorrect", "PCRE2 returned a partial span");
        return true;
    }
    if (rc == PCRE2_ERROR_NOMATCH) return false;
    const bool limit = rc == PCRE2_ERROR_MATCHLIMIT || rc == PCRE2_ERROR_DEPTHLIMIT ||
        rc == PCRE2_ERROR_HEAPLIMIT || rc == PCRE2_ERROR_JIT_STACKLIMIT || rc == PCRE2_ERROR_NOMEMORY;
    throw AdapterFailure(limit ? "resource_limit" : "error", "PCRE2 match code " + std::to_string(rc));
}

} // namespace comparison
