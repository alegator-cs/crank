#include "marpa_adapter.hpp"
extern "C" {
#include <marpa.h>
}
#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <map>
#include <set>
#include <utility>

namespace comparison {
namespace {
using Clock = std::chrono::steady_clock;
double micros(Clock::time_point begin, Clock::time_point end) {
    return std::chrono::duration<double, std::micro>(end - begin).count();
}

template<class T, void (*Release)(T)> struct Owned {
    T value{};
    explicit Owned(T v = {}) : value(v) {}
    ~Owned() { if (value) Release(value); }
    Owned(const Owned&) = delete;
    Owned& operator=(const Owned&) = delete;
    operator T() const { return value; }
};
using Grammar = Owned<Marpa_Grammar, marpa_g_unref>;
using Recognizer = Owned<Marpa_Recognizer, marpa_r_unref>;
using Bocage = Owned<Marpa_Bocage, marpa_b_unref>;
using Order = Owned<Marpa_Order, marpa_o_unref>;
using Tree = Owned<Marpa_Tree, marpa_t_unref>;
using Value = Owned<Marpa_Value, marpa_v_unref>;

std::string error_message(Marpa_Grammar g, const char* operation) {
    const char* detail = nullptr;
    const int code = marpa_g_error(g, &detail);
    return std::string(operation) + ": libmarpa error " + std::to_string(code) +
           (detail ? std::string(" (") + detail + ")" : "");
}
int checked(Marpa_Grammar g, int value, const char* operation) {
    if (value < 0) throw std::runtime_error(error_message(g, operation));
    return value;
}

// Register only TOP events. TOP never appears on a right hand side, so its
// completion can only describe a prefix beginning at input position zero.
struct EventState { bool accepted = false, exhausted = false; };
EventState events(Marpa_Grammar g, Marpa_Symbol_ID top) {
    EventState state;
    const int count = checked(g, marpa_g_event_count(g), "event count");
    for (int i = 0; i < count; ++i) {
        Marpa_Event event;
        const int kind = checked(g, marpa_g_event(g, &event, i), "event");
        if ((kind == MARPA_EVENT_SYMBOL_COMPLETED || kind == MARPA_EVENT_SYMBOL_NULLED)
                && marpa_g_event_value(&event) == top)
            state.accepted = true;
        if (kind == MARPA_EVENT_EXHAUSTED) state.exhausted = true;
    }
    return state;
}

bool token_mismatch(int code) {
    return code == MARPA_ERR_UNEXPECTED_TOKEN_ID ||
           code == MARPA_ERR_INACCESSIBLE_TOKEN ||
           code == MARPA_ERR_NO_TOKEN_EXPECTED_HERE;
}
}

struct MarpaProgram::Impl {
    Grammar grammar;
    std::vector<Marpa_Symbol_ID> symbols;
    std::array<Marpa_Symbol_ID, 256> terminals;
    Marpa_Symbol_ID top = -1;
    bool cycles = false;
    // libmarpa rejects duplicate productions. AST alternatives may legitimately
    // contain duplicates, which do not affect recognition or first-tree existence.
    std::set<std::pair<Marpa_Symbol_ID, std::vector<Marpa_Symbol_ID>>> rules;

