#include "assisted_matcher.hpp"
#include "ast_machine.hpp"
#include "atomic_exclusions.hpp"
#include "private_culler.hpp"

#include <algorithm>
#include <atomic>
#include <future>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
std::size_t checks = 0;
void require(bool condition, const std::string& context) {
    ++checks;
    if (!condition) throw std::runtime_error(context);
}
template<class Exception, class Function>
void expect_exception(Function&& function, const std::string& context) {
    bool caught = false;
    try { function(); } catch (const Exception&) { caught = true; }
    require(caught, context);
}

using Ends = std::set<std::size_t>;
using Table = std::vector<std::vector<Ends>>;

// Independent finite relational semantics: repeatedly evaluate every node at
// every input boundary, adding spans until nothing changes. This oracle uses
// no native requests, pruning analysis, helper publication or scheduler.
Table oracle(const crank::Ast& ast, std::string_view subject) {
    Table table(ast.nodes.size(), std::vector<Ends>(subject.size() + 1));
    bool changed;
    do {
        changed = false;
        for (crank::NodeId id = 0; id < ast.nodes.size(); ++id) {
            const auto& node = ast.nodes[id];
            for (std::size_t begin = 0; begin <= subject.size(); ++begin) {
                Ends result;
                switch (node.kind) {
                case crank::Kind::Empty:
                case crank::Kind::Define:
                    result.insert(begin);
                    break;
                case crank::Kind::CharSet:
                    if (begin < subject.size() && node.characters.test(
                            static_cast<unsigned char>(subject[begin]))) result.insert(begin + 1);
                    break;
                case crank::Kind::Capture:
                    result = table.at(node.children.at(0)).at(begin);
                    break;
                case crank::Kind::Call:
                    result = table.at(node.call_target).at(begin);
                    break;
                case crank::Kind::Alt:
                    for (auto child : node.children)
                        result.insert(table[child][begin].begin(), table[child][begin].end());
                    break;
                case crank::Kind::Concat:
                    result.insert(begin);
                    for (auto child : node.children) {
                        Ends next;
                        for (auto middle : result)
                            next.insert(table[child][middle].begin(), table[child][middle].end());
                        result = std::move(next);
                    }
                    break;
                case crank::Kind::Repeat: {
                    Ends power{begin};
                    if (node.lower == 0) result.insert(begin);
                    std::size_t count = 0;
                    while (!node.upper || count < *node.upper) {
                        Ends next;
                        for (auto middle : power) {
                            const auto& endpoints = table.at(node.children.at(0)).at(middle);
                            next.insert(endpoints.begin(), endpoints.end());
                        }
                        ++count;
                        if (count >= node.lower) result.insert(next.begin(), next.end());
                        if (next == power) {
                            if (!node.upper || std::max(count, node.lower) <= *node.upper)
                                result.insert(next.begin(), next.end());
                            break;
                        }
                        power = std::move(next);
                    }
                    break;
                }
                case crank::Kind::Reference:
                    throw std::runtime_error("stateful expression reached pure oracle");
                }
                auto& cell = table[id][begin];
                const auto old_size = cell.size();
                cell.insert(result.begin(), result.end());
                changed |= old_size != cell.size();
            }
        }
    } while (changed);
    return table;
}

void sound_publications(const crank::Ast& ast, std::string_view subject,
                        const crank::detail::AtomicExclusions& exclusions,
                        const Table& table, const std::string& context) {
    for (crank::NodeId node = 0; node < ast.nodes.size(); ++node)
        if (exclusions.test(node))
            for (const auto& endpoints : table[node])
                require(endpoints.empty(), context + ": excluded node has no span, including epsilon");
    if (exclusions.root_rejected())
        require(!table[ast.root][0].contains(subject.size()),
                context + ": full-query certificate never rejects a supported root span");
}

