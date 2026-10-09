#include "crank.hpp"
#include <charconv>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
std::size_t number(std::string_view value) {
    std::size_t result = 0;
    auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
        throw std::invalid_argument("invalid unsigned integer");
    return result;
}
std::string unhex(std::string_view text) {
    if (text.size() % 2) throw std::invalid_argument("odd hex length");
    std::string result;
    for (std::size_t i = 0; i < text.size(); i += 2) {
        unsigned byte = 0;
        auto parsed = std::from_chars(text.data() + i, text.data() + i + 2, byte, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + i + 2)
            throw std::invalid_argument("invalid hex byte");
        result += static_cast<char>(byte);
    }
    return result;
}
std::string quote(std::string_view text) {
    constexpr char hex[] = "0123456789abcdef";
    std::string result = "\"";
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') { result += '\\'; result += static_cast<char>(c); }
        else if (c < 32 || c >= 127) {
            result += "\\u00"; result += hex[c / 16]; result += hex[c % 16];
        } else result += static_cast<char>(c);
    }
    return result + '"';
}
void outcomes_json(const std::vector<crank::Outcome>& outcomes, const crank::Ast& ast,
                   std::string_view subject) {
    std::cout << '[';
    for (std::size_t i = 0; i < outcomes.size(); ++i) {
        if (i) std::cout << ',';
        const auto& out = outcomes[i];
        std::cout << "{\"end\":" << out.end << ",\"captures\":[";
        for (std::size_t j = 0; j < out.captures.size(); ++j) {
            if (j) std::cout << ',';
            if (!out.captures[j]) { std::cout << "null"; continue; }
            const auto span = *out.captures[j];
            std::cout << "{\"begin\":" << span.begin << ",\"end\":" << span.end
                      << ",\"text\":" << quote(subject.substr(span.begin, span.end - span.begin)) << '}';
        }
        std::cout << "],\"counts\":{";
        for (std::size_t j = 0; j < out.counts.size(); ++j) {
            if (j) std::cout << ',';
            std::cout << quote(ast.count_names[j]) << ':';
            if (out.counts[j]) std::cout << *out.counts[j]; else std::cout << "null";
        }
        std::cout << "}}";
    }
    std::cout << ']';
}
void trace_json(const std::optional<crank::TracedOutcome>& trace, const crank::Ast& ast,
                std::string_view subject, const crank::RunStatistics& stats) {
    std::cout << "{\"matches\":";
    outcomes_json(trace ? std::vector<crank::Outcome>{trace->outcome} : std::vector<crank::Outcome>{}, ast, subject);
    std::cout << ",\"occurrences\":[";
    if (trace) for (std::size_t i = 0; i < trace->occurrences.size(); ++i) {
        const auto& r = trace->occurrences[i];
        if (i) std::cout << ',';
        const char* kind = "root";
        switch (r.kind) {
        case crank::OccurrenceKind::Root: break;
        case crank::OccurrenceKind::Group: kind = "group"; break;
        case crank::OccurrenceKind::Count: kind = "count"; break;
        case crank::OccurrenceKind::Iteration: kind = "iteration"; break;
        }
        std::cout << "{\"id\":" << r.id << ",\"parent\":";
        if (r.parent == crank::no_id) std::cout << "null"; else std::cout << r.parent;
        std::cout << ",\"kind\":" << quote(kind) << ",\"source_id\":" << r.source_id
                  << ",\"source_node\":" << r.source_node << ",\"name\":" << quote(r.name)
                  << ",\"begin\":" << r.span.begin << ",\"end\":" << r.span.end
                  << ",\"bytes\":" << r.span.end - r.span.begin << ",\"count\":";
        if (r.count) std::cout << *r.count; else std::cout << "null";
        std::cout << ",\"iteration\":";
        if (r.iteration) std::cout << *r.iteration; else std::cout << "null";
        std::cout << ",\"depth\":" << r.depth << ",\"height\":" << r.height
                  << ",\"source_rule\":" << (r.source_rule ? "true" : "false")
                  << ",\"closed\":" << (r.closed ? "true" : "false") << ",\"children\":[";
        for (std::size_t j = 0; j < r.children.size(); ++j) {
            if (j) std::cout << ',';
            std::cout << r.children[j];
        }
        std::cout << "]}";
    }
    const auto& x = stats.occurrence;
    std::cout << "],\"statistics\":{\"backend\":" << quote(stats.backend)
              << ",\"states\":" << x.states << ",\"work_steps\":" << x.work_steps
              << ",\"down_steps\":" << x.down_steps << ",\"up_steps\":" << x.up_steps
              << ",\"pruned\":" << x.pruned << ",\"arithmetic_pruned\":" << x.arithmetic_pruned
              << ",\"path_checks\":" << x.path_checks << ",\"forward_obligations\":" << x.forward_obligations
              << ",\"max_call_depth\":" << x.max_call_depth
              << ",\"occurrences\":" << x.occurrences
              << ",\"limit_reached\":" << (x.limit_reached ? "true" : "false") << "}}";
}
crank::Backend backend(std::string_view value) {
    using B = crank::Backend;
    if (value == "auto") return B::Auto;
    if (value == "dfa") return B::Dfa;
    if (value == "states") return B::States;
    if (value == "greedy") return B::Greedy;
    if (value == "pcre2") return B::Pcre2;
    if (value == "occurrences") return B::Occurrences;
    throw std::invalid_argument("unknown backend");
}
void batch(crank::RunOptions options, bool captures, bool first_only, bool trace, crank::CompileOptions compile) {
    std::string line;
    while (std::getline(std::cin, line)) {
        try {
            auto first = line.find('\t'), second = line.find('\t', first + 1);
            if (first == std::string::npos || second == std::string::npos)
                throw std::invalid_argument("expected hex pattern, hex subject, start");
            const auto pattern = unhex(std::string_view(line).substr(0, first));
            const auto subject = unhex(std::string_view(line).substr(first + 1, second - first - 1));
            const auto start = number(std::string_view(line).substr(second + 1));
            crank::Program program(pattern, compile);
            if (trace) {
                if (start != 0) throw std::invalid_argument("trace batch supports full matching from zero");
                crank::RunStatistics stats;
                const auto result = program.first_trace(subject, options, &stats);
                trace_json(result, program.ast(), subject, stats);
                std::cout << '\n';
            } else if (captures || first_only) {
                if (start != 0) throw std::invalid_argument("capture batch supports full matching from zero");
                std::vector<crank::Outcome> results;
                if (first_only) {
                    if (auto first = program.first_match(subject, options)) results.push_back(std::move(*first));
                } else results = program.outcomes(subject, options);
                outcomes_json(results, program.ast(), subject);
                std::cout << '\n';
            } else {
                auto endpoint_options = options;
                if (endpoint_options.backend == crank::Backend::Greedy)
                    endpoint_options.backend = crank::Backend::Auto;
                const auto ends = program.endpoints(subject, start, endpoint_options);
                const bool result = program.full_match(subject, options);
                std::cout << (result ? '1' : '0') << '\t';
                for (std::size_t i = 0; i < ends.size(); ++i) {
                    if (i) std::cout << ',';
                    std::cout << ends[i];
                }
                std::cout << '\n';
            }
        } catch (const crank::ResourceLimit& error) {
            std::cout << "LIMIT\t" << error.what() << '\n';
        } catch (const std::exception& error) {
            std::cout << "ERR\t" << error.what() << '\n';
        }
    }
}
}

