#include "crank.hpp"
#include "assisted_matcher.hpp"
#include "adapters/adapter_error.hpp"
#ifdef BENCH_MARPA
#include "adapters/marpa_adapter.hpp"
#endif
#ifdef BENCH_PCRE2
#include "adapters/pcre2_adapter.hpp"
#endif
#ifdef BENCH_HYPERSCAN
#include "adapters/hyperscan_adapter.hpp"
#endif
#include <chrono>
#include <algorithm>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
using Failure = comparison::AdapterFailure;
std::string quote(std::string_view s) {
    std::ostringstream out;
    out << '"';
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') out << '\\' << c;
        else if (c < 32 || c > 126)
            out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(c) << std::dec;
        else out << c;
    }
    return out.str() + '"';
}
std::string read(const char* path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw Failure("error", "cannot read fixture file");
    return {std::istreambuf_iterator<char>(input), {}};
}
double ns(Clock::time_point start) {
    return std::chrono::duration<double, std::nano>(Clock::now() - start).count();
}
double cpu_ns(std::clock_t start) {
    const auto now = std::clock();
    return start == std::clock_t(-1) || now == std::clock_t(-1) ? -1.0
        : double(now - start) * 1e9 / CLOCKS_PER_SEC;
}
void array(std::ostream& out, const std::vector<double>& values, bool nullable = false) {
    out << '[';
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i) out << ',';
        if (nullable && values[i] < 0) out << "null"; else out << values[i];
    }
    out << ']';
}

