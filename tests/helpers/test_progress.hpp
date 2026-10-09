#pragma once

// Optional diagnostics for long differential tests. No timeout or work budget
// is changed. Enable with CRANK_TEST_PROGRESS=1; diagnostics go to stderr.
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace crank_test {
class Progress {
    using Clock = std::chrono::steady_clock;
    struct Record {
        unsigned long long id = 0;
        std::string phase;
        std::string detail;
        Clock::time_point began{};
        bool risky = false;
        int exceptions = 0;
    };
    bool enabled_ = false;
    std::string test_;
    Clock::time_point began_{};
    std::mutex mutex_;
    std::condition_variable wake_;
    bool stopping_ = false;
    std::thread watcher_;
    std::vector<Record> active_;
    Record last_;
    double last_seconds_ = 0;
    unsigned long long started_ = 0, completed_ = 0, completed_cases_ = 0, exceptions_ = 0;

    static std::string quote(std::string_view value) {
        std::ostringstream out;
        out << '"';
        for (unsigned char c : value) {
            if (c == '"' || c == '\\') out << '\\' << char(c);
            else if (c < 32) out << "\\u" << std::hex << std::setw(4)
                                  << std::setfill('0') << unsigned(c) << std::dec;
            else out << char(c);
        }
        out << '"';
        return out.str();
    }
    static double seconds(Clock::time_point start) {
        return std::chrono::duration<double>(Clock::now() - start).count();
    }
    void emit(const char* event, const Record* item = nullptr) noexcept {
        try {
        // Called with mutex_ held. One compact periodic line avoids recording
        // millions of successful calls; long/risky calls also record boundaries.
        std::ostringstream out;
        out << "CRANK_PROGRESS {\"event\":" << quote(event)
            << ",\"test\":" << quote(test_) << std::fixed << std::setprecision(6)
            << ",\"wall_seconds\":" << seconds(began_)
            << ",\"started\":" << started_ << ",\"completed\":" << completed_
            << ",\"completed_cases\":" << completed_cases_
            << ",\"exceptions\":" << exceptions_;
        const Record* current = item ? item : (active_.empty() ? nullptr : &active_.back());
        if (current) {
            out << ",\"current_id\":" << current->id
                << ",\"current_phase\":" << quote(current->phase)
                << ",\"current_detail\":" << quote(current->detail)
                << ",\"current_seconds\":" << seconds(current->began);
        }
        if (last_.id) {
            out << ",\"last_completed_id\":" << last_.id
                << ",\"last_completed_phase\":" << quote(last_.phase)
                << ",\"last_completed_detail\":" << quote(last_.detail)
                << ",\"last_completed_seconds\":" << last_seconds_;
        }
        out << "}\n";
        std::cerr << out.str() << std::flush;
        } catch (...) { /* Diagnostics must not terminate the test. */ }
    }
    unsigned long long start(std::string_view phase, std::string_view detail, bool risky) {
        if (!enabled_) return 0;
        std::lock_guard<std::mutex> lock(mutex_);
        Record record{++started_, std::string(phase), std::string(detail),
                      Clock::now(), risky, std::uncaught_exceptions()};
        active_.push_back(std::move(record));
        if (risky) emit("START");
        return started_;
    }
    void finish(unsigned long long id) noexcept {
        if (!id) return;
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            if (active_.empty() || active_.back().id != id) return;
            last_ = std::move(active_.back());
            active_.pop_back();
            last_seconds_ = seconds(last_.began);
            ++completed_;
            if (last_.phase == "case") ++completed_cases_;
            if (std::uncaught_exceptions() > last_.exceptions) ++exceptions_;
            if (last_.risky || last_seconds_ >= 0.25) emit("END", &last_);
        } catch (...) { /* Diagnostic destruction must not affect test results. */ }
    }
public:
    class Guard {
        Progress* owner_ = nullptr;
        unsigned long long id_ = 0;
    public:
        Guard(Progress* owner, unsigned long long id) : owner_(owner), id_(id) {}
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
        Guard(Guard&& other) noexcept : owner_(other.owner_), id_(other.id_) { other.id_ = 0; }
        ~Guard() { finish(); }
        void finish() noexcept { if (id_) { owner_->finish(id_); id_ = 0; } }
    };
    explicit Progress(std::string_view test) {
        const char* value = std::getenv("CRANK_TEST_PROGRESS");
        enabled_ = value && std::string_view(value) == "1";
        if (!enabled_) return;
        test_ = std::string(test);
        began_ = Clock::now();
        emit("READY");
        watcher_ = std::thread([this] {
            std::unique_lock<std::mutex> lock(mutex_);
            while (!wake_.wait_for(lock, std::chrono::seconds(1), [this] { return stopping_; }))
                emit("STATE");
        });
    }
    ~Progress() {
        if (!enabled_) return;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
            emit("FINAL");
        }
        wake_.notify_one();
        watcher_.join();
    }
    bool enabled() const noexcept { return enabled_; }
    Guard scope(std::string_view phase, std::string_view detail, bool risky = false) {
        return Guard(this, start(phase, detail, risky));
    }
    template<class F>
    decltype(auto) call(std::string_view phase, std::string_view detail, F&& fn, bool risky = false) {
        auto guard = scope(phase, detail, risky);
        return std::forward<F>(fn)();
    }
};
} // namespace crank_test