void atomic_words() {
    using crank::detail::AtomicExclusions;
    AtomicExclusions empty(0);
    require(empty.node_count() == 0 && !empty.root_rejected(), "empty table starts unknown");
    AtomicExclusions table(257);
    std::atomic_size_t newly_published{0};
    std::vector<std::thread> publishers;
    // Concurrent publishers deliberately contend on the SAME word and bit.
    // Exactly one of the four redundant publications may report a new fact.
    for (unsigned worker = 0; worker < 4; ++worker) publishers.emplace_back([&] {
        for (crank::NodeId node = 0; node < table.node_count(); ++node)
            if (table.exclude(node)) newly_published.fetch_add(1, std::memory_order_relaxed);
    });
    for (auto& publisher : publishers) publisher.join();
    require(newly_published == table.node_count(), "atomic OR neither loses nor duplicates publications");
    for (crank::NodeId node = 0; node < table.node_count(); ++node)
        require(table.test(node) && !table.exclude(node), "exclusion is permanent and idempotent");
    AtomicExclusions scoped(3);
    scoped.reject_root();
    require(scoped.root_rejected(), "whole-input rejection is visible");
    for (crank::NodeId node = 0; node < scoped.node_count(); ++node)
        require(!scoped.test(node), "whole-input rejection does not poison recursive node spans");
    AtomicExclusions batched(65);
    const auto alternating = std::uint64_t{0xaaaaaaaaaaaaaaaaULL};
    require(batched.exclude_word(0, alternating) == alternating,
            "first batch reports every newly excluded node");
    require(batched.exclude_word(0, ~std::uint64_t{0}) == ~alternating,
            "overlapping batch reports only its previously unset bits");
    require(batched.exclude_word(1, 1) == 1 && batched.test(64),
            "final partial word publishes its valid high-index node");
    for (crank::NodeId node = 0; node < batched.node_count(); ++node)
        require(batched.test(node), "batched and individual bit addressing agree");
}

void culler_semantics() {
    using crank::detail::AtomicExclusions;
    using crank::detail::PrivateCuller;
    struct Case { const char* pattern; const char* subject; };
    for (const Case item : {
        Case{"abc(def)*z", "abcz"}, Case{"abc(def)+z", "abcz"},
        Case{"(abc|bc)*", "abcbc"}, Case{"(abc)*(def)*a", "abcadef"},
        Case{"a*ab", "aaaaaab"}, Case{"a*(a|aa)b", "aaaaaab"},
        Case{"(?<S>(?&S))", ""}, Case{"(?<S>(?&S)|)", ""},
        Case{"(?<S>(?&S)){0}", ""}, Case{"(?<S>(?&S)){1}", ""},
        Case{"(?<S>(?&S)a|)", "aaa"}, Case{"(?<S>a(?&S)b|)", "aabb"},
        Case{"a(?R)b|", "aaabbbx"}, Case{"a(?R)b|", "aabb"},
        Case{"(?(DEFINE)(?<A>(?&B)|a)(?<B>(?&A)|))(?&A)", "a"},
        Case{"(?(DEFINE)(?<unused>z))ab", "ab"},
        Case{"(?(DEFINE)(?<unused>z))ab", "z"},
        Case{"(?:a?){2,3}", "aaa"}, Case{"(?:a?){2,3}", "aaaa"},
        Case{"(?:ab|bc)*", "abc"}, Case{"[ab]*", "abba"},
    }) {
        const auto ast = crank::parse(item.pattern);
        const crank::OccurrenceAnalysis analysis(ast);
        const PrivateCuller culler(ast, analysis);
        const auto table = oracle(ast, item.subject);
        const auto context = std::string(item.pattern) + " on '" + item.subject + "'";
        require(culler.available(), context + ": small helper program compiles");
        AtomicExclusions exclusions(ast.nodes.size());
        std::atomic_bool cancel{false};
        PrivateCuller::Stats stats;
        culler.scan_presence(item.subject, cancel, exclusions, {}, stats);
        require(!stats.declined && !stats.cancelled, context + ": small helper finishes within budget");
        sound_publications(ast, item.subject, exclusions, table, context);
        const crank::detail::AstMachineProgram native(ast);
        for (bool compress : {false, true}) {
            crank::OccurrenceMatchOptions options;
            options.compress_returns = compress;
            const bool actual = native.full_match_assisted(item.subject, options, exclusions);
            require(actual == table[ast.root][0].contains(std::string_view(item.subject).size()),
                    context + ": prepublished negatives preserve native recognition");
        }
    }
    // The root is nullable and can match proper subspans of this rejected
    // query. A full-query certificate must remain separate from its node bit.
    const auto ast = crank::parse("a(?R)b|");
    const crank::OccurrenceAnalysis analysis(ast);
    const PrivateCuller culler(ast, analysis);
    AtomicExclusions scoped(ast.nodes.size());
    std::atomic_bool cancel{false};
    PrivateCuller::Stats stats;
    culler.scan_presence("aaabbbx", cancel, scoped, {}, stats);
    require(scoped.root_rejected(), "bad query alphabet produces a whole-input rejection");
    require(!scoped.test(ast.root), "recursive root retains its supported proper subspans");

    std::string broad = "(?:a";
    for (unsigned branch = 0; branch < 48; ++branch) broad += "|b" + std::to_string(branch);
    broad += ')';
    const auto broad_ast = crank::parse(broad);
    const crank::OccurrenceAnalysis broad_analysis(broad_ast);
    const PrivateCuller broad_culler(broad_ast, broad_analysis);
    AtomicExclusions broad_exclusions(broad_ast.nodes.size());
    broad_culler.scan_presence("a", cancel, broad_exclusions, {}, stats);
    require(stats.inactive_nodes > 64, "one completed scan publishes multiple words of dead alternatives");
    sound_publications(broad_ast, "a", broad_exclusions, oracle(broad_ast, "a"),
                       "multiword exclusion publication including final partial word");
}

