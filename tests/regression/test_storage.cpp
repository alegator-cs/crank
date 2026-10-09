#include "occurrence_matcher.hpp"

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
std::size_t checks = 0, endpoint_cases = 0, witness_cases = 0;

void require(bool condition, const std::string& context) {
    ++checks;
    if (!condition) throw std::runtime_error(context);
}

crank::OccurrenceMatchOptions strategy(unsigned mask, bool prune = true) {
    crank::OccurrenceMatchOptions options;
    options.compress_returns = (mask & 1U) != 0;
    options.collapse_control = (mask & 2U) != 0;
    options.prune = prune;
    return options;
}

std::string context(std::string_view name, unsigned mask, bool prune) {
    return std::string(name) + " [compression=" + std::to_string(mask & 1U) +
        ", control=" + std::to_string((mask >> 1U) & 1U) +
        ", prune=" + std::to_string(prune) + "]";
}

void check_endpoints(const crank::OccurrenceProgram& program, std::string_view name,
                     const std::string& subject, std::size_t start,
                     const std::vector<std::size_t>& expected) {
    require(std::is_sorted(expected.begin(), expected.end()) &&
            std::adjacent_find(expected.begin(), expected.end()) == expected.end(),
            "test fixture supplies a sorted mathematical endpoint set");
    for (unsigned mask = 0; mask < 4; ++mask)
        for (const bool prune : {false, true}) {
            crank::OccurrenceStatistics statistics;
            const auto tag = context(name, mask, prune);
            require(program.endpoints(subject, start, strategy(mask, prune), &statistics) == expected,
                    tag + ": every expected endpoint, exactly once");
            require(!statistics.limit_reached && statistics.proof_nodes == 0,
                    tag + ": completed recognition without proof storage");
            if (start == 0) {
                const bool matched = std::binary_search(expected.begin(), expected.end(), subject.size());
                require(program.full_match(subject, strategy(mask, prune)) == matched,
                        tag + ": full membership agrees with independent endpoint set");
            }
        }
    ++endpoint_cases;
}

std::vector<std::size_t> interval(std::size_t begin, std::size_t end) {
    std::vector<std::size_t> result;
    for (auto position = begin; position <= end; ++position) result.push_back(position);
    return result;
}

// Exercise empty and singleton chains, a second result, arena growth, and
// later duplicates while other requests allocate cells in the same arena.
void result_cardinality_and_duplicates() {
    const crank::OccurrenceProgram none(crank::parse("(?<S>(?&S))"));
    check_endpoints(none, "unproductive result storage", "aaa", 0, {});
    const crank::OccurrenceProgram one(crank::parse("(?<S>a(?&S)b|)"));
    check_endpoints(one, "single nullable result", "aaa", 0, {0});
    check_endpoints(one, "two nested results", "aaabbb", 0, {0, 6});
    const crank::OccurrenceProgram unary(crank::parse("(?<S>a(?&S)|)"));
    for (const std::size_t length : {0U, 1U, 2U, 3U, 9U, 65U}) {
        const std::string subject(length, 'a');
        check_endpoints(unary, "result chain growth", subject, 0, interval(0, length));
        check_endpoints(unary, "offset result chain growth", "!" + subject + "!", 1,
                        interval(1, length + 1));
    }

    const crank::OccurrenceProgram cyclic(crank::parse("(?<A>(?&A)|a|aa|)"));
    check_endpoints(cyclic, "duplicates after multiple distinct results", "aaa", 0, {0, 1, 2});
    for (unsigned mask = 0; mask < 4; ++mask) {
        crank::OccurrenceStatistics statistics;
        auto options = strategy(mask);
        // This regression specifically needs duplicate native completions.
        // The ordinary endpoint check above also covers the exact DFA union,
        // which can remove these internal duplicates before they exist.
        options.local_regular_regions = false;
        require(cyclic.endpoints("aaa", 0, options, &statistics) ==
                std::vector<std::size_t>{0, 1, 2}, "nullable cycle endpoint set");
        require(statistics.duplicate_completions > 0,
                "duplicate-result regression exercises an actual duplicate completion");
    }
}

// FIFO mode can discover a long endpoint before a shorter one. A chain of
// unit rules delays short alternatives without consuming input. A second
// delayed branch repeats the long endpoint after the short result arrives.
// Keeping only the most recently seen end is unsound in this mode.
std::string delayed_finite_alternatives(bool reverse) {
    constexpr unsigned depth = 56;
    std::string pattern = "(?(DEFINE)";
    for (unsigned index = 0; index < depth; ++index) {
        pattern += "(?<D" + std::to_string(index) + ">";
        if (index + 1 < depth) pattern += "(?&D" + std::to_string(index + 1) + ")";
        pattern += ')';
    }
    pattern += "(?<A>";
    const std::string direct = "aaaaaaaa";
    const std::string delayed = "(?&D0)a|(?&D0)aaaaaaaa|(?&D0)|(?&D0)aaa";
    pattern += reverse ? delayed + '|' + direct : direct + '|' + delayed;
    return pattern + "))(?&A)";
}