    explicit Impl(const crank::Ast& ast) : grammar(new_grammar()) {
        terminals.fill(-1);
        if (ast.nodes.empty() || ast.root >= ast.nodes.size())
            throw std::invalid_argument("Marpa adapter: invalid AST root");
        for (const auto& node : ast.nodes) {
            if (node.kind == crank::Kind::Reference || node.bind_count != crank::no_id ||
                node.use_count != crank::no_id || node.count_path != crank::no_id)
                throw std::invalid_argument("Marpa adapter supports context-free AST nodes only");
            for (auto child : node.children)
                if (child >= ast.nodes.size())
                    throw std::invalid_argument("Marpa adapter: invalid AST child");
            if (node.kind == crank::Kind::Call && node.call_target >= ast.nodes.size())
                throw std::invalid_argument("Marpa adapter: invalid call target");
            symbols.push_back(symbol(false));
        }
        checked(grammar, marpa_g_force_valued(grammar), "force valued grammar");
        for (std::size_t i = 0; i < ast.nodes.size(); ++i) lower(ast.nodes[i], symbols[i]);
        top = symbol(false);
        rule(top, {symbols[ast.root]});
        checked(grammar, marpa_g_start_symbol_set(grammar, top), "start symbol");
        checked(grammar, marpa_g_symbol_is_completion_event_set(grammar, top, 1),
                "enable completion event");
        checked(grammar, marpa_g_symbol_is_nulled_event_set(grammar, top, 1),
                "enable nulled event");
        const int status = marpa_g_precompute(grammar);
        if (status < 0) {
            const int code = marpa_g_error(grammar, nullptr);
            if (code == MARPA_ERR_GRAMMAR_HAS_CYCLE && marpa_g_is_precomputed(grammar) == 1) {
                marpa_g_error_clear(grammar);
            } else if (code == MARPA_ERR_UNPRODUCTIVE_START || code == MARPA_ERR_NO_RULES ||
                       code == MARPA_ERR_START_NOT_LHS) {
                throw CompileRejected(error_message(grammar, "precompute"));
            } else {
                throw std::runtime_error(error_message(grammar, "precompute"));
            }
        }
        cycles = checked(grammar, marpa_g_has_cycle(grammar), "grammar cycle flag") != 0;
        rules.clear();
    }

    static Marpa_Grammar new_grammar() {
        Marpa_Config config;
        if (marpa_c_init(&config) < 0)
            throw std::runtime_error("Marpa adapter: configuration initialization failed");
        Marpa_Grammar g = marpa_g_new(&config);
        if (!g) throw std::runtime_error("Marpa adapter: grammar allocation failed");
        return g;
    }

    Marpa_Symbol_ID symbol(bool terminal) {
        const int id = checked(grammar, marpa_g_symbol_new(grammar), "new symbol");
        checked(grammar, marpa_g_symbol_is_terminal_set(grammar, id, terminal ? 1 : 0),
                "set terminal flag");
        return id;
    }
    void rule(Marpa_Symbol_ID lhs, std::vector<Marpa_Symbol_ID> rhs) {
        if (!rules.emplace(lhs, rhs).second) return;
        if (rhs.size() > static_cast<std::size_t>(INT_MAX))
            throw std::length_error("Marpa adapter: production is too long");
        checked(grammar, marpa_g_rule_new(grammar, lhs, rhs.empty() ? nullptr : rhs.data(),
                                          static_cast<int>(rhs.size())), "new rule");
    }

    // Binary helpers keep a numeric quantifier compact. This is solely BNF
    // construction; it does not test input lengths or use Crank arithmetic.
    struct Repeat {
        Impl& owner;
        Marpa_Symbol_ID body;
        std::vector<Marpa_Symbol_ID> powers;
        std::map<std::size_t, Marpa_Symbol_ID> exact_cache, upto_cache;
        Repeat(Impl& o, Marpa_Symbol_ID b) : owner(o), body(b), powers{b} {}
        Marpa_Symbol_ID exact(std::size_t n) {
            if (n == 1) return body;
            if (auto it = exact_cache.find(n); it != exact_cache.end()) return it->second;
            const auto id = owner.symbol(false);
            exact_cache.emplace(n, id);
            std::vector<Marpa_Symbol_ID> rhs;
            std::size_t bits = n, bit = 0;
            while (bits) {
                if (bit == powers.size()) {
                    auto next = owner.symbol(false);
                    owner.rule(next, {powers.back(), powers.back()});
                    powers.push_back(next);
                }
                if (bits & 1) rhs.push_back(powers[bit]);
                bits >>= 1;
                ++bit;
            }
            owner.rule(id, std::move(rhs));
            return id;
        }
        Marpa_Symbol_ID upto(std::size_t n) {
            if (!n) return exact(0);
            if (auto it = upto_cache.find(n); it != upto_cache.end()) return it->second;
            const auto id = owner.symbol(false);
            upto_cache.emplace(n, id);
            const auto half = n / 2;
            owner.rule(id, {upto(half)});
            owner.rule(id, {exact(half + 1), upto((n & 1) ? half : half - 1)});
            return id;
        }
    };

