// C++ graded logger tests (P1 logging).
#include "someip/log.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

static int failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            ++failures;                                                   \
        }                                                                 \
    } while (0)

namespace {

std::string capture_stderr(const std::function<void()> &fn) {
    char path[] = "/tmp/someip_log_test_XXXXXX";
    const int fd = mkstemp(path);
    const int saved = dup(STDERR_FILENO);
    dup2(fd, STDERR_FILENO);
    fn();
    std::fflush(stderr);
    dup2(saved, STDERR_FILENO);
    close(saved);
    close(fd);
    std::FILE *f = std::fopen(path, "rb");
    std::string out;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        out.append(buf, n);
    }
    std::fclose(f);
    unlink(path);
    return out;
}

}  // namespace

static void emit(someip::log::Logger &lg) {
    lg.debug("dbg-line");
    lg.info("info-line");
    lg.warn("warn-line");
    lg.error("err-line");
}

int main() {
    someip::log::Logger info_lg("t", someip::log::Level::Info);
    const std::string out = capture_stderr([&] { emit(info_lg); });
    CHECK(out.find("info-line") != std::string::npos);
    CHECK(out.find("warn-line") != std::string::npos);
    CHECK(out.find("err-line") != std::string::npos);
    CHECK(out.find("dbg-line") == std::string::npos);
    CHECK(out.find("[info]") != std::string::npos);
    CHECK(out.find("t]") != std::string::npos);

    someip::log::Logger err_lg("t2", someip::log::Level::Error);
    const std::string out2 = capture_stderr([&] { emit(err_lg); });
    CHECK(out2.find("err-line") != std::string::npos);
    CHECK(out2.find("warn-line") == std::string::npos);
    CHECK(out2.find("info-line") == std::string::npos);

    someip::log::Logger dbg_lg("t3", someip::log::Level::Debug);
    const std::string out3 = capture_stderr([&] { emit(dbg_lg); });
    CHECK(out3.find("dbg-line") != std::string::npos);

    CHECK(someip::log::level_from_name("debug") == someip::log::Level::Debug);
    CHECK(someip::log::level_from_name("WARN") == someip::log::Level::Info);
    CHECK(someip::log::level_from_name("error") == someip::log::Level::Error);

    if (failures == 0) {
        std::printf("C++ v2 log: ALL PASSED\n");
        return 0;
    }
    std::printf("C++ v2 log: %d failure(s)\n", failures);
    return 1;
}