void partial_work_and_shards() {
    using crank::detail::AtomicExclusions;
    using crank::detail::PrivateCuller;
    const auto ast = crank::parse("abc(def)*z");
    const crank::OccurrenceAnalysis analysis(ast);
    const PrivateCuller culler(ast, analysis);
    const auto table = oracle(ast, "abcz");
    std::atomic_bool cancel{false};
    bool observed_decline = false;
    for (std::size_t budget = 0; budget < 96; ++budget) {
        AtomicExclusions exclusions(ast.nodes.size());
        PrivateCuller::Limits limits;
        limits.max_steps = budget;
        PrivateCuller::Stats stats;
        culler.scan_presence("abcz", cancel, exclusions, limits, stats);
        observed_decline |= stats.declined;
        sound_publications(ast, "abcz", exclusions, table, "truncated presence/productivity scan");
    }
    require(observed_decline, "small helper quotas actually exercised incomplete work");
    for (unsigned mode = 0; mode < 2; ++mode) {
        AtomicExclusions exclusions(ast.nodes.size());
        PrivateCuller::Limits limits;
        if (mode == 0) limits.max_input_bytes = 3;
        cancel.store(mode == 1, std::memory_order_relaxed);
        PrivateCuller::Stats stats;
        culler.scan_presence("abcz", cancel, exclusions, limits, stats);
        require(mode == 0 ? stats.declined : stats.cancelled,
                "byte quota and cancellation each abandon optional work");
        sound_publications(ast, "abcz", exclusions, table, "cancelled or over-quota helper");
    }
    cancel.store(false, std::memory_order_relaxed);
    const auto shard_ast = crank::parse("(?:ab)*");
    const crank::OccurrenceAnalysis shard_analysis(shard_ast);
    const PrivateCuller shard_culler(shard_ast, shard_analysis);
    for (const std::string subject : {"abab", "abaa"}) {
        AtomicExclusions exclusions(shard_ast.nodes.size());
        PrivateCuller::Stats stats;
        // The offending aa pair has its left byte OUTSIDE this shard.
        shard_culler.scan_range(subject, 3, 4, cancel, exclusions, {}, stats);
        require(exclusions.root_rejected() == (subject == "abaa"),
                "shard checks adjacency across its left boundary");
        for (crank::NodeId node = 0; node < shard_ast.nodes.size(); ++node)
            require(!exclusions.test(node), "partial range never publishes global node absence");
    }
}

