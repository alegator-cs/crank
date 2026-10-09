#include "ast_machine.hpp"
#include "tests/helpers/relation_reference.hpp"

#include <atomic>
#include <iostream>
#include <optional>
#include <stdexcept>

using namespace crank;
namespace ref = certified_reference;
namespace {
std::size_t checks = 0, inputs = 0, attempts = 0, fallbacks = 0;
std::string context;
void require(bool yes, const char* why) {
    ++checks; if (!yes) throw std::runtime_error(context + ": " + why);
}
OccurrenceMatchOptions base() {
    OccurrenceMatchOptions options;
    // Preserve the historical whole-root trailing-rim comparison.
    options.regional_execution = options.regional_constraints = false;
    // Preserve this historical representation under the newer root tiers.
    options.bounded_word_prediction=options.deterministic_programs=false;
    options.exact_end_spines = false;
    options.max_states = 2'000'000;
    options.max_steps = 8'000'000;
    return options;
}
struct Fixture {
    const char* name;
    const char* pattern;
    std::string alphabet;
    unsigned maximum;
    std::vector<std::string> extra;
    // False forbids this optimization. True leaves certification conservative.
    bool may_activate = true;
};
void fixture(const Fixture& f) {
    const auto ast = parse(f.pattern);
    const detail::AstMachineProgram p(ast);
    auto on = base(), off = on, native = on;
    off.predictive_exact_rims = false;
    native.predictive_recursion = native.predictive_exact_rims = false;
    auto subjects = ref::words(f.alphabet, f.maximum);
    subjects.insert(subjects.end(), f.extra.begin(), f.extra.end());
    for (const auto& input : subjects) {
        ++inputs; context = std::string(f.name) + " input=" + ref::escaped(input);
        const auto language = ref::language(ast, input);
        const bool expected = language.nodes[ast.root].at(0, input.size());
        OccurrenceStatistics stats;
        require(p.full_match(input, native) == expected, "original native disagrees with finite relation");
        require(p.full_match(input, off) == expected, "previous predictor disagrees with finite relation");
        require(p.full_match(input, on, &stats) == expected, "bounded predictor changed language");
        attempts += stats.predictive_rim_attempts;
        if (!f.may_activate) require(!stats.predictive_rim_attempts, "ineligible root entered exact-rim predictor");
        if (input.size() <= 3) {
            for (std::size_t start = 0; start <= input.size(); ++start) {
                std::vector<std::size_t> ends;
                for (auto end = start; end <= input.size(); ++end)
                    if (language.nodes[ast.root].at(start, end)) ends.push_back(end);
                require(p.endpoints(input, start, on, &stats) == ends, "rim changed endpoint relation");
                require(!stats.predictive_rim_attempts, "rim entered endpoint enumeration");
            }
            require(p.first_match(input, on, &stats) == p.first_match(input, off), "rim changed witness/captures");
            require(!stats.predictive_rim_attempts, "rim entered first-witness execution");
            for (NodeId entry = 0; entry < ast.nodes.size(); ++entry) {
                require(p.full_match_node(entry, input, on, &stats) == language.nodes[entry].at(0, input.size()),
                        "rim changed arbitrary-entry exact demand");
                if (entry != ast.root) require(!stats.predictive_rim_attempts, "root rim applied to another entry");
            }
            std::vector<NodeId> entries{ast.root};
            bool any = expected;
            for (const auto capture : ast.capture_nodes) if (capture != no_id) {
                entries.push_back(capture); any |= language.nodes[capture].at(0, input.size());
            }
            require(p.full_match_any(entries, input, on, &stats) == any, "rim changed entry union");
            require(!stats.predictive_rim_attempts, "root rim applied to entry union");
            auto unpruned = on; unpruned.prune = false;
            require(p.full_match(input, unpruned) == expected, "rim relied on pruning for correctness");
            auto unfolded = on; unfolded.predictive_continuation_runs = false;
            require(p.full_match(input, unfolded) == expected, "rim relied on continuation folding");
        }
    }
}

void activation_and_quotas() {
    const auto ast = parse(R"((?<E>a(?&E)b|x)(?:\n(?&E))*\n)");
    const detail::AstMachineProgram p(ast);
    auto on = base(), off = on;
    off.predictive_exact_rims = false;
    for (std::size_t depth : {0U, 1U, 8U, 128U, 2048U}) {
        context = "Event exact trailing newline depth=" + std::to_string(depth);
        const auto event = std::string(depth, 'a') + 'x' + std::string(depth, 'b');
        const auto input = event + '\n' + event + '\n';
        OccurrenceStatistics current, previous;
        require(p.full_match(input, off, &previous), "reference Event input rejected");
        require(p.full_match(input, on, &current), "rim Event input rejected");
        require(current.predictive_rim_attempts == 1, "Event did not activate exact trailing rim");
        require(current.predictive_rim_bytes == 1, "trailing newline not checked exactly once");
        require(!current.return_links, "bounded predictor materialized native return links");
        std::cout << "event_depth=" << depth << " states=" << current.states
                  << " old_states=" << previous.states << " steps=" << current.work_steps << '\n';
    }
    for (const std::string input : {"x\n", "x\nx\n", "axb\n", "x", "x\nx", "x\naab\n", "x\nxx\n"}) {
        context = "rim quota input=" + ref::escaped(input);
        OccurrenceStatistics prior;
        const bool truth = p.full_match(input, off, &prior);
        const std::vector<std::size_t> states{0, 1, 2, 3, 4, 8, 16, 32, prior.states};
        const std::vector<std::size_t> steps{0, 1, 2, 3, 4, 8, 16, 32, 64, 128, prior.work_steps};
        for (const auto s : states) for (const auto w : steps) {
            auto original = off; original.max_states = s; original.max_steps = w;
            auto option = original; option.predictive_exact_rims = true;
            std::optional<bool> expected;
            try { expected = p.full_match(input, original); } catch (const ResourceLimit&) {}
            OccurrenceStatistics stats;
            try { require(p.full_match(input, option, &stats) == truth, "limited rim returned wrong Boolean"); }
            catch (const ResourceLimit&) { require(!expected.has_value(), "rim lost an old successful resource allowance"); }
            require(stats.native_predictive_rim_fallbacks <= 1, "rim retry repeated");
            require(stats.native_acceleration_fallbacks == stats.native_regional_fallbacks + stats.native_spine_fallbacks +
                    stats.native_predictive_rim_fallbacks + stats.native_predictive_fallbacks +
                    stats.native_consumer_fallbacks + stats.native_frontier_fallbacks +
                    stats.native_ordinary_fallbacks, "rim retry tier sum mismatch");
            fallbacks += stats.native_predictive_rim_fallbacks;
        }
    }
    context = "cancelled exact rim";
    std::atomic_bool cancel{true}; bool caught = false;
    try { (void)p.full_match_node(ast.root, "x\n", on, nullptr, &cancel); }
    catch (const detail::AstExecutionCancelled&) { caught = true; }
    require(caught, "cancelled exact rim returned Boolean");
}
} // namespace