void unordered_discovery_and_late_replay() {
    for (const bool reverse : {false, true}) {
        const crank::OccurrenceProgram program(crank::parse(delayed_finite_alternatives(reverse)));
        for (const std::size_t length : {0U, 1U, 2U, 3U, 7U, 8U, 9U}) {
            std::vector<std::size_t> expected;
            for (const std::size_t candidate : {0U, 1U, 3U, 8U})
                if (candidate <= length) expected.push_back(candidate);
            check_endpoints(program, "out-of-order FIFO discoveries and duplicate replay",
                            std::string(length, 'a'), 0, expected);
        }
        check_endpoints(program, "late replay at a nonzero offset", "!aaaaaaaaa!", 1, {1, 2, 4, 9});
    }

    const crank::OccurrenceProgram mixed(crank::parse(
        "(?(DEFINE)(?<A>a(?&A)|)(?<E>(?&E)|))(?:(?&A)b|(?&E)(?&A)c|(?&A))"));
    check_endpoints(mixed, "late subscriber with mixed nullable and consuming alternatives", "aaac", 0,
                    {0, 1, 2, 3, 4});
    check_endpoints(mixed, "late subscriber near miss retains shorter results", "aaad", 0,
                    {0, 1, 2, 3});
}

std::string two_digits(unsigned value) {
    return std::string(1, char('0' + value / 10)) + char('0' + value % 10);
}

std::string fanout_pattern(bool reverse) {
    constexpr unsigned depth = 12, branches = 64;
    std::string pattern = "(?(DEFINE)(?<A>a(?&A)|)";
    for (unsigned index = 0; index < depth; ++index) {
        pattern += "(?<E" + std::to_string(index) + ">";
        if (index + 1 < depth) pattern += "(?&E" + std::to_string(index + 1) + ")";
        pattern += ')';
    }
    pattern += ")(?:";
    for (unsigned position = 0; position < branches; ++position) {
        const auto branch = reverse ? branches - position - 1 : position;
        if (position != 0) pattern += '|';
        // Different lengths of nullable descent stagger subscriptions to A.
        if (branch % (depth + 1) != depth)
            pattern += "(?&E" + std::to_string(branch % (depth + 1)) + ")";
        pattern += "(?&A)b" + two_digits(branch);
    }
    return pattern + ')';
}

std::size_t capture_id(const crank::Ast& ast, std::string_view name) {
    const auto found = std::find(ast.capture_names.begin(), ast.capture_names.end(), name);
    require(found != ast.capture_names.end(), "expected named capture is present");
    return static_cast<std::size_t>(found - ast.capture_names.begin());
}

void many_waiters_survive_return_arena_growth() {
    for (const bool reverse : {false, true}) {
        const auto ast = crank::parse(fanout_pattern(reverse));
        const crank::OccurrenceProgram program(ast);
        for (unsigned branch = 0; branch < 64; ++branch) {
            const auto length = std::size_t(branch % 5);
            const auto subject = std::string(length, 'a') + 'b' + two_digits(branch);
            check_endpoints(program, "64 staggered subscribers retain every branch", subject, 0,
                            {subject.size()});
            const auto padded = '!' + subject + '!';
            check_endpoints(program, "64 staggered subscribers preserve absolute offsets", padded, 1,
                            {subject.size() + 1});
        }
        check_endpoints(program, "subscriber suffix near miss", "aaab64", 0, {});
        check_endpoints(program, "subscriber incomplete suffix", "aaab6", 0, {});
        for (const unsigned branch : {0U, 31U, 63U}) {
            const auto subject = std::string("aaa") + 'b' + two_digits(branch);
            for (unsigned mask = 0; mask < 4; ++mask) {
                const auto witness = program.first_match(subject, strategy(mask));
                require(witness && witness->captures.at(capture_id(ast, "A")) == crank::Span{0, 3},
                        "high fanout witness retains the selected recursive capture");
            }
            ++witness_cases;
        }
    }
}

// Intrusive links must preserve the append order of the old return vector.
// These equally deep callers share the same A request and match the same
// bytes; changing subscriber traversal order changes the selected witness.
void subscriber_order_and_first_proof() {
    for (const bool reverse : {false, true}) {
        const std::string first = reverse ? "second" : "first";
        const std::string second = reverse ? "first" : "second";
        const auto pattern = "(?(DEFINE)(?<A>a|(?<late>a)))(?:(?<" + first +
            ">(?&A))|(?<" + second + ">(?&A)))";
        const auto ast = crank::parse(pattern);
        const crank::OccurrenceProgram program(ast);
        for (unsigned mask = 0; mask < 4; ++mask) {
            const auto witness = program.first_match("a", strategy(mask));
            require(witness.has_value(), "shared-result witness exists");
            require(witness->captures.at(capture_id(ast, first)) == crank::Span{0, 1},
                    "first subscribed caller remains first scalar witness");
            require(!witness->captures.at(capture_id(ast, second)),
                    "later subscribed caller does not overwrite first proof");
            require(!witness->captures.at(capture_id(ast, "late")),
                    "duplicate endpoint retains its first proof rather than a later capture");
            require(witness->captures.at(capture_id(ast, "A")) == crank::Span{0, 1},
                    "first callee result preserves its capture proof");
            require(program.first_match("a", strategy(mask)) == witness,
                    "subscriber-order witness is repeatable");
        }
        ++witness_cases;
    }
}
} // namespace

int main() {
    try {
        result_cardinality_and_duplicates();
        unordered_discovery_and_late_replay();
        many_waiters_survive_return_arena_growth();
        subscriber_order_and_first_proof();
        std::cout << "Compact storage: " << checks << " checks passed; " << endpoint_cases
                  << " independently specified endpoint cases; " << witness_cases
                  << " scalar witness cases.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Compact storage failure after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
