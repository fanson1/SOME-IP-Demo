// Petting watchdog timer (P1 robustness).
//
// Runs a worker thread that fires `on_expired` once per lapse of `timeout`
// seconds between `pet()` calls. `stop()` signals and joins the worker for a
// graceful shutdown. Used to detect stall/death of owned loops (offer
// publishing, event stream) and drive recovery.
#ifndef SOMEIP_WATCHDOG_HPP
#define SOMEIP_WATCHDOG_HPP

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace someip {
namespace watchdog {

class Watchdog {
public:
    using Clock = std::chrono::steady_clock;

    // `ond` is invoked from the worker thread, once per lapse.
    // `interval_s` is the polling granularity; a small value keeps firing
    // latency low at the cost of a wakeup.
    explicit Watchdog(double timeout_s,
                      std::function<void()> ond = nullptr,
                      double interval_s = 0.1)
        : timeout_ns_(dur(timeout_s)), interval_ns_(dur(interval_s)),
          on_expired_(std::move(ond)) {
        if (timeout_s <= 0) {
            throw std::invalid_argument("watchdog timeout must be > 0");
        }
        if (interval_s <= 0) {
            throw std::invalid_argument("watchdog interval must be > 0");
        }
    }
    Watchdog(const Watchdog &) = delete;
    Watchdog &operator=(const Watchdog &) = delete;

    ~Watchdog() { stop(); }

    void start() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (thread_) {
                return;
            }
            last_ = Clock::now();
            running_.store(true);
            thread_ = std::make_unique<std::thread>(&Watchdog::run, this);
        }
    }

    void pet() {
        std::lock_guard<std::mutex> lk(mu_);
        if (!running_.load()) {
            return;
        }
        last_ = Clock::now();
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (!running_.exchange(false)) {
                return;
            }
            cv_.notify_all();
        }
        std::thread *t = nullptr;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (thread_) {
                t = thread_.get();
            }
        }
        if (t && t->joinable()) {
            t->join();
        }
        std::lock_guard<std::mutex> lk(mu_);
        thread_.reset();
    }

    bool running() const { return running_.load(); }

private:
    static std::chrono::nanoseconds dur(double s) {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::duration<double>(s));
    }

    void run() {
        while (running_.load()) {
            std::function<void()> fire;
            {
                std::lock_guard<std::mutex> lk(mu_);
                const auto now = Clock::now();
                if (now - last_ >= timeout_ns_) {
                    fire = on_expired_;
                    last_ = now;
                }
            }
            if (fire) {
                try {
                    fire();
                } catch (...) {
                    // the watchdog must never take the owner down
                }
            }
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait_for(lk, interval_ns_, [&] { return !running_.load(); });
            lk.unlock();
        }
    }

    std::chrono::nanoseconds timeout_ns_;
    std::chrono::nanoseconds interval_ns_;
    std::function<void()> on_expired_;
    std::atomic<bool> running_{false};
    std::mutex mu_;
    std::condition_variable cv_;
    Clock::time_point last_{};
    std::unique_ptr<std::thread> thread_;
};

}  // namespace watchdog
}  // namespace someip

#endif  // SOMEIP_WATCHDOG_HPP