    void lower(const crank::Node& node, Marpa_Symbol_ID lhs) {
        using crank::Kind;
        switch (node.kind) {
        case Kind::Empty: case Kind::Define:
            rule(lhs, {}); break;
        case Kind::CharSet:
            for (std::size_t byte = 0; byte < 256; ++byte) if (node.characters.test(byte)) {
                if (terminals[byte] < 0) terminals[byte] = symbol(true);
                rule(lhs, {terminals[byte]});
            }
            break;
        case Kind::Concat: {
            std::vector<Marpa_Symbol_ID> rhs;
            for (auto child : node.children) rhs.push_back(symbols[child]);
            rule(lhs, std::move(rhs));
            break;
        }
        case Kind::Alt:
            for (auto child : node.children) rule(lhs, {symbols[child]});
            break;
        case Kind::Capture:
            if (node.children.size() != 1) throw std::invalid_argument("invalid capture AST");
            rule(lhs, {symbols[node.children.front()]}); break;
        case Kind::Call:
            rule(lhs, {symbols[node.call_target]}); break;
        case Kind::Repeat: {
            if (node.children.size() != 1) throw std::invalid_argument("invalid repeat AST");
            Repeat repeat(*this, symbols[node.children.front()]);
            if (node.upper) {
                if (*node.upper < node.lower) throw std::invalid_argument("invalid repeat bounds");
                if (*node.upper == node.lower) rule(lhs, {repeat.exact(node.lower)});
                else rule(lhs, {repeat.exact(node.lower), repeat.upto(*node.upper - node.lower)});
            } else {
                const auto star = symbol(false);
                rule(star, {});
                rule(star, {star, repeat.body});
                if (!node.lower) rule(lhs, {star});
                else rule(lhs, {repeat.exact(node.lower), star});
            }
            break;
        }
        case Kind::Reference:
            throw std::invalid_argument("Marpa adapter does not implement backreferences");
        }
    }
};

MarpaProgram::MarpaProgram(const crank::Ast& ast) : impl_(std::make_shared<Impl>(ast)) {}
bool MarpaProgram::has_cycles() const { return impl_->cycles; }
std::string MarpaProgram::version() const {
    int version[3];
    if (marpa_version(version) != MARPA_ERR_NONE)
        throw std::runtime_error("Marpa adapter: version query failed");
    return std::to_string(version[0]) + "." + std::to_string(version[1]) + "." +
           std::to_string(version[2]);
}

Result MarpaProgram::run(std::string_view input, bool want_endpoints, bool extract,
                         bool leo, bool diagnostics, bool check_bocage) const {
    if (input.size() > static_cast<std::size_t>(INT_MAX))
        throw std::length_error("Marpa adapter: input exceeds C API index range");
    Result result;
    const auto g = impl_->grammar.value;
    const bool time_stages = diagnostics || extract;
    const auto begin = time_stages ? Clock::now() : Clock::time_point{};
    Recognizer r(marpa_r_new(g));
    if (!r.value) throw std::runtime_error(error_message(g, "new recognizer"));
    checked(g, _marpa_r_is_use_leo_set(r, leo ? 1 : 0), "set Leo");
    // A full query only needs TOP completion at the final position. Keeping
    // these events inactive earlier avoids collecting every accepted prefix.
    // Empty input retains its start-input nulled event; endpoint/bocage checks
    // retain events at every position. Activation is recognizer-local.
    const bool late_completion = !input.empty() && !want_endpoints && !check_bocage;
    if (late_completion) {
        checked(g, marpa_r_completion_symbol_activate(r, impl_->top, 0),
                "deactivate early completion events");
        checked(g, marpa_r_nulled_symbol_activate(r, impl_->top, 0),
                "deactivate early nulled events");
    }
    checked(g, marpa_r_start_input(r), "start input");
    auto state = late_completion ? EventState{} : events(g, impl_->top);
    std::vector<std::size_t> accepted;
    const bool keep_endpoints = want_endpoints || check_bocage;
    if (keep_endpoints && state.accepted) accepted.push_back(0);
    std::size_t consumed = 0;
    for (unsigned char byte : input) {
        const bool exhausted = late_completion
            ? checked(g, marpa_r_is_exhausted(r), "recognizer exhaustion") != 0
            : state.exhausted;
        if (exhausted || impl_->terminals[byte] < 0) break;
        const int status = marpa_r_alternative(r, impl_->terminals[byte], byte + 1, 1);
        if (status != MARPA_ERR_NONE) {
            if (!token_mismatch(status))
                throw std::runtime_error(error_message(g, "token alternative"));
            marpa_g_error_clear(g);
            break;
        }
        if (late_completion && consumed + 1 == input.size())
            checked(g, marpa_r_completion_symbol_activate(r, impl_->top, 1),
                    "activate final completion event");
        checked(g, marpa_r_earleme_complete(r), "complete earleme");
        ++consumed;
        if (!late_completion || consumed == input.size()) state = events(g, impl_->top);
        if (keep_endpoints && state.accepted) accepted.push_back(consumed);
    }
    result.matched = consumed == input.size() && state.accepted;
    if (time_stages) result.recognition_us = micros(begin, Clock::now());

    // These counters are deliberately outside the recognition interval. The
    // benchmark also runs its primary timing with diagnostics disabled.
    if (diagnostics) {
        const int latest = checked(g, marpa_r_latest_earley_set(r), "latest Earley set");
        result.earley_sets = static_cast<std::size_t>(latest) + 1;
        for (int i = 0; i <= latest; ++i)
            result.earley_items += checked(g, _marpa_r_earley_set_size(r, i), "Earley set size");
        result.leo_enabled = checked(g, _marpa_r_is_use_leo(r), "Leo flag");
    }
    if (check_bocage) {
        const int latest = checked(g, marpa_r_latest_earley_set(r), "latest Earley set");
        for (int i = 0; i <= latest; ++i) {
            Bocage b(marpa_b_new(r, i));
            if (!b.value) {
                if (marpa_g_error(g, nullptr) != MARPA_ERR_NO_PARSE)
                    throw std::runtime_error(error_message(g, "check bocage"));
                marpa_g_error_clear(g);
            }
            const auto position = checked(g, marpa_r_earleme(r, i), "Earley set position");
            const bool event_accepted = std::binary_search(accepted.begin(), accepted.end(),
                                                          static_cast<std::size_t>(position));
            result.bocage_agrees = result.bocage_agrees && (bool(b.value) == event_accepted);
        }
    }
    if (extract && result.matched) {
        const auto extract_begin = Clock::now();
        Bocage b(marpa_b_new(r, -1));
        if (!b.value) throw std::runtime_error(error_message(g, "extract bocage"));
        Order o(marpa_o_new(b));
        if (!o.value) throw std::runtime_error(error_message(g, "new order"));
        Tree t(marpa_t_new(o));
        if (!t.value) throw std::runtime_error(error_message(g, "new tree"));
        checked(g, marpa_t_next(t), "first tree");
        result.tree_size = checked(g, _marpa_t_size(t), "tree size");
        Value v(marpa_v_new(t));
        if (!v.value) throw std::runtime_error(error_message(g, "new valuation"));
        for (;;) {
            const int step = checked(g, marpa_v_step(v), "valuation step");
            if (step == MARPA_STEP_INACTIVE) break;
            ++result.value_steps;
        }
        result.extraction_us = micros(extract_begin, Clock::now());
    }
    if (want_endpoints) result.endpoints = std::move(accepted);
    return result;
}
}
