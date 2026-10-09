#pragma once
#include "deterministic_program.hpp"
#include <cstring>
#include <exception>
#include <span>
#include <type_traits>
#if defined(__x86_64__) && defined(__linux__) && !defined(CRANK_DISABLE_NATIVE_SLR)
#include <sys/mman.h>
#include <unistd.h>
#define CRANK_NATIVE_SLR_HOST 1
#else
#define CRANK_NATIVE_SLR_HOST 0
#endif

namespace crank::detail {

// Optional x86-64 SysV control lowering of an already certified SLR table.
// It never reads ASTs or changes parser actions. Calls that might throw are
// caught by C++ bridges; exceptions never unwind through the generated frame.
// The source certificate must outlive this program. Code is immutable RX after
// publication, owned here, and never keyed by a subject or shared globally.
class NativeSlrProgram {
public:
    static constexpr std::uint32_t compiler_version = 1;
    struct Options {
        std::size_t max_code_bytes = 256 * 1024;
        std::size_t max_analysis_steps = 1'048'576;
        bool executable_memory = true;
    };
    enum class Outcome : unsigned { reject, match, exception, work_limit, state_limit, cancelled, internal_error };
    struct Meter {
        std::size_t work = 0, states = 0, actions = 0;
        std::size_t max_work = std::numeric_limits<std::size_t>::max();
        std::size_t max_states = std::numeric_limits<std::size_t>::max();
        const std::atomic_bool* cancel = nullptr;
    };
    struct Preparation {
        bool ready = false, supported = CRANK_NATIVE_SLR_HOST, limited = false;
        bool executable_memory_failed = false;
        std::size_t analysis_steps = 0, code_bytes = 0, mapped_bytes = 0, retained_bytes = 0;
        std::size_t states = 0, productions = 0;
    };
private:
    using State = std::uint16_t;
    struct Context {
        const unsigned char *position = nullptr, *end = nullptr;
        State *base = nullptr, *top = nullptr, *capacity_end = nullptr;
        DeterministicProgram::Runtime* runtime = nullptr;
        std::size_t allocations = 0, work_allowance = 0, state_allowance = 0, initial_actions = 0;
        const std::atomic_bool* cancel = nullptr;
        bool (*tick)(Context*) noexcept = nullptr;
        bool (*allocate)(Context*) noexcept = nullptr;
        void* callbacks = nullptr;
        std::exception_ptr* exception = nullptr;
        std::vector<State>* spill = nullptr;
    };
    static_assert(std::is_standard_layout_v<Context>);
    const DeterministicProgram& source_;
    void* mapping_ = nullptr;
    Preparation statistics_;
    using Entry = unsigned (*)(Context*);
    static bool poll(Context* c) noexcept { return !c->cancel->load(std::memory_order_relaxed); }
    static bool grow(Context* c) noexcept {
        try {
            const auto size = static_cast<std::size_t>(c->top - c->base);
            const auto capacity = static_cast<std::size_t>(c->capacity_end - c->base);
            if (capacity > std::numeric_limits<std::size_t>::max() / (2 * sizeof(State)))
                throw std::bad_alloc{};
            std::vector<State> next(capacity * 2);
            std::copy_n(c->base, size, next.data()); c->spill->swap(next);
            c->base = c->spill->data(); c->top = c->base + size;
            c->capacity_end = c->base + c->spill->size();
            return true;
        } catch (...) { *c->exception = std::current_exception(); return false; }
    }
    struct Limit {};
#if CRANK_NATIVE_SLR_HOST
    // Intentionally small assembler: fixed registers, imm32 branches, and
    // bounded labels. No instruction optimizer or grammar reasoning lives here.
    struct Emitter {
        enum Register : unsigned { ax, cx, dx, bx, sp, bp, si, di, r8, r9, r10, r11, r12, r13, r14, r15 };
        std::vector<unsigned char> code;
        struct Fix { std::size_t offset, label; };
        std::vector<std::size_t> labels;
        std::vector<Fix> fixes;
        std::size_t cap, work_cap;
        std::size_t& work;
        explicit Emitter(Options options, std::size_t& observed_work) : cap(std::min<std::size_t>(options.max_code_bytes, 256 * 1024)),
            work_cap(std::min<std::size_t>(options.max_analysis_steps, 1'048'576)), work(observed_work) {}
        void charge(std::size_t n = 1) { if (n > work_cap - work) throw Limit{}; work += n; }
        void byte(unsigned value) { charge(); if (code.size() == cap) throw Limit{}; code.push_back(value); }
        void bytes(std::initializer_list<unsigned> values) { for (auto value : values) byte(value); }
        void u32(std::uint32_t value) { for (unsigned i = 0; i < 4; ++i) byte(value >> (i * 8)); }
        void u64(std::uint64_t value) { for (unsigned i = 0; i < 8; ++i) byte(value >> (i * 8)); }
        std::size_t label() { charge(); labels.push_back(std::numeric_limits<std::size_t>::max()); return labels.size() - 1; }
        void bind(std::size_t id) { charge(); if (labels.at(id) != std::numeric_limits<std::size_t>::max()) throw Limit{}; labels[id] = code.size(); }
        void rex(bool wide, unsigned r = 0, unsigned x = 0, unsigned b = 0) {
            const unsigned value = 0x40 | (wide ? 8 : 0) | ((r >> 3) << 2) | ((x >> 3) << 1) | (b >> 3);
            if (value != 0x40) byte(value);
        }
        void mem(unsigned reg, unsigned base, std::int32_t displacement = 0) {
            byte(0x80 | ((reg & 7) << 3) | (base & 7));
            if ((base & 7) == sp) byte(0x24);
            u32(static_cast<std::uint32_t>(displacement));
        }
        void mov(unsigned dst, unsigned src) { rex(true, src, 0, dst); byte(0x89); byte(0xc0 | ((src & 7) << 3) | (dst & 7)); }
        void imm(unsigned reg, std::uint64_t value) { rex(true, 0, 0, reg); byte(0xb8 + (reg & 7)); u64(value); }
        void load(unsigned dst, unsigned base, std::int32_t displacement = 0) { rex(true, dst, 0, base); byte(0x8b); mem(dst, base, displacement); }
        void store(unsigned base, std::int32_t displacement, unsigned src) { rex(true, src, 0, base); byte(0x89); mem(src, base, displacement); }
        void load16(unsigned dst, unsigned base, std::int32_t displacement = 0) { rex(false, dst, 0, base); bytes({0x0f,0xb7}); mem(dst, base, displacement); }
        void store16(unsigned base, unsigned src, std::int32_t offset = 0) { byte(0x66); rex(false, src, 0, base); byte(0x89); mem(src, base, offset); }
        void alu(unsigned opcode, unsigned dst, unsigned src) { rex(true, src, 0, dst); byte(opcode); byte(0xc0 | ((src & 7) << 3) | (dst & 7)); }
        void add(unsigned dst, unsigned src) { alu(0x01, dst, src); }
        void sub(unsigned dst, unsigned src) { alu(0x29, dst, src); }
        void cmp(unsigned a, unsigned b) { alu(0x39, a, b); }
        void test(unsigned r) { alu(0x85, r, r); }
        void immediate(unsigned operation, unsigned dst, std::uint32_t value) { rex(true, 0, 0, dst); byte(0x81); byte(0xc0 | (operation << 3) | (dst & 7)); u32(value); }
        void add_imm(unsigned dst, std::uint32_t n) { immediate(0,dst,n); }
        void subtract(unsigned dst, std::uint32_t n) { immediate(5,dst,n); }
        void compare(unsigned dst, std::uint32_t n) { immediate(7,dst,n); }
        void shift(unsigned dst, unsigned operation, unsigned amount) { rex(true,0,0,dst); bytes({0xc1, 0xc0U | (operation << 3) | (dst & 7), amount}); }
        void jump(std::size_t id) { byte(0xe9); fixes.push_back({code.size(),id}); u32(0); }
        void branch(unsigned condition, std::size_t id) { bytes({0x0f,0x80U + condition}); fixes.push_back({code.size(),id}); u32(0); }
        void call(std::uintptr_t address) { imm(ax,address); bytes({0xff,0xd0}); }
        void call_member(std::size_t offset) { load(ax,r12,offset); bytes({0xff,0xd0}); }
        void finish() {
            for (const auto fix : fixes) {
                charge(); const auto target = labels.at(fix.label);
                if (target == std::numeric_limits<std::size_t>::max()) throw Limit{};
                const auto relative = static_cast<std::int64_t>(target) - static_cast<std::int64_t>(fix.offset + 4);
                if (relative < INT32_MIN || relative > INT32_MAX) throw Limit{};
                const auto value = static_cast<std::uint32_t>(relative);
                for (unsigned i = 0; i < 4; ++i) code.at(fix.offset+i) = static_cast<unsigned char>(value >> (8*i));
            }
        }
    };
    void build(Options options) {
        if (!source_.certified() || !options.executable_memory) return;
        const auto states = source_.statistics_.states, columns = source_.terminals_ + 1;
        if (!states || states > 512 || !columns || columns > 257 || !source_.nonterminals_ ||
            source_.nonterminals_ > 128 || source_.actions_.size() != states * columns ||
            source_.gotos_.size() != states * source_.nonterminals_ || source_.reductions_.empty() ||
            source_.reductions_.size() > 1024) return;
        Emitter e(options, statistics_.analysis_steps);
        for (const auto token : source_.byte_class_) { e.charge(); if (token >= columns - 1) return; }
        for (const auto action : source_.actions_) {
            e.charge();
            if (action == DeterministicProgram::accept_action) continue;
            if (action > 0 && static_cast<std::size_t>(action) > states) return;
            if (action < 0 && static_cast<std::uint64_t>(-static_cast<std::int64_t>(action)) >= source_.reductions_.size()) return;
        }
        for (std::size_t state = 0; state < states; ++state) {
            const auto action = source_.actions_[state * columns + columns - 1];
            if (action > 0 && action != DeterministicProgram::accept_action) return;
        }
        for (const auto next : source_.gotos_) { e.charge(); if (next != 65535 && next >= states) return; }
        for (const auto reduction : source_.reductions_) {
            e.charge(); if (reduction.lhs >= source_.nonterminals_ || reduction.length > 64) return;
        }
        using R = Emitter;
        constexpr unsigned equal=4, not_equal=5, below=2, above_equal=3;
        const auto done=e.label(), reject=e.label(), accept=e.label(), exception=e.label();
        const auto work_limit=e.label(), state_limit=e.label(), cancelled=e.label(), invalid=e.label();
        std::vector<std::size_t> state_labels(states), shift_labels(states), reduce_labels(source_.reductions_.size());
        for(auto& label : state_labels) label=e.label();
        for(auto& label : shift_labels) label=e.label();
        for(auto& label : reduce_labels) label=e.label();
        std::vector<bool> used_shift(states),used_reduce(reduce_labels.size());
        const auto class_pointer = reinterpret_cast<std::uintptr_t>(source_.byte_class_.data());
        const auto goto_pointer = reinterpret_cast<std::uintptr_t>(source_.gotos_.data());
        const auto runtime_load = [&] {};
        const auto increment = [&](std::size_t offset) { runtime_load(); e.load(R::cx,R::r10,offset); e.add_imm(R::cx,1U); e.store(R::r10,offset,R::cx); };
        const auto save_counters = [&] {
            e.store(R::r10,offsetof(DeterministicProgram::Runtime,actions),R::r8);
            e.store(R::r12,offsetof(Context,allocations),R::r9);
        };
        const auto preserve = [&] { e.bytes({0x41,0x50,0x41,0x51,0x41,0x52,0x41,0x53}); };
        const auto restore = [&] { e.bytes({0x41,0x5b,0x41,0x5a,0x41,0x59,0x41,0x58}); };
        const auto helper = [&](std::uintptr_t address) {
            save_counters(); preserve(); e.mov(R::di,R::r12); e.call(address); restore();
            e.bytes({0x84,0xc0}); e.branch(equal,exception);
        };
        const auto callback = [&](std::size_t offset) {
            const auto skip=e.label(); e.load(R::ax,R::r12,offset); e.test(R::ax); e.branch(equal,skip);
            save_counters(); preserve(); e.mov(R::di,R::r12); e.bytes({0xff,0xd0}); restore();
            e.bytes({0x84,0xc0}); e.branch(equal,exception); e.bind(skip);
        };
        const auto tick = [&] {
            // In callback mode the user callback owns metering/cancellation.
            callback(offsetof(Context,tick));

            const auto no_poll=e.label();
            e.load(R::ax,R::r12,offsetof(Context,cancel)); e.test(R::ax); e.branch(equal,no_poll);
            e.load(R::ax,R::r12,offsetof(Context,initial_actions)); e.add(R::ax,R::r8); e.immediate(4,R::ax,63);
            e.test(R::ax); e.branch(not_equal,no_poll);
            save_counters(); preserve(); e.mov(R::di,R::r12); e.call(reinterpret_cast<std::uintptr_t>(&poll)); restore(); e.bytes({0x84,0xc0}); e.branch(equal,cancelled);
            e.bind(no_poll);
            e.load(R::dx,R::r12,offsetof(Context,work_allowance)); e.cmp(R::r8,R::dx); e.branch(above_equal,work_limit);
            e.add_imm(R::r8,1U);
        };
        const auto classify = [&] {
            const auto eof=e.label(),end=e.label(); e.cmp(R::r13,R::r14); e.branch(equal,eof);
            // movzx eax,byte [r13]; movzx ebp,word [classmap+rax*2]
            e.bytes({0x41,0x0f,0xb6,0x45,0x00}); e.imm(R::dx,class_pointer);
            e.bytes({0x0f,0xb7,0x2c,0x42}); e.jump(end);
            e.bind(eof); e.imm(R::bp,source_.terminals_); e.bind(end);
        };
        const auto push = [&] {
            // ax is a valid target state; preserve it across callbacks/growth.
            e.mov(R::r11,R::ax);
            callback(offsetof(Context,allocate));
            e.load(R::dx,R::r12,offsetof(Context,state_allowance));
            e.cmp(R::r9,R::dx); e.branch(above_equal,state_limit); e.add_imm(R::r9,1U);
            const auto capacity=e.label(); e.load(R::dx,R::r12,offsetof(Context,capacity_end)); e.cmp(R::r15,R::dx); e.branch(below,capacity);
            e.store(R::r12,offsetof(Context,top),R::r15); helper(reinterpret_cast<std::uintptr_t>(&grow));
            e.load(R::bx,R::r12,offsetof(Context,base)); e.load(R::r15,R::r12,offsetof(Context,top)); e.bind(capacity);
            e.store16(R::r15,R::r11); e.add_imm(R::r15,2U);
            e.mov(R::cx,R::r15); e.sub(R::cx,R::bx); e.shift(R::cx,5,1); runtime_load();
            e.load(R::dx,R::r10,offsetof(DeterministicProgram::Runtime,peak_stack)); e.cmp(R::cx,R::dx);
            const auto peak=e.label(); e.branch(below,peak); e.store(R::r10,offsetof(DeterministicProgram::Runtime,peak_stack),R::cx); e.bind(peak);
        };
        // ENDBR64 also makes the entry valid under Intel CET indirect-branch tracking.
        e.bytes({0xf3,0x0f,0x1e,0xfa,0x53,0x55,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x83,0xec,0x08});
        e.mov(R::r12,R::di); e.imm(R::r8,0); e.imm(R::r9,0); e.load(R::r10,R::r12,offsetof(Context,runtime)); e.load(R::r13,R::r12,offsetof(Context,position)); e.load(R::r14,R::r12,offsetof(Context,end));
        e.load(R::bx,R::r12,offsetof(Context,base)); e.mov(R::r15,R::bx); e.imm(R::ax,0); push(); classify(); e.jump(state_labels[0]);
        for(std::size_t state=0;state<states;++state) {
            e.bind(state_labels[state]); e.bytes({0xf3,0x0f,0x1e,0xfa}); tick();
            std::map<std::int32_t,std::size_t> frequencies;
            for(std::size_t token=0;token<columns;++token) ++frequencies[source_.actions_[state*columns+token]];
            auto common=frequencies.begin(); for(auto it=frequencies.begin();it!=frequencies.end();++it)if(it->second>common->second)common=it;
            const auto target=[&](std::int32_t action) {
                if(!action)return reject;
                if(action==DeterministicProgram::accept_action)return accept;
                if(action>0){used_shift[action-1]=true;return shift_labels[action-1];}
                used_reduce[-action]=true;return reduce_labels[-action];
            };
            for(std::size_t token=0;token<columns;++token) {
                const auto action=source_.actions_[state*columns+token]; if(action==common->first)continue;
                e.compare(R::bp,token); e.branch(equal,target(action));
            }
            e.jump(target(common->first));
        }
        for(std::size_t state=0;state<states;++state)if(used_shift[state]) {
            e.bind(shift_labels[state]);
            e.imm(R::ax,state); push(); e.add_imm(R::r13,1U); increment(offsetof(DeterministicProgram::Runtime,shifts));
            classify(); e.jump(state_labels[state]);
        }
        // Dynamic gotos use the source table and one immutable relative-offset
        // table. Shift destinations remain direct native branches.
        const auto state_offsets=e.label();
        for(std::size_t i=1;i<reduce_labels.size();++i)if(used_reduce[i]) {
            e.bind(reduce_labels[i]); const auto reduction=source_.reductions_[i];
            e.mov(R::ax,R::r15); e.sub(R::ax,R::bx); e.compare(R::ax,reduction.length*2); e.branch(6,invalid);
            e.subtract(R::r15,reduction.length*2); e.load16(R::ax,R::r15,-2);
            // imul rax,rax,nonterminals; add lhs; load target goto state.
            e.bytes({0x48,0x69,0xc0});e.u32(source_.nonterminals_);e.add_imm(R::ax,static_cast<std::uint32_t>(reduction.lhs));
            e.imm(R::dx,goto_pointer); e.bytes({0x0f,0xb7,0x04,0x42}); e.compare(R::ax,65535);e.branch(equal,invalid);
            push(); increment(offsetof(DeterministicProgram::Runtime,reductions));
            e.load16(R::ax,R::r15,-2);
            // lea rdx,[rip+state_offsets]; movsxd rax,[rdx+rax*4]; add rax,rdx; jmp rax.
            e.bytes({0x48,0x8d,0x15});e.fixes.push_back({e.code.size(),state_offsets});e.u32(0);
            e.bytes({0x48,0x63,0x04,0x82,0x48,0x01,0xd0,0xff,0xe0});
        }
        const auto result=[&](std::size_t label,Outcome outcome){e.bind(label);e.imm(R::ax,static_cast<unsigned>(outcome));e.jump(done);};
        result(reject,Outcome::reject); e.bind(accept);e.cmp(R::r13,R::r14);e.branch(not_equal,reject);e.imm(R::ax,1);e.jump(done);
        result(exception,Outcome::exception);result(work_limit,Outcome::work_limit);result(state_limit,Outcome::state_limit);
        result(cancelled,Outcome::cancelled);result(invalid,Outcome::internal_error);
        e.bind(done);save_counters();e.store(R::r12,offsetof(Context,position),R::r13);e.store(R::r12,offsetof(Context,top),R::r15);
        e.bytes({0x48,0x83,0xc4,0x08,0x41,0x5f,0x41,0x5e,0x41,0x5d,0x41,0x5c,0x5d,0x5b,0xc3});
        while(e.code.size()%4)e.byte(0);
        e.bind(state_offsets);
        for(const auto label:state_labels)e.u32(static_cast<std::uint32_t>(static_cast<std::int32_t>(e.labels[label])-static_cast<std::int32_t>(e.labels[state_offsets])));
        e.finish();statistics_.analysis_steps=e.work;statistics_.code_bytes=e.code.size();
        const auto page=::sysconf(_SC_PAGESIZE);
        if(page<=0||static_cast<std::size_t>(page)>1024*1024){statistics_.executable_memory_failed=true;return;}
        const auto mapped=(e.code.size()+page-1)/page*page;
        void* memory=::mmap(nullptr,mapped,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        if(memory==MAP_FAILED){statistics_.executable_memory_failed=true;return;}
        std::memcpy(memory,e.code.data(),e.code.size());
        __builtin___clear_cache(static_cast<char*>(memory),static_cast<char*>(memory)+e.code.size());
        if(::mprotect(memory,mapped,PROT_READ|PROT_EXEC)!=0){::munmap(memory,mapped);statistics_.executable_memory_failed=true;return;}
        mapping_=memory;statistics_.mapped_bytes=mapped;statistics_.retained_bytes=sizeof(*this)+mapped;
        statistics_.states=states;statistics_.productions=source_.reductions_.size();statistics_.ready=true;
    }
#endif
    Outcome invoke(std::string_view subject,std::size_t begin,std::size_t end,Context& c) const {
        if(!ready()||begin>end||end>subject.size())throw std::invalid_argument("invalid native SLR demand");
        // Avoid pointer arithmetic on null data for the default empty view.
        static const unsigned char empty=0;
        const auto* data=subject.data()?reinterpret_cast<const unsigned char*>(subject.data()):&empty;
        c.position=data+begin;c.end=data+end;
#if CRANK_NATIVE_SLR_HOST
        return static_cast<Outcome>(reinterpret_cast<Entry>(mapping_)(&c));
#else
        return Outcome::internal_error;
#endif
    }
public:
    explicit NativeSlrProgram(const DeterministicProgram& source) : NativeSlrProgram(source,Options{}) {}
    NativeSlrProgram(const DeterministicProgram& source,Options options) : source_(source) {
        statistics_.retained_bytes=sizeof(*this);
#if CRANK_NATIVE_SLR_HOST
        try{build(options);}catch(const Limit&){statistics_.limited=true;}catch(const std::bad_alloc&){statistics_.limited=true;}
#else
        (void)options;
#endif
    }
    NativeSlrProgram(const NativeSlrProgram&)=delete;
    NativeSlrProgram& operator=(const NativeSlrProgram&)=delete;
    ~NativeSlrProgram(){
#if CRANK_NATIVE_SLR_HOST
        if(mapping_)::munmap(mapping_,statistics_.mapped_bytes);
#endif
    }
    [[nodiscard]] bool ready()const{return statistics_.ready;}
    [[nodiscard]] bool certified()const{return ready();}
    [[nodiscard]] const Preparation& statistics()const{return statistics_;}
    [[nodiscard]] std::size_t compilation_steps()const{return statistics_.analysis_steps;}
    [[nodiscard]] std::size_t code_bytes()const{return statistics_.code_bytes;}
    [[nodiscard]] std::size_t retained_bytes()const{return statistics_.retained_bytes;}
    [[nodiscard]] std::span<const unsigned char> code()const{return {static_cast<const unsigned char*>(mapping_),ready()?code_bytes():0};}
    template<class Tick,class Allocate>
    bool execute(std::string_view subject,std::size_t begin,std::size_t end,Tick&& tick,Allocate&& allocate,DeterministicProgram::Runtime& runtime)const {
        if(!ready()||begin>end||end>subject.size())throw std::invalid_argument("invalid native SLR demand");
        runtime={};std::array<State,64> local;std::vector<State> spill;std::exception_ptr exception;
        struct Callbacks{Tick& tick;Allocate& allocate;};Callbacks callbacks{tick,allocate};
        Context c;c.base=local.data();c.capacity_end=c.base+local.size();c.runtime=&runtime;c.spill=&spill;c.exception=&exception;c.callbacks=&callbacks;
        c.work_allowance=c.state_allowance=std::numeric_limits<std::size_t>::max();
        c.tick=[](Context* c)noexcept{try{static_cast<Callbacks*>(c->callbacks)->tick();return true;}catch(...){*c->exception=std::current_exception();return false;}};
        c.allocate=[](Context* c)noexcept{try{static_cast<Callbacks*>(c->callbacks)->allocate();return true;}catch(...){*c->exception=std::current_exception();return false;}};
        const auto outcome=invoke(subject,begin,end,c);
        if(exception)std::rethrow_exception(exception);
        if(outcome!=Outcome::match&&outcome!=Outcome::reject)throw std::logic_error("invalid native SLR callback exit");
        return outcome==Outcome::match;
    }
    Outcome execute_metered(std::string_view subject,std::size_t begin,std::size_t end,Meter& meter,DeterministicProgram::Runtime& runtime)const {
        if(meter.work>meter.max_work||meter.states>meter.max_states)throw std::invalid_argument("invalid native SLR meter");
        if(!ready()||begin>end||end>subject.size())throw std::invalid_argument("invalid native SLR demand");
        runtime={};std::array<State,64> local;std::vector<State> spill;std::exception_ptr exception;
        Context c;c.base=local.data();c.capacity_end=c.base+local.size();c.runtime=&runtime;c.spill=&spill;c.exception=&exception;
        c.work_allowance=meter.max_work-meter.work;c.state_allowance=meter.max_states-meter.states;c.initial_actions=meter.actions;c.cancel=meter.cancel;
        const auto outcome=invoke(subject,begin,end,c);
        meter.work+=runtime.actions;meter.states+=c.allocations;meter.actions+=runtime.actions;
        if(exception)std::rethrow_exception(exception);
        if(outcome==Outcome::internal_error||outcome==Outcome::exception)throw std::logic_error("invalid native SLR metered exit");
        return outcome;
    }
};
} // namespace crank::detail
#undef CRANK_NATIVE_SLR_HOST