int main(int argc, char** argv) {
    try {
        if (argc == 1) {
            std::string pattern, subject;
            std::cout << "Enter a regex: ";
            if (!std::getline(std::cin, pattern)) return 2;
            std::cout << "Enter input string: ";
            if (!std::getline(std::cin, subject)) return 2;
            crank::Program program(pattern);
            crank::RunStatistics stats;
            const bool matched = program.full_match(subject, {}, &stats);
            std::cout << (matched ? "MATCH" : "NO MATCH") << " [" << stats.backend << "]\n";
            return matched ? 0 : 1;
        }
        crank::RunOptions options;
        crank::CompileOptions compile;
        bool captures = false, first_only = false, trace = false, batch_mode = false, explain = false, print_ends = false;
        std::vector<std::string_view> positional;
        for (int i = 1; i < argc; ++i) {
            const std::string_view arg = argv[i];
            auto next = [&]() -> std::string_view {
                if (++i == argc) throw std::invalid_argument("missing option value");
                return argv[i];
            };
            if (arg == "--backend") options.backend = backend(next());
            else if (arg == "--max-dfa-states") compile.regular.max_dfa_states = number(next());
            else if (arg == "--max-nfa-states") compile.regular.max_nfa_states = number(next());
            else if (arg == "--max-states") options.occurrence.max_states = options.match.max_states = number(next());
            else if (arg == "--max-steps") options.occurrence.max_steps = options.match.max_steps = number(next());
            else if (arg == "--max-repeat") options.occurrence.max_repeat = options.match.max_repeat = number(next());
            else if (arg == "--max-call-depth") options.occurrence.max_call_depth = number(next());
            else if (arg == "--max-occurrences") options.occurrence.max_occurrences = number(next());
            else if (arg == "--max-occurrence-states") options.occurrence.max_states = number(next());
            else if (arg == "--max-occurrence-steps") options.occurrence.max_steps = number(next());
            else if (arg == "--max-occurrence-repeat") options.occurrence.max_repeat = number(next());
            else if (arg == "--max-candidates") options.affine.max_candidates = number(next());
            else if (arg == "--max-validation-bytes") options.affine.max_validation_bytes = number(next());
            else if (arg == "--no-call-sharing") options.occurrence.share_calls = false;
            else if (arg == "--no-prune") { options.optimize = false; options.match.prune = false; options.occurrence.prune = false; }
            else if (arg == "--captures") captures = true;
            else if (arg == "--first") first_only = true;
            else if (arg == "--trace") trace = true;
            else if (arg == "--batch") batch_mode = true;
            else if (arg == "--explain") explain = true;
            else if (arg == "--endpoints") print_ends = true;
            else if (arg == "--") {
                while (++i < argc) positional.emplace_back(argv[i]);
                break;
            } else positional.push_back(arg);
        }
        if (unsigned(captures) + unsigned(first_only) + unsigned(trace) > 1)
            throw std::invalid_argument("choose --first, --captures, or --trace");
        if (batch_mode) { batch(options, captures, first_only, trace, compile); return 0; }
        if (positional.empty() || positional.size() > 2 || (positional.size() == 1 && !explain)) {
            std::cerr << "Usage: crank [--captures|--first|--trace] [--endpoints] [--explain] [--no-prune]\n"
                         "             [--backend auto|dfa|states|greedy|pcre2|occurrences]\n"
                         "             PATTERN SUBJECT\n"
                         "             [--max-states N] [--max-steps N] [--max-repeat N]\n"
                         "             [--max-call-depth N] [--max-occurrences N] [--no-call-sharing]\n"
                         "             [--max-candidates N] [--max-validation-bytes N]\n"
                         "             [--max-nfa-states N] [--max-dfa-states N]\n"
                         "       crank --explain PATTERN\n"
                         "       crank --batch [--captures|--first|--trace] [options]\n";
            return 2;
        }
        crank::Program program{std::string(positional[0]), compile};
        if (explain) std::cout << program.explain();
        if (positional.size() == 1) return 0;
        const auto subject = positional[1];
        crank::RunStatistics stats;
        bool matched;
        if (trace) {
            const auto result = program.first_trace(subject, options, &stats);
            matched = result.has_value(); trace_json(result, program.ast(), subject, stats); std::cout << '\n';
        } else if (captures || first_only) {
            std::vector<crank::Outcome> results;
            if (first_only) {
                if (auto first = program.first_match(subject, options, &stats)) results.push_back(std::move(*first));
            } else results = program.outcomes(subject, options, &stats);
            matched = !results.empty(); outcomes_json(results, program.ast(), subject); std::cout << '\n';
        } else {
            matched = program.full_match(subject, options, &stats);
            std::cout << (matched ? "MATCH" : "NO MATCH") << " [" << stats.backend << "]\n";
        }
        if (print_ends) {
            std::cout << "Endpoints:";
            auto endpoint_options = options;
            if (endpoint_options.backend == crank::Backend::Greedy)
                endpoint_options.backend = crank::Backend::Auto;
            for (auto end : program.endpoints(subject, 0, endpoint_options)) std::cout << ' ' << end;
            std::cout << '\n';
        }
        return matched ? 0 : 1;
    } catch (const crank::ResourceLimit& error) {
        std::cerr << "RESOURCE LIMIT: " << error.what() << '\n'; return 3;
    } catch (const std::exception& error) {
        std::cerr << "ERROR: " << error.what() << '\n'; return 2;
    }
}
