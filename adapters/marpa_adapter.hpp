#pragma once
#include "regex_ast.hpp"
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace comparison {

// A grammar rejected during libmarpa precomputation is not a negative match.
class CompileRejected : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct Result {
    bool matched = false;
    std::vector<std::size_t> endpoints;
    std::size_t earley_items = 0, earley_sets = 0, leo_enabled = 0;
    std::size_t value_steps = 0, tree_size = 0;
    double recognition_us = 0, extraction_us = 0;
    bool bocage_agrees = true;
};

// Direct AST-to-BNF adapter. No Crank length filter or matching backend is used.
// Calls on one instance must be sequential: libmarpa stores events in its grammar.
class MarpaProgram {
public:
    explicit MarpaProgram(const crank::Ast&);
    [[nodiscard]] bool has_cycles() const;
    [[nodiscard]] std::string version() const;
    [[nodiscard]] Result run(std::string_view input, bool endpoints = false,
                             bool extract = false, bool leo = true,
                             bool diagnostics = false,
                             bool check_bocage = false) const;
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
}