int main() {
    try {
        const std::vector<Fixture> all{
            {"Event", R"((?<E>a(?&E)b|x)(?:\n(?&E))*\n)", "abx\n", 4,
                {"x\nx\n", "axb\naaxbb\n", "x\n\n", "x\nx", "x\nx\nx\n"}},
            {"nullable Event", R"((?<E>a(?&E)b|)(?:\n(?&E))*\n)", "ab\n", 5,
                {"\n", "\n\n", "ab\n\naabb\n", "aabb\n\nab\n"}},
            {"nullable root prefix", R"(z?(?<E>a(?&E)b|x)(?:\n(?&E))*\n)", "zx\n", 4,
                {"zaxb\nx\n", "zx\n", "z\n", "zzx\n"}},
            {"class rim", R"((?<E>a(?&E)b|x)(?:[cd](?&E))*[cd])", "abcdx", 3,
                {"xcxd", "xdxc", "axbcaaxbbd", "xcbxd"}},
            {"two-byte rim", R"((?<E>a(?&E)b|x)(?:cd(?&E))*cd)", "abcdx", 3,
                {"xcd", "xcdxcd", "axbcdxcd", "xc", "xdd", "xcdxc"}},
            {"fixed repeated rim", R"((?<E>a(?&E)b|x)(?:c{2}(?&E))*c{2})", "abcx", 3,
                {"xcc", "xccxcc", "axbccxcc", "xc", "xccc"}},
            {"correlated alternative rim", R"((?<E>a(?&E)b|x)(?:(?:cd|dc)(?&E))*(?:cd|dc))", "cdx", 4,
                {"xcd", "xdc", "xcc", "xdd", "xcdxdc", "xcdxcc"}, false},
            {"wrong middle branch", R"((?<E>a(?&E)b|x)(?:\n(?&E))*\n)", "abx\n", 2,
                {"x\naaxb\n", "x\naaxbb\n", "x\nbax\n", "x\naxb\n"}},
            {"recursive root must stay intact", R"((?:a(?R)b|x)\n)", "abx\n", 3,
                {"x\n", "ax\nb\n", "aax\nb\nb\n", "axb\n", "aaxbb\n"}, false},
            {"root alternative declines", R"((?<E>a(?&E)b|x)(?:\n(?&E))*\n|z)", "xz\n", 4,
                {"z", "x\nx\n", "axb\n"}, false},
        };
        for (const auto& f : all) fixture(f);
        activation_and_quotas();
        require(attempts > 0, "no exact-rim activation in differential fixtures");
        std::cout << "{\"status\":\"passed\",\"fixtures\":" << all.size()
                  << ",\"inputs\":" << inputs << ",\"checks\":" << checks
                  << ",\"rim_attempts\":" << attempts << ",\"rim_fallbacks\":" << fallbacks << "}\n";
    } catch (const std::exception& error) {
        std::cerr << "EXACT RIM FAILURE: " << error.what() << '\n'; return 1;
    }
}
