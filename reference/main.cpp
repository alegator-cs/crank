#include "span_regex.hpp"

#include <charconv>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

using crank::experimental::Program;

namespace {
std::string unhex(std::string_view encoded) {
    if (encoded.size() % 2) throw std::invalid_argument("odd hex length");
    std::string result;
    for (std::size_t i = 0; i < encoded.size(); i += 2) {
        unsigned value = 0;
        auto conversion = std::from_chars(encoded.data() + i, encoded.data() + i + 2, value, 16);
        if (conversion.ec != std::errc{} || conversion.ptr != encoded.data() + i + 2)
            throw std::invalid_argument("invalid hex byte");
        result.push_back(static_cast<char>(value));
    }
    return result;
}

void batch() {
    // Hex pattern, tab, hex subject, tab, start. This also supports NUL/newline bytes.
    std::string line;
    while (std::getline(std::cin, line)) {
        try {
            const auto tab = line.find('\t');
            const auto next = line.find('\t', tab == std::string::npos ? 0 : tab + 1);
            if (tab == std::string::npos || next == std::string::npos)
                throw std::invalid_argument("expected three tab-separated fields");
            auto pattern = unhex(std::string_view(line).substr(0, tab));
            auto subject = unhex(std::string_view(line).substr(tab + 1, next - tab - 1));
            std::size_t start = 0;
            auto conversion = std::from_chars(line.data() + next + 1, line.data() + line.size(), start);
            if (conversion.ec != std::errc{} || conversion.ptr != line.data() + line.size())
                throw std::invalid_argument("invalid start");
            Program program(pattern);
            const auto ends = program.endpoints(subject, start);
            const bool matched = program.full_match(subject);
            std::cout << (matched ? '1' : '0') << '\t';
            for (std::size_t i = 0; i < ends.size(); ++i) {
                if (i) std::cout << ',';
                std::cout << ends[i];
            }
            std::cout << '\n';
        } catch (const std::exception& error) {
            std::cout << "ERR\t" << error.what() << '\n';
        }
    }
}
}

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--batch") {
        batch();
        return 0;
    }
    if (argc != 3) {
        std::cerr << "Usage: span_regex PATTERN SUBJECT\n"
                     "       span_regex --batch\n";
        return 2;
    }
    try {
        Program program(argv[1]);
        const auto ends = program.endpoints(argv[2]);
        const bool matched = program.full_match(argv[2]);
        std::cout << (matched ? "MATCH" : "NO MATCH") << "\nEndpoints from 0:";
        for (auto end : ends) std::cout << ' ' << end;
        std::cout << '\n';
        return matched ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
