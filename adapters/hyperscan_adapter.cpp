#include "hyperscan_adapter.hpp"
#include "adapter_error.hpp"
#include <climits>
#include <memory>
#include <new>

namespace comparison {

HyperscanProgram::~HyperscanProgram() {
    hs_free_scratch(scratch_); hs_free_database(database_);
}

int HyperscanProgram::found(unsigned, unsigned long long, unsigned long long, unsigned, void* value) {
    *static_cast<bool*>(value) = true; return 1;
}

void HyperscanProgram::prepare(std::string_view pattern) {
    if (database_ || scratch_) throw AdapterFailure("error", "Hyperscan adapter must be prepared once");
    version_ = hs_version();
    if (!pinned_version(version_, "5.4.2"))
        throw AdapterFailure("unsupported", "Hyperscan runtime differs from pinned 5.4.2");
    if (pattern.find('\0') != std::string_view::npos) throw AdapterFailure("unsupported", "literal NUL in pattern");
    const std::string anchored = "\\A(?:" + std::string(pattern) + ")\\z";
    hs_compile_error_t* error = nullptr;
    const auto rc = hs_compile(anchored.c_str(), HS_FLAG_SINGLEMATCH | HS_FLAG_ALLOWEMPTY,
        HS_MODE_BLOCK, nullptr, &database_, &error);
    const std::unique_ptr<hs_compile_error_t, decltype(&hs_free_compile_error)> cleanup(error, hs_free_compile_error);
    if (rc != HS_SUCCESS) {
        const std::string detail = error ? error->message : "Hyperscan compile failed";
        throw AdapterFailure(rc == HS_NOMEM ? "resource_limit" : "unsupported", detail);
    }
    if (hs_alloc_scratch(database_, &scratch_) != HS_SUCCESS) throw std::bad_alloc();
    prepared_ = true;
}

bool HyperscanProgram::full_match(std::string_view subject) {
    if (!prepared_) throw AdapterFailure("error", "unprepared Hyperscan adapter");
    if (subject.size() > UINT_MAX) throw AdapterFailure("unsupported", "Hyperscan block length exceeds API");
    bool hit = false;
    const auto bytes = subject.empty() ? "" : subject.data();
    const auto rc = hs_scan(database_, bytes, static_cast<unsigned>(subject.size()),
        0, scratch_, found, &hit);
    if (rc != HS_SUCCESS && !(rc == HS_SCAN_TERMINATED && hit))
        throw AdapterFailure(rc == HS_NOMEM ? "resource_limit" : "error", "Hyperscan scan code " + std::to_string(rc));
    return hit;
}

} // namespace comparison
