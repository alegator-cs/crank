#pragma once
#include <hs/hs.h>
#include <string>
#include <string_view>

namespace comparison {

// Full-span byte Boolean block scan. Prepare once and reuse sequentially;
// scratch belongs to the instance and must not be shared across callers.
class HyperscanProgram {
public:
    HyperscanProgram() = default;
    ~HyperscanProgram();
    HyperscanProgram(const HyperscanProgram&) = delete;
    HyperscanProgram& operator=(const HyperscanProgram&) = delete;
    void prepare(std::string_view pattern);
    [[nodiscard]] bool full_match(std::string_view subject);
    [[nodiscard]] const std::string& version() const noexcept { return version_; }
private:
    std::string version_;
    bool prepared_ = false;
    hs_database_t* database_ = nullptr;
    hs_scratch_t* scratch_ = nullptr;
    static int found(unsigned, unsigned long long, unsigned long long, unsigned, void*);
};

} // namespace comparison
