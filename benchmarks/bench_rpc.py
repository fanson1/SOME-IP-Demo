#!/usr/bin/env python3
# SOME/IP v2 RPC benchmark (P2): latency / throughput / loss / backoff.
#
# Runs the stack in-process (real SD multicast discovery like the demos, but
# service and client share the host). Emits `key=value` lines and, on success,
# a short report. Exit code 0 = thresholds held, 1 = a threshold was exceeded.
#
#   python3 -u benchmarks/bench_rpc.py
import argparse
import concurrent.futures
import statistics
import struct
import sys
import time

sys.path.insert(0, "platform/python")

from someip import app
from someip.app import ClientV2, MAX_IN_FLIGHT_REQUESTS, SomeipServiceV2

SERVICE_ID = 0x1234
INSTANCE_ID = 0x5678
METHOD_ECHO = 0x0050
METHOD_DROP = 0x0051
N = 200


def u32(p):
    return struct.unpack(">I", p)[0]


def bench_latency(service, client):
    times = []
    for _ in range(N - 1):
        t0 = time.perf_counter()
        rc, payload = client.request(SERVICE_ID, INSTANCE_ID, METHOD_ECHO,
                                     struct.pack(">I", 7), timeout=3.0)
        times.append((time.perf_counter() - t0) * 1000)
        assert rc == 0 and u32(payload) == 7
    times.sort()
    avg = statistics.fmean(times)
    p50 = times[len(times) // 2]
    p90 = times[int(len(times) * 0.9)]
    return avg, p50, p90


def bench_throughput(service, client):
    def one(_):
        rc, payload = client.request(SERVICE_ID, INSTANCE_ID, METHOD_ECHO,
                                     struct.pack(">I", 1), timeout=2.0)
        return rc == 0 and u32(payload) == 1

    total = 600
    t0 = time.perf_counter()
    with concurrent.futures.ThreadPoolExecutor(max_workers=16) as ex:
        results = list(ex.map(one, range(total)))
    dt = time.perf_counter() - t0
    ok = sum(results)
    return total / dt, ok, total


def bench_loss_and_backoff(service, client):
    # Service drops the first `k` responses for METHOD_DROP; the client must
    # time out and retry. Measures per-drop recovery cost (backoff behavior).
    k = 5
    service._drop_remaining = k
    times = []
    for i in range(k):
        t0 = time.perf_counter()
        try:
            _ = client.request(SERVICE_ID, INSTANCE_ID, METHOD_DROP,
                               struct.pack(">I", i), timeout=0.3)
        except TimeoutError:
            pass
        times.append(time.perf_counter() - t0)
    avg = statistics.fmean(times)
    # now the drop budget is exhausted -> the next call must succeed fast
    t0 = time.perf_counter()
    rc, payload = client.request(SERVICE_ID, INSTANCE_ID, METHOD_DROP,
                                 struct.pack(">I", 99), timeout=2.0)
    recovered_ms = (time.perf_counter() - t0) * 1000
    return avg * 1000, recovered_ms, rc == 0


def bench_backpressure_bound(service, client):
    # Fire far more concurrent requests than the in-flight cap; expect the cap
    # to trip (SomeIpError) instead of unbounded memory growth. The real cap is
    # 1024; here we lower it to make the bound observable without 1024 threads.
    from someip.types import SomeIpError

    tested_cap = 64
    old = MAX_IN_FLIGHT_REQUESTS
    app.MAX_IN_FLIGHT_REQUESTS = tested_cap
    errors = 0

    def one(_):
        nonlocal errors
        try:
            client.request(SERVICE_ID, INSTANCE_ID, METHOD_ECHO,
                           struct.pack(">I", 0), timeout=3.0)
        except (SomeIpError, TimeoutError):
            errors += 1

    total = tested_cap * 3
    try:
        with concurrent.futures.ThreadPoolExecutor(max_workers=160) as ex:
            list(ex.map(one, range(total)))
    finally:
        app.MAX_IN_FLIGHT_REQUESTS = old
    return errors, total, tested_cap


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--min-inflight-once", type=float, default=500.0,
                    help="minimum throughput in req/s to hold")
    args = ap.parse_args()

    service = SomeipServiceV2(SERVICE_ID, INSTANCE_ID, 0x01, 1,
                              method_port=0, event_port=0, sd_port=30500)
    service._drop_remaining = 0

    def on_echo(payload, _src):
        return (0, payload)

    def on_drop(payload, _src):
        if service._drop_remaining > 0:
            service._drop_remaining -= 1
            return None  # drop the response on purpose
        return (0, struct.pack(">I", 0xDEAD))

    service.add_method(METHOD_ECHO, on_echo)
    service.add_method(METHOD_DROP, on_drop)
    service.start()

    client = ClientV2(client_id=0x0BEE, sd_port=30500).start()
    try:
        if not client.wait_for_service(SERVICE_ID, INSTANCE_ID, (), 5.0):
            print("FAIL discovery timeout")
            return 2

        avg, p50, p90 = bench_latency(service, client)
        rps, ok, total = bench_throughput(service, client)
        miss_ms, recover_ms, recovered = bench_loss_and_backoff(service, client)
        errs, total_bp, tested_cap = bench_backpressure_bound(service, client)

        print("latency_ms_avg=%0.3f" % avg)
        print("latency_ms_p50=%0.3f" % p50)
        print("latency_ms_p90=%0.3f" % p90)
        print("throughput_req_per_s=%0.1f ok=%d/%d" % (rps, ok, total))
        print("drop_first_unreplied_avg_ms=%0.3f recovered=%s"
              % (miss_ms, recovered))
        print("drop_recovery_ms=%0.3f" % recover_ms)
        print("backpressure_cap_tested=%d errors=%d fired=%d"
              % (tested_cap, errs, total_bp))

        ok_flag = (rps >= args.min_inflight_once) and recovered and errs > 0
        print("RESULT: %s" % ("PASS" if ok_flag else "FAIL"))
        return 0 if ok_flag else 1
    finally:
        client.stop()
        service.stop()


if __name__ == "__main__":
    sys.exit(main())