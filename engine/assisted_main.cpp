#include "assisted_matcher.hpp"
#include <charconv>
#include <chrono>
#include <iostream>
#include <stdexcept>

namespace {
std::size_t number(std::string_view value) {
    std::size_t result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
        throw std::invalid_argument("expected an unsigned integer");
    return result;
}
std::string quote(std::string_view value) {
    constexpr char hex[] = "0123456789abcdef";
    std::string result = "\"";
    for (unsigned char byte : value) {
        if (byte == '"' || byte == '\\') {
            result += '\\'; result += static_cast<char>(byte);
        } else if (byte < 32 || byte >= 127) {
            result += "\\u00"; result += hex[byte >> 4]; result += hex[byte & 15];
        } else result += static_cast<char>(byte);
    }
    return result + '"';
}
const char* decision(crank::AssistanceDecision value) {
    using D = crank::AssistanceDecision;
    switch (value) {
    case D::Disabled: return "disabled";
    case D::SmallInput: return "small_input";
    case D::LargeInput: return "large_input";
    case D::NoBudget: return "no_budget";
    case D::Unavailable: return "unavailable";
    case D::RootRejected: return "root_rejected";
    case D::Busy: return "busy";
    case D::AllocationDeclined: return "allocation_declined";
    case D::Admitted: return "admitted";
    case D::NoUsefulProof: return "no_useful_proof";
    case D::PolicyBackoff: return "policy_backoff";
    case D::BelowWorkThreshold: return "below_work_threshold";
    }
    return "unknown";
}
void result_json(std::string_view status, const crank::AssistedStatistics& s,
                 const crank::AssistancePoolStatistics& p, double query_us,
                 double drain_us, std::string_view error = {}) {
    std::cout << "{\"status\":" << quote(status) << ",\"matched\":";
    if (status == "match") std::cout << "true";
    else if (status == "no_match") std::cout << "false";
    else std::cout << "null";
    if (!error.empty()) std::cout << ",\"error\":" << quote(error);
    std::cout << ",\"query_us\":" << query_us << ",\"drain_us\":" << drain_us
              << ",\"decision\":" << quote(decision(s.decision)) << ",\"statistics\":{";
    bool first = true;
    const auto field = [&](const char* name, auto value) {
        if (!first) std::cout << ',';
        first = false;
        std::cout << quote(name) << ':' << value;
    };
    field("states", s.matching.states);
    field("work_steps", s.matching.work_steps);
    field("pruned", s.matching.pruned);
    field("limit_reached", s.matching.limit_reached ? "true" : "false");
    field("checks", s.consumption.checks);
    field("culled_requests", s.consumption.culled_requests);
    field("culled_tasks", s.consumption.culled_tasks);
    field("root_rejections", s.consumption.root_rejections);
    field("helpers_admitted", s.helpers_admitted);
    field("helpers_finished_at_return", s.helpers_finished_at_return);
    field("copied_input_bytes", s.copied_input_bytes);
    field("native_steps_before_admission", s.native_steps_before_admission);
    field("root_rejected_at_return", s.root_rejected_at_return ? "true" : "false");
    field("native_limit_rescued", s.native_limit_rescued ? "true" : "false");
    std::cout << "},\"pool\":{";
    first = true;
    field("queries_admitted", p.queries_admitted);
    field("queries_busy", p.queries_busy);
    field("queries_policy_skipped", p.queries_policy_skipped);
    field("queries_no_useful_proof", p.queries_no_useful_proof);
    field("tasks_submitted", p.tasks_submitted);
    field("tasks_finished", p.tasks_finished);
    field("tasks_cancelled", p.tasks_cancelled);
    field("tasks_declined", p.tasks_declined);
    field("helper_steps", p.helper_steps);
    field("helper_bytes", p.helper_bytes);
    field("nodes_excluded", p.nodes_excluded);
    field("root_rejections", p.root_rejections);
    field("active_helpers", p.active_helpers);
    field("max_stop_observation_ns", p.max_stop_observation_ns);
    field("max_cleanup_ns", p.max_cleanup_ns);
    std::cout << "}}\n";
}
}

