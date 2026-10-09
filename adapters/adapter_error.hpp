#pragma once
#include <cctype>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace comparison {

// A compile failure or resource limit is not a negative recognition result.
struct AdapterFailure : std::runtime_error {
    std::string status;
    AdapterFailure(std::string value, std::string message)
        : std::runtime_error(std::move(message)), status(std::move(value)) {}
};

inline bool pinned_version(std::string_view actual, std::string_view pin) {
    return actual.substr(0, pin.size()) == pin &&
        (actual.size() == pin.size() || !std::isdigit(static_cast<unsigned char>(actual[pin.size()])));
}

} // namespace comparison
