# 性能基准基线（bench_report）

`benchmarks/bench_rpc.py` 对 v2 栈（Python）做本机对拍：反复执行、观察波动、把较稳数据
作为基线。vsomeip 侧同场景对拍已完成，结果见「与 vsomeip 对拍」。（`docs/vsomeip-gap-analysis.md`）

## 方法

- 环境：macOS 12.7.6 (Intel)，CPython 3.9.2，本机 loopback + 组播 SD（同 demos）。
- 服务与客户端同进程，走完整 SD 发现 + UDP RPC，非短路直调。
- 延迟：200 次顺序往返（前 1 次 warmup 不计），`time.perf_counter`。
- 吞吐：600 请求 / 16 worker 线程并发，墙钟求 req/s。
- 丢包/退避：服务端对前 5 次调用不回复（void handler），客户端 `timeout=0.3s`
  超时后重试，度量单次未回复成本与恢复后首呼延迟。
- 背压：并发投递 3× 上限（测试时把上限降到 64），统计被 `SomeIpError` 拒绝数。

## 基线（2026-09-19）

| 指标 | 基线 | 说明 |
| --- | --- | --- |
| RTT 均值 | 0.44 ms (p50 0.23 / p90 0.43) | 含 SD 发现后的纯 RPC 往返 |
| 吞吐 | ~2000 req/s | 16 worker 并发 |
| 单次未回复成本 | ≈ timeout 参数（0.3s） | 无自动重试，超时即释放 |
| 恢复后首呼 | <1 ms | 丢包预算耗尽后立即成功 |
| 背压上限命中 | 61/192（cap=64 时） | 抛错而非无限排队 |

运行：

```bash
python3 -u benchmarks/bench_rpc.py            # 全绿且达标 = PASS
python3 -u benchmarks/bench_rpc.py --min-inflight-once 500
```

阈值：吞吐 ≥ 500 req/s、丢包可恢复、背压可触发即为通过。历史值：

| 日期 | avg | p50 | p90 | req/s | 备注 |
| --- | --- | --- | --- | --- | --- |
| 2026-09-19 | 0.44 | 0.23 | 0.43 | 2060 | 基线入库 |

## 与 vsomeip 对拍（完成，2026-09-19）

同场景：`Add(3,4)` 顺序往返、完整 SD 发现；vsomeip 侧走进程内路由 manager（in-process
routing，本地 UDS 承载）。vsomeip 基准由 `platform/vsomeip/bench.cpp` 在**云端 CI
（ubuntu-latest VM，C++）**产出并持续断言 `BENCH PASS`；v2 基准在本机
（macOS 12.7.6 Intel，CPython 3.9.2）由 `benchmarks/bench_rpc.py` 产出。

| 指标 | v2（本机，Python） | vsomeip 3.7.6（CI VM，C++） |
| --- | --- | --- |
| RTT 平均值 | 0.44 ms | 1.08 ms |
| p50 | 0.23 ms | 1.08 ms |
| p90 | 0.43 ms | 1.09 ms |
| 吞吐 | ~2060 req/s（16 worker 并发） | 923 req/s（单在途顺序推算 1000/avg） |

口径与可比性说明：

- **环境不同**：v2 在本机 macOS 上经真实 UDP 端点（Python 解释执行）；vsomeip 在 CI 虚拟机上
  （C++，进程内 RM + UDS）。两者不是同一台/同一栈，绝对值仅作工程参考。
- **吞吐口径不同**：v2 为并发吞吐（16 worker），vsomeip 为顺序单在途推算（1000/rtt_avg），
  不宜直接相除比较；延迟基准（p50/p90）方法一致（顺序往返）可比。
- **可复现**：vsomeip 每次 CI 都会跑 bench 并断言 `BENCH PASS`；本机可用
  `platform/vsomeip/bench.cpp` 复跑（`./build/bench`，N=100，单次 0.3s 超时）。
- vsomeip 首版 bench 用 `get_request()` 预存 session 做关联而全部超时：vsomeip 的
  client/session 在 `send()` 时才由路由层分配，读取时机错误导致响应无法回关联（issue #6）。
  已改为「单在途 + 收到即置位」的 flag 方案（与 bench_rpc.py 相同方法论），100/100 通过。

复现命令：

```bash
# v2
python3 -u benchmarks/bench_rpc.py
# vsomeip（需 Linux + vsomeip 构建产物）
./gen_config.sh vsomeip.json && export VSOMEIP_CONFIGURATION=$PWD/vsomeip.json && ./build/bench
```