void publication_during_native_execution() {
    const auto ast = crank::parse("(?:a|aa)*b");
    const crank::detail::AstMachineProgram native(ast);
    const std::string subject(512, 'a');
    for (bool compress : {false, true}) for (bool whole_query : {false, true}) {
        crank::detail::AtomicExclusions exclusions(ast.nodes.size());
        std::promise<void> entered;
        auto started = entered.get_future();
        auto foreground = std::async(std::launch::async, [&] {
            entered.set_value();
            crank::OccurrenceMatchOptions options;
            options.prune = false;
            options.compress_returns = compress;
            return native.full_match_assisted(subject, options, exclusions);
        });
        started.wait();
        for (unsigned attempt = 0; attempt < 64; ++attempt) std::this_thread::yield();
        if (whole_query) exclusions.reject_root();
        else for (crank::NodeId node = 0; node < ast.nodes.size(); ++node)
                if (ast.nodes[node].kind == crank::Kind::CharSet &&
                    ast.nodes[node].characters.count() == 1 && ast.nodes[node].characters.test('b'))
                    exclusions.exclude(node);
        require(!foreground.get(), "late permanent absence preserves the native verdict");
    }
    crank::detail::AtomicExclusions rejected(ast.nodes.size());
    rejected.reject_root();
    crank::OccurrenceMatchOptions zero;
    zero.max_steps = 0;
    require(!native.full_match_assisted(subject, zero, rejected),
            "already-proved whole-query rejection needs no speculative native step");
    crank::detail::AtomicExclusions wrong_size(ast.nodes.size() + 1);
    expect_exception<std::invalid_argument>([&] {
        (void)native.full_match_assisted(subject, {}, wrong_size);
    }, "native assisted entry checks certificate table ownership shape");
}

void cancel_an_active_private_scan() {
    const auto ast = crank::parse("(?:a|z)*");
    const crank::OccurrenceAnalysis analysis(ast);
    const crank::detail::PrivateCuller culler(ast, analysis);
    std::string subject(2 * 1024 * 1024, 'a');
    subject.back() = 'z'; // absence cannot be certified from an unfinished prefix
    crank::detail::AtomicExclusions exclusions(ast.nodes.size());
    std::atomic_bool cancel{false};
    std::promise<void> entered;
    auto started = entered.get_future();
    auto helper = std::async(std::launch::async, [&] {
        entered.set_value();
        crank::detail::PrivateCuller::Stats stats;
        crank::detail::PrivateCuller::Limits limits;
        limits.max_steps = 16 * 1024 * 1024;
        culler.scan_presence(subject, cancel, exclusions, limits, stats);
        return stats;
    });
    started.wait();
    cancel.store(true, std::memory_order_relaxed);
    const auto stats = helper.get();
    require(stats.cancelled || stats.completed, "private scan either observes cancellation or finishes first");
    require(!exclusions.root_rejected(), "stopping a valid query never fabricates a root exclusion");
    // Every node of this expression has a supported span somewhere in the
    // subject. In particular z only occurs at its final byte.
    for (crank::NodeId node = 0; node < ast.nodes.size(); ++node)
        require(!exclusions.test(node), "cancelled scan does not turn unseen suffix occurrences into absence");
}