class Engine {
    std::string mode_;
    std::unique_ptr<crank::Program> crank_;
    crank::RunOptions options_;
    std::unique_ptr<crank::detail::AstMachineProgram> native_ast_;
    std::unique_ptr<crank::AssistedProgram> assisted_;
    std::shared_ptr<const std::string> owned_input_;
    std::size_t workers_ = 1;
    std::size_t helper_steps_ = 1000000, min_native_steps_ = 512, min_input_bytes_ = 1024;
#ifdef BENCH_MARPA
    std::unique_ptr<comparison::MarpaProgram> marpa_;
#endif
#ifdef BENCH_PCRE2
    comparison::PCRE2Program pcre2_;
#endif
#ifdef BENCH_HYPERSCAN
    comparison::HyperscanProgram hyperscan_;
#endif
public:
    std::string version;
    explicit Engine(std::string mode) : mode_(std::move(mode)) {}
    void own_input(const std::string& input) { owned_input_ = std::make_shared<const std::string>(input); }
    void prepare(const std::string& pattern, std::size_t states, std::size_t steps,
                 std::size_t helper_steps, std::size_t min_native_steps, std::size_t min_input_bytes) {
        helper_steps_ = helper_steps; min_native_steps_ = min_native_steps; min_input_bytes_ = min_input_bytes;
        if (mode_ == "native_ast" || mode_ == "assisted2" || mode_ == "assisted4" || mode_ == "assisted_always4") {
            options_.occurrence.max_states = states; options_.occurrence.max_steps = steps;
            auto& o = options_.occurrence;
            // The assisted foreground bypasses these routes. Disable them on
            // both sides so the control reaches the same native request agenda.
            o.compiled_exact_programs = o.bounded_word_prediction = o.deterministic_programs = false;
            o.normalized_exact_programs = o.regional_execution = o.regional_constraints = false;
            o.exact_end_spines = o.predictive_recursion = o.predictive_exact_rims = false;
            const auto ast = crank::parse(pattern);
            if (!crank::AssistedProgram::eligible(ast)) throw Failure("unsupported", "assistance control requires pure AST language");
            if (mode_ == "native_ast") native_ast_ = std::make_unique<crank::detail::AstMachineProgram>(ast);
            else {
                crank::AssistanceOptions assistance;
                workers_ = mode_ == "assisted2" ? 2 : 4;
                assistance.workers = workers_; assistance.min_input_bytes = min_input_bytes_;
                assistance.max_helper_steps = helper_steps_; assistance.min_native_steps = min_native_steps_;
                assistance.policy = mode_ == "assisted_always4" ? crank::AssistancePolicy::Always : crank::AssistancePolicy::Progress;
                assisted_ = std::make_unique<crank::AssistedProgram>(ast, assistance);
            }
            version = "current source; symmetric native agenda control; owned input";
            return;
        }
        if (mode_ == "crank") {
            options_.match.max_states = options_.occurrence.max_states = states;
            options_.match.max_steps = options_.occurrence.max_steps = steps;
            crank_ = std::make_unique<crank::Program>(pattern);
            version = "current source; see build receipt";
            return;
        }
        if (mode_ == "marpa") {
#ifdef BENCH_MARPA
            marpa_ = std::make_unique<comparison::MarpaProgram>(crank::parse(pattern));
            version = marpa_->version();
            if (version != "11.0.13") throw Failure("unsupported", "Libmarpa runtime differs from pinned 11.0.13");
            return;
#else
            throw Failure("unavailable", "Libmarpa was not linked");
#endif
        }
        if (mode_ == "pcre2_interp" || mode_ == "pcre2_jit") {
#ifdef BENCH_PCRE2
            pcre2_.prepare(pattern, mode_ == "pcre2_jit"); version = pcre2_.version();
            return;
#else
            throw Failure("unavailable", "PCRE2 was not linked");
#endif
        }
        if (mode_ == "hyperscan") {
#ifdef BENCH_HYPERSCAN
            hyperscan_.prepare(pattern); version = hyperscan_.version();
            return;
#else
            throw Failure("unavailable", "Hyperscan was not linked");
#endif
        }
        throw Failure("error", "unknown engine");
    }
    bool match(const std::string& subject, bool extra_check = false) {
        if (native_ast_) return native_ast_->full_match(*owned_input_, options_.occurrence);
        if (assisted_) return assisted_->full_match(owned_input_, options_.occurrence);
        if (mode_ == "crank") return crank_->full_match(subject, options_);
#ifdef BENCH_MARPA
        if (mode_ == "marpa") {
            const auto result = marpa_->run(subject, false, false, true, false, extra_check);
            if (!result.bocage_agrees) throw Failure("incorrect", "Libmarpa recognition/bocage disagreement");
            return result.matched;
        }
#endif
#ifdef BENCH_PCRE2
        if (mode_ == "pcre2_interp" || mode_ == "pcre2_jit") return pcre2_.full_match(subject);
#endif
#ifdef BENCH_HYPERSCAN
        if (mode_ == "hyperscan") return hyperscan_.full_match(subject);
#endif
        throw Failure("error", "unprepared engine");
    }
    void drain() { if (assisted_) assisted_->wait_idle(); }
    std::string pool() const {
        if (!assisted_) return "null";
        const auto p = assisted_->pool_statistics();
        std::ostringstream out;
        out << "{\"queries_admitted\":" << p.queries_admitted << ",\"queries_busy\":" << p.queries_busy
            << ",\"queries_policy_skipped\":" << p.queries_policy_skipped
            << ",\"tasks_submitted\":" << p.tasks_submitted << ",\"tasks_finished\":" << p.tasks_finished
            << ",\"tasks_cancelled\":" << p.tasks_cancelled << ",\"tasks_declined\":" << p.tasks_declined
            << ",\"helper_steps\":" << p.helper_steps << ",\"helper_bytes\":" << p.helper_bytes
            << ",\"nodes_excluded\":" << p.nodes_excluded << ",\"root_rejections\":" << p.root_rejections
            << ",\"active_helpers\":" << p.active_helpers << '}';
        return out.str();
    }
    std::string diagnostic(const std::string& subject, bool expected) {
        if (!native_ast_ && !assisted_) return "null";
        crank::AssistedStatistics a; crank::OccurrenceStatistics n;
        const auto before = Clock::now();
        const bool matched = assisted_ ? assisted_->full_match(owned_input_, options_.occurrence, &a)
                                      : native_ast_->full_match(subject, options_.occurrence, &n);
        const auto foreground = ns(before); const auto at_return = pool();
        const auto before_drain = Clock::now(); drain(); const auto tail = ns(before_drain);
        if (matched != expected) throw Failure("incorrect", "untimed diagnostic changed verdict");
        const auto& m = assisted_ ? a.matching : n;
        std::ostringstream out;
        out << "{\"scope\":\"separate untimed query; does not establish helper use in timed queries\""
            << ",\"workers_including_foreground\":" << workers_ << ",\"owned_input\":true"
            << ",\"policy\":" << quote(mode_ == "assisted_always4" ? "Always" : assisted_ ? "Progress" : "none")
            << ",\"min_native_steps\":" << min_native_steps_ << ",\"max_helper_steps\":" << helper_steps_
            << ",\"min_input_bytes\":" << min_input_bytes_
            << ",\"foreground_ns\":" << foreground << ",\"drain_ns\":" << tail
            << ",\"native_work_steps\":" << m.work_steps << ",\"native_limit_reached\":" << (m.limit_reached ? "true" : "false")
            << ",\"admission_decision_code\":" << static_cast<int>(a.decision)
            << ",\"helpers_admitted\":" << a.helpers_admitted << ",\"helpers_finished_at_return\":" << a.helpers_finished_at_return
            << ",\"culled_requests\":" << a.consumption.culled_requests << ",\"culled_tasks\":" << a.consumption.culled_tasks
            << ",\"consumed_root_rejections\":" << a.consumption.root_rejections
            << ",\"root_rejected_at_return\":" << (a.root_rejected_at_return ? "true" : "false")
            << ",\"native_limit_rescued\":" << (a.native_limit_rescued ? "true" : "false")
            << ",\"pool_at_return\":" << at_return << ",\"pool_after_drain\":" << pool() << '}';
        return out.str();
    }
};
std::size_t number(const char* text, std::size_t maximum, bool zero = false) {
    std::string value(text); std::size_t consumed = 0;
    if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos)
        throw Failure("error", "integer argument is invalid");
    const auto result = std::stoull(value, &consumed);
    if ((!result && !zero) || result > maximum || consumed != value.size()) throw Failure("error", "integer argument is out of range");
    return result;
}
}