int main(int argc, char** argv) {
    crank::AssistedStatistics stats;
    crank::AssistancePoolStatistics pool;
    std::unique_ptr<crank::AssistedProgram> program;
    bool drain = false;
    double query_us = 0, drain_us = 0;
    using Clock = std::chrono::steady_clock;
    const auto collect = [&] {
        if (!program) return;
        if (drain) {
            const auto start = Clock::now();
            program->wait_idle();
            drain_us = std::chrono::duration<double, std::micro>(Clock::now() - start).count();
        }
        pool = program->pool_statistics();
    };
    try {
        crank::AssistanceOptions assistance;
        crank::OccurrenceMatchOptions native;
        bool owned = false;
        std::vector<std::string_view> positional;
        for (int i = 1; i < argc; ++i) {
            const std::string_view arg = argv[i];
            const auto next = [&]() -> std::string_view {
                if (++i == argc) throw std::invalid_argument("missing option value");
                return argv[i];
            };
            if (arg == "--threads") assistance.workers = number(next());
            else if (arg == "--assistance-policy") {
                const auto value = next();
                if (value == "always") assistance.policy = crank::AssistancePolicy::Always;
                else if (value == "selective") assistance.policy = crank::AssistancePolicy::Selective;
                else if (value == "progress") assistance.policy = crank::AssistancePolicy::Progress;
                else throw std::invalid_argument("expected always, selective, or progress assistance policy");
            }
            else if (arg == "--min-native-steps") assistance.min_native_steps = number(next());
            else if (arg == "--min-helper-bytes") assistance.min_input_bytes = number(next());
            else if (arg == "--max-helper-bytes") assistance.max_input_bytes = number(next());
            else if (arg == "--helper-steps") assistance.max_helper_steps = number(next());
            else if (arg == "--max-states") native.max_states = number(next());
            else if (arg == "--max-steps") native.max_steps = number(next());
            else if (arg == "--no-prune") native.prune = false;
            else if (arg == "--owned-input") owned = true;
            else if (arg == "--drain") drain = true;
            else if (arg == "--help") {
                std::cout << "Usage: crank_assisted [options] -- PATTERN SUBJECT\n"
                             "  --threads N              Total workers including caller (1..64; default 2)\n"
                             "  --assistance-policy P   always, selective, or progress\n"
                             "  --min-native-steps N    Work before progress admission (default 512)\n"
                             "  --min-helper-bytes N     Admission threshold (default 1024)\n"
                             "  --max-helper-bytes N     Retained input cap (default 64 MiB)\n"
                             "  --helper-steps N         Extra helper work allowance (default 1000000)\n"
                             "  --max-states N           Native state budget\n"
                             "  --max-steps N            Native work budget\n"
                             "  --no-prune               Disable native static pruning\n"
                             "  --owned-input            Retain a prepared immutable input buffer\n"
                             "  --drain                  Separately measure helper reclamation barrier\n"
                             "Pure CFG Boolean recognition. Capture output is omitted; runtime\n"
                             "references and count effects are unsupported.\n"
                             "JSON output; exits: 0 match, 1 no match, 2 error, 3 unresolved limit.\n"
                             "Query time excludes compilation and optional owned-buffer preparation.\n"
                             "Process exit reclaims workers even without --drain.\n";
                return 0;
            } else if (arg == "--") {
                while (++i < argc) positional.emplace_back(argv[i]);
                break;
            } else if (arg.starts_with("--")) throw std::invalid_argument("unknown option");
            else positional.push_back(arg);
        }
        if (positional.size() != 2) throw std::invalid_argument("expected PATTERN and SUBJECT; see --help");
        program = std::make_unique<crank::AssistedProgram>(crank::parse(positional[0]), assistance);
        std::shared_ptr<const std::string> input;
        if (owned) input = std::make_shared<const std::string>(positional[1]);
        const auto start = Clock::now();
        bool matched;
        try {
            matched = owned ? program->full_match(input, native, &stats)
                            : program->full_match(positional[1], native, &stats);
        } catch (...) {
            query_us = std::chrono::duration<double, std::micro>(Clock::now() - start).count();
            throw;
        }
        query_us = std::chrono::duration<double, std::micro>(Clock::now() - start).count();
        collect();
        result_json(matched ? "match" : "no_match", stats, pool, query_us, drain_us);
        return matched ? 0 : 1;
    } catch (const crank::ResourceLimit& error) {
        collect();
        result_json("resource_limit", stats, pool, query_us, drain_us, error.what());
        return 3;
    } catch (const std::exception& error) {
        collect();
        result_json("error", stats, pool, query_us, drain_us, error.what());
        return 2;
    }
}