// Public API checks are kept below the low-level tests so a failure identifies
// whether the proof publisher, native consumer or query-lifetime wrapper broke.
void unchanged_native_budget() {
    const auto ast = crank::parse("(?:a|aa)*b");
    const crank::detail::AstMachineProgram native(ast);
    const std::string subject = std::string(48, 'a') + 'b';
    crank::detail::AtomicExclusions unknown(ast.nodes.size());
    for (bool compress : {false, true}) {
        crank::OccurrenceMatchOptions options;
        options.compress_returns = compress;
        crank::OccurrenceStatistics plain_stats, hooked_stats;
        crank::detail::NativeAssistanceStatistics consumed;
        require(native.full_match(subject, options, &plain_stats), "baseline accepts budget witness");
        require(native.full_match_assisted(subject, options, unknown, &hooked_stats, &consumed),
                "all-unknown certificates preserve baseline membership");
        require(plain_stats.work_steps == hooked_stats.work_steps &&
                plain_stats.states == hooked_stats.states &&
                plain_stats.down_steps == hooked_stats.down_steps &&
                plain_stats.up_steps == hooked_stats.up_steps &&
                plain_stats.completions == hooked_stats.completions,
                "optional atomic reads neither charge nor reorder native work");
        require(consumed.checks > 0 && consumed.culled_requests == 0 &&
                consumed.culled_tasks == 0 && consumed.root_rejections == 0,
                "unknown proof table has lookup overhead but no invented exclusions");
        options.max_steps = plain_stats.work_steps;
        options.max_states = plain_stats.states;
        for (unsigned mode = 0; mode < 2; ++mode) {
            crank::AssistanceOptions assistance;
            assistance.workers = mode == 0 ? 1 : 4;
            assistance.min_input_bytes = 0;
            assistance.max_helper_steps = 0;
            const crank::AssistedProgram program(ast, assistance);
            crank::AssistedStatistics stats;
            require(program.full_match(subject, options, &stats),
                    "zero helper allowance preserves the entire original native budget");
            require(stats.matching.work_steps == plain_stats.work_steps &&
                    stats.matching.states == plain_stats.states && stats.helpers_admitted == 0,
                    "disabled helper work uses the original foreground execution");
        }
    }
}

void public_program_tests() {
    unchanged_native_budget();
    std::vector<std::string> subjects{""};
    for (unsigned length = 0, begin = 0, end = 1; length < 4; ++length) {
        for (auto i = begin; i < end; ++i)
            for (char byte : std::string_view("ab")) subjects.push_back(subjects[i] + byte);
        begin = end;
        end = static_cast<unsigned>(subjects.size());
    }
    const std::vector<std::string> patterns{
        "(?:a|ab)*", "(?:a?|b?){1,3}", "a*ab", "a*(a|aa)b",
        "(?<S>a(?&S)b|)", "(?<S>(?&S)a|)", "a(?R)b|",
        "(?<S>(?&S)(?&S)|a|)", "(?<S>(?&S))",
        "(?(DEFINE)(?<A>(?&B)|a)(?<B>(?&A)|))(?&A)",
        "(?(DEFINE)(?<S>a|))(?:(?&S)b|(?&S)a)",
        "(?(DEFINE)(?<unused>z))a*b*"
    };
    for (const auto& pattern : patterns) {
        const auto ast = crank::parse(pattern);
        require(crank::AssistedProgram::eligible(ast), "pure CFG is assisted-eligible");
        for (const auto workers : {std::size_t(1), std::size_t(2), std::size_t(4)}) {
            crank::AssistanceOptions assistance;
            assistance.workers = workers;
            assistance.min_input_bytes = 0;
            assistance.policy = crank::AssistancePolicy::Always;
            const crank::AssistedProgram program(ast, assistance);
            for (const auto& subject : subjects) {
                const auto table = oracle(ast, subject);
                const bool expected = table[ast.root][0].contains(subject.size());
                crank::AssistedStatistics stats;
                require(program.full_match(subject, {}, &stats) == expected,
                        pattern + ": assisted membership agrees with independent finite relation");
                require(!stats.matching.limit_reached, "small complete query has no resource limit");
                require(stats.helpers_admitted < workers, "caller occupies one configured worker slot");
                // Explicitly drain between cases to exercise helper admission;
                // this maintenance barrier is not part of full_match.
                program.wait_idle();
                const auto pool = program.pool_statistics();
                require(pool.active_helpers == 0 && pool.tasks_submitted == pool.tasks_finished,
                        "idle barrier accounts for every completed or abandoned helper");
            }
        }
    }
    for (const std::string pattern : {"(a)\\1", "a{#n}b{=n}", "a{#n}"}) {
        auto ast = crank::parse(pattern);
        require(!crank::AssistedProgram::eligible(ast), "observable state is ineligible for shared absence");
        expect_exception<std::invalid_argument>([&] { crank::AssistedProgram program(ast); },
                                               "constructor declines observable runtime state");
        ast.has_references = ast.has_counts = ast.has_extended = ast.has_occurrence_paths = false;
        require(!crank::AssistedProgram::eligible(ast), "node-level state cannot bypass eligibility flags");
    }
    {
        crank::AssistanceOptions options;
        options.workers = 4;
        options.min_input_bytes = 0;
        const crank::AssistedProgram program(crank::parse("(?:\\x00a|\\xffb)*"), options);
        require(program.full_match(std::string("\0a\xff" "b", 4)), "owned helper copies preserve binary input");
        require(!program.full_match(std::string("\0a\xff" "a", 4)), "binary mismatch remains a rejection");
        program.wait_idle();
    }
    {
        crank::AssistanceOptions options;
        options.workers = 4;
        options.min_input_bytes = 0;
        const crank::AssistedProgram program(crank::parse("(?:a|aa)*b"), options);
        crank::OccurrenceMatchOptions limited;
        limited.max_steps = 0;
        crank::AssistedStatistics stats;
        expect_exception<crank::ResourceLimit>([&] {
            (void)program.full_match("aaab", limited, &stats);
        }, "foreground budget exhaustion remains unresolved with helpers present");
        require(stats.matching.limit_reached, "public statistics retain foreground limit status");
        require(program.full_match("aaab", {}, &stats), "query remains reusable after cancelled limited work");
        require(!stats.matching.limit_reached, "successful query resets earlier limit statistics");
        program.wait_idle();
    }
}