int main(int argc, char** argv) {
    std::cout << std::setprecision(17);
    double prepare_ns = 0, first_ns = 0;
    std::string phase = "arguments", status = "error", detail, version;
    try {
        if (argc != 15) throw Failure("error", "runner ENGINE PATTERN SUBJECT EXPECTED check|measure ITERATIONS SAMPLES STATES STEPS v2 TARGET_NS HELPER_STEPS MIN_NATIVE_STEPS MIN_INPUT_BYTES");
        const std::string mode = argv[1], expected_text = argv[4], operation = argv[5];
        if (expected_text != "0" && expected_text != "1") throw Failure("error", "expected must be exactly 0 or 1");
        if (operation != "check" && operation != "measure") throw Failure("error", "operation must be check or measure");
        if (std::string_view(argv[10]) != "v2") throw Failure("error", "protocol must be v2");
        const bool expected = expected_text == "1";
        auto iterations = number(argv[6], 1000000, true);
        const auto samples = number(argv[7], 31), target_ns = number(argv[11], 1000000000);
        const auto states = number(argv[8], 1000000000), steps = number(argv[9], 1000000000);
        const auto helper_steps = number(argv[12], 1000000000), min_native_steps = number(argv[13], 1000000000);
        const auto min_input_bytes = number(argv[14], 67108864);
        phase = "read"; const auto pattern = read(argv[2]), subject = read(argv[3]);
        Engine engine(mode);
        // Immutable owned input is made before any preparation/query timer.
        if (mode == "native_ast" || mode.substr(0, 8) == "assisted") engine.own_input(subject);
        phase = "prepare";
        const auto all_start = Clock::now(); const auto all_cpu = std::clock();
        const auto before_prepare = Clock::now();
        try { engine.prepare(pattern, states, steps, helper_steps, min_native_steps, min_input_bytes); }
        catch (...) { prepare_ns = ns(before_prepare); throw; }
        prepare_ns = ns(before_prepare); version = engine.version;
        std::cout << "{\"stage\":\"prepared\",\"prepare_ns\":" << prepare_ns
            << ",\"version\":" << quote(version) << "}\n" << std::flush;
        phase = "correctness";
        const auto before_first = Clock::now();
        const bool first = engine.match(subject, operation == "check"); first_ns = ns(before_first);
        if (first != expected) throw Failure("incorrect", "fixture verdict differs from independent oracle");
        std::vector<double> wall, cpu, batches;
        double calibration_ns = 0; std::size_t calibration_queries = 0;
        bool calibration_capped = false;
        Clock::time_point warm_start{}; std::clock_t warm_cpu = std::clock_t(-1);
        std::string pool_at_warm_start = "null";
        if (operation == "measure") {
            if (!iterations) {
                phase = "calibration"; iterations = 1;
                const auto calibration_start = Clock::now();
                for (;;) {
                    const auto before = Clock::now();
                    for (std::size_t i = 0; i < iterations; ++i)
                        if (engine.match(subject) != expected) throw Failure("incorrect", "calibration changed verdict");
                    const auto elapsed = ns(before); calibration_queries += iterations;
                    if (elapsed >= target_ns || iterations == 1000000) {
                        calibration_capped = elapsed < target_ns; break;
                    }
                    const double factor = std::min(10.0, std::max(2.0, double(target_ns) / std::max(1.0, elapsed)));
                    iterations = std::min<std::size_t>(1000000, static_cast<std::size_t>(iterations * factor));
                }
                calibration_ns = ns(calibration_start);
            }
            phase = "warm";
            pool_at_warm_start = engine.pool(); warm_start = Clock::now(); warm_cpu = std::clock();
            for (std::size_t sample = 0; sample < samples; ++sample) {
                const auto before_cpu = std::clock(); const auto before = Clock::now();
                for (std::size_t i = 0; i < iterations; ++i)
                    if (engine.match(subject) != expected) throw Failure("incorrect", "warm query changed verdict");
                const auto batch = ns(before), cpu_elapsed = cpu_ns(before_cpu);
                batches.push_back(batch); wall.push_back(batch / iterations);
                cpu.push_back(cpu_elapsed < 0 ? -1 : cpu_elapsed / iterations);
            }
        }
        const auto pool_at_timed_return = engine.pool();
        phase = "drain"; const auto before_drain = Clock::now(); engine.drain();
        const auto final_drain_ns = ns(before_drain), cpu_through_drain = cpu_ns(all_cpu);
        const auto wall_through_drain = ns(all_start);
        const auto pool_after_timed_drain = engine.pool();
        const auto warm_wall_through_drain = operation == "measure" ? ns(warm_start) : 0;
        const auto warm_cpu_through_drain = operation == "measure" ? cpu_ns(warm_cpu) : -1;
        phase = "diagnostic"; const auto diagnostic = engine.diagnostic(subject, expected);
        std::cout << "{\"stage\":\"result\",\"status\":\"complete\",\"matched\":" << (first ? "true" : "false")
            << ",\"prepare_ns\":" << prepare_ns << ",\"first_query_ns\":" << first_ns
            << ",\"first_includes_bocage_check\":" << ((mode == "marpa" && operation == "check") ? "true" : "false")
            << ",\"iterations\":" << (operation == "measure" ? iterations : 0)
            << ",\"calibration_ns\":" << calibration_ns << ",\"calibration_queries\":" << calibration_queries
            << ",\"calibration_capped\":" << (calibration_capped ? "true" : "false")
            << ",\"target_batch_ns\":" << target_ns
            << ",\"warm_wall_ns_per_query\":"; array(std::cout, wall);
        std::cout << ",\"warm_cpu_ns_per_query\":"; array(std::cout, cpu, true);
        std::cout << ",\"warm_batch_ns\":"; array(std::cout, batches);
        std::cout << ",\"final_drain_ns\":" << final_drain_ns
            << ",\"wall_ns_prepare_through_timed_drain\":" << wall_through_drain
            << ",\"cpu_ns_prepare_through_timed_drain\":";
        if (cpu_through_drain < 0) std::cout << "null"; else std::cout << cpu_through_drain;
        std::cout << ",\"pool_at_timed_return\":" << pool_at_timed_return
            << ",\"pool_after_timed_drain\":" << pool_after_timed_drain
            << ",\"pool_at_warm_start\":" << pool_at_warm_start
            << ",\"warm_query_count\":" << (operation == "measure" ? iterations * samples : 0)
            << ",\"warm_wall_ns_through_drain\":" << warm_wall_through_drain
            << ",\"warm_cpu_ns_through_drain\":";
        if (warm_cpu_through_drain < 0) std::cout << "null"; else std::cout << warm_cpu_through_drain;
        std::cout << ",\"warm_scope\":\"foreground batches plus final drain; may include helper carry-in from first/calibration\""
            << ",\"assistance_diagnostic\":" << diagnostic;
        std::cout << "}\n"; return 0;
#ifdef BENCH_MARPA
    } catch (const comparison::CompileRejected& error) { status = "unsupported"; detail = error.what();
#endif
    } catch (const crank::ResourceLimit& error) { status = "resource_limit"; detail = error.what();
    } catch (const std::bad_alloc& error) { status = "resource_limit"; detail = error.what();
    } catch (const Failure& error) { status = error.status; detail = error.what();
    } catch (const std::invalid_argument& error) { status = phase == "prepare" ? "unsupported" : "error"; detail = error.what();
    } catch (const std::exception& error) { status = "error"; detail = error.what(); }
    std::cout << "{\"stage\":\"result\",\"status\":" << quote(status)
        << ",\"matched\":null,\"phase\":" << quote(phase) << ",\"detail\":" << quote(detail)
        << ",\"prepare_ns\":" << prepare_ns << "}\n";
    return status == "incorrect" || status == "error" ? 1 : 0;
}
