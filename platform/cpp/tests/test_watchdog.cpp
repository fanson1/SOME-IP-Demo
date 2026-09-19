// C++ petting watchdog tests (P1 robustness).
#include "someip/watchdog.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

static int failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            ++failures;                                                   \
        }                                                                 \
    } while (0)

#define CHECK_THROWS(expr)                                                \
    do {                                                                  \
        bool _threw = false;                                              \
        try {                                                             \
            (void)(expr);                                                 \
        } catch (const std::invalid_argument &) {                         \
            _threw = true;                                                \
        }                                                                 \
        if (!_threw) {                                                    \
            std::printf("FAIL %s:%d: expected throw\n", __FILE__,         \
                        __LINE__);                                        \
            ++failures;                                                   \
        }                                                                 \
    } while (0)

int main() {
    // fires when starved
    {
        std::atomic<int> fired{0};
        someip::watchdog::Watchdog wd(
            0.1, [&] { fired.fetch_add(1); }, 0.01);
        wd.start();
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        wd.stop();
        CHECK(fired.load() >= 1);
        CHECK(fired.load() <= 3);  // once per lapse, not a tight burst
    }
    // not fired when petted
    {
        std::atomic<int> fired{0};
        someip::watchdog::Watchdog wd(
            0.2, [&] { fired.fetch_add(1); }, 0.01);
        wd.start();
        for (int i = 0; i < 50; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
            wd.pet();
        }
        wd.stop();
        CHECK(fired.load() == 0);
    }
    // stop() before start() is safe
    {
        someip::watchdog::Watchdog wd(1.0);
        wd.stop();
        wd.stop();
    }
    // invalid timeout
    CHECK_THROWS(someip::watchdog::Watchdog(0.0));
    CHECK_THROWS(someip::watchdog::Watchdog(-1.0, nullptr, 0.1));

    if (failures == 0) {
        std::printf("C++ v2 watchdog: ALL PASSED\n");
        return 0;
    }
    std::printf("C++ v2 watchdog: %d failure(s)\n", failures);
    return 1;
}