void query_lifetime_and_concurrency() {
    crank::AssistanceOptions options;
    options.workers = 4;
    options.min_input_bytes = 0;
    options.policy = crank::AssistancePolicy::Always;
    const crank::AssistedProgram original(crank::parse("(?<S>a(?&S)b|)"), options);
    auto copy = original;
    std::vector<std::future<bool>> callers;
    for (unsigned caller = 0; caller < 8; ++caller) callers.push_back(std::async(std::launch::async,
        [copy, caller] {
            for (unsigned repeat = 0; repeat < 12; ++repeat) {
                const auto count = 1 + (caller + repeat) % 16;
                std::string input = std::string(count, 'a') + std::string(count, 'b');
                crank::AssistedStatistics stats;
                if (!copy.full_match(std::string_view(input), {}, &stats)) return false;
                // Reuse the caller's allocation immediately after return; an
                // outstanding helper must own an independent immutable copy.
                std::fill(input.begin(), input.end(), 'x');
                auto owned = std::make_shared<const std::string>(std::string(count, 'a') +
                                                                  std::string(count, 'b'));
                if (!copy.full_match(owned)) return false;
                owned.reset();
                if (!copy.full_match(std::make_shared<const std::string>(
                        std::string(count, 'a') + std::string(count, 'b')))) return false;
                if (copy.full_match(std::string(count, 'a') + std::string(count + 1, 'b'))) return false;
            }
            return true;
        }));
    for (auto& caller : callers) require(caller.get(), "concurrent copied handles isolate query facts and input lifetime");
    original.wait_idle();
    const auto pool = original.pool_statistics();
    require(pool.active_helpers == 0 && pool.tasks_submitted == pool.tasks_finished,
            "concurrent completion leaves no stranded or forgotten helper tasks");
    require(original.full_match(""), "nullable later query survives unrelated earlier exclusions");
    original.wait_idle();
}
} // namespace

int main() {
    try {
        atomic_words();
        culler_semantics();
        partial_work_and_shards();
        publication_during_native_execution();
        cancel_an_active_private_scan();
        public_program_tests();
        query_lifetime_and_concurrency();
        std::cout << "assisted regression: " << checks << " checks\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
