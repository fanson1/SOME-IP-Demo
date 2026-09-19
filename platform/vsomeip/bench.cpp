// vsomeip RPC latency/throughput benchmark (P2 parity with benchmarks/bench_rpc.py).
//
// Measures sequential round-trip latency (avg/p50/p90) for the Add method and
// derives throughput; prints a stable machine-readable block:
//   rtt_avg_ms=... rtt_p50_ms=... rtt_p90_ms=... req_per_s=... completed=N/N
//   BENCH PASS
// The exact same scenario family (RTT + derived req/s over one host, in-process
// routing manager) is what the hand-written Python stack benchmarks.
#include <vsomeip/vsomeip.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <thread>
#include <vector>

static const vsomeip::service_t  SERVICE_ID = 0x1234;
static const vsomeip::instance_t INSTANCE_ID = 0x5678;
static const vsomeip::method_t   METHOD_ADD = 0x0002;
static const size_t N = 100;

static std::shared_ptr<vsomeip::application> g_app;
static std::atomic<bool> g_started{false};
static std::atomic<size_t> g_got{0};

struct Pending {
    std::chrono::steady_clock::time_point start;
    bool done = false;
    std::mutex m;
    std::condition_variable cv;
};

static std::mutex g_pending_mx;
static std::map<uint64_t, std::shared_ptr<Pending>> g_pending;
static std::vector<double> g_rtt_ms;

static void on_response(const std::shared_ptr<vsomeip::message> &response) {
    if (response->get_method() != METHOD_ADD) {
        return;
    }
    std::shared_ptr<Pending> p;
    {
        std::lock_guard<std::mutex> lk(g_pending_mx);
        auto it = g_pending.find(response->get_request());
        if (it == g_pending.end()) {
            return;
        }
        p = it->second;
        g_pending.erase(it);
    }
    {
        std::lock_guard<std::mutex> lk(p->m);
        p->done = true;
    }
    p->cv.notify_all();
    const double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - p->start).count();
    {
        std::lock_guard<std::mutex> lk(g_pending_mx);
        g_rtt_ms.push_back(ms);
    }
    g_got.fetch_add(1);
}

static void run_bench() {
    size_t completed = 0;
    for (size_t i = 0; i < N; ++i) {
        auto request = vsomeip::runtime::get()->create_request();
        request->set_service(SERVICE_ID);
        request->set_instance(INSTANCE_ID);
        request->set_method(METHOD_ADD);
        std::vector<vsomeip::byte_t> bytes{0, 0, 0, 3, 0, 0, 0, 4};  // 3 + 4
        auto payload = vsomeip::runtime::get()->create_payload();
        payload->set_data(bytes);
        request->set_payload(payload);

        auto p = std::make_shared<Pending>();
        p->start = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lk(g_pending_mx);
            g_pending[request->get_request()] = p;
        }
        g_app->send(request);
        std::unique_lock<std::mutex> lk(p->m);
        if (p->cv.wait_for(lk, std::chrono::seconds(2), [&] { return p->done; })) {
            ++completed;
        } else {
            std::lock_guard<std::mutex> pmx(g_pending_mx);
            g_pending.erase(request->get_request());
        }
    }
    while (g_got.load() != completed) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

static void print_stats() {
    std::vector<double> v;
    {
        std::lock_guard<std::mutex> lk(g_pending_mx);
        v = g_rtt_ms;
    }
    if (v.size() != N) {
        std::printf("completed=%zu/%zu BENCH FAIL (rtt samples=%zu)\n",
                    v.size(), N, v.size());
        g_app->stop();
        return;
    }
    std::sort(v.begin(), v.end());
    const double sum = std::accumulate(v.begin(), v.end(), 0.0);
    const double avg = sum / v.size();
    const double p50 = v[v.size() / 2];
    const double p90 = v[size_t(v.size() * 0.9)];
    std::printf("rtt_avg_ms=%.3f rtt_p50_ms=%.3f rtt_p90_ms=%.3f "
                "req_per_s=%.1f\n",
                avg, p50, p90, 1000.0 / avg);
    std::printf("completed=%zu/%zu\n", v.size(), N);
    std::printf("BENCH PASS\n");
    std::fflush(stdout);
    g_app->stop();
}

static void on_availability(vsomeip::service_t service, vsomeip::instance_t instance,
                            bool available) {
    if (available && !g_started.exchange(true)) {
        std::printf("Service 0x%04X/0x%04X is available (bench)\n", service,
                    instance);
        std::thread([] {
            const auto t0 = std::chrono::steady_clock::now();
            run_bench();
            const double total_s =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
                    .count();
            std::printf("wall_s=%.3f\n", total_s);
            print_stats();
        }).detach();
    }
}

static void on_state(vsomeip::state_type_e state) {
    if (state == vsomeip::state_type_e::ST_REGISTERED) {
        g_app->request_service(SERVICE_ID, INSTANCE_ID);
    }
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    g_app = vsomeip::runtime::get()->create_application("someip-bench");
    if (!g_app->init()) {
        fprintf(stderr, "vsomeip init failed\n");
        return 1;
    }
    g_app->register_state_handler(on_state);
    g_app->register_availability_handler(SERVICE_ID, INSTANCE_ID, on_availability);
    g_app->register_message_handler(vsomeip::ANY_SERVICE, INSTANCE_ID,
                                    vsomeip::ANY_METHOD, on_response);
    g_app->start();
    return 0;
}