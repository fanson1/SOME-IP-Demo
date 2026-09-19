# 性能基准基线（bench_report）

`benchmarks/bench_rpc.py` 对 v2 栈（Python）做本机对拍：反复执行、观察波动、把较稳数据
作为基线。vsomeip 侧预留同场景对拍（P2 预留项，见 `docs/vsomeip-gap-analysis.md`）。

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

## 与 vsomeip 对拍（预留）

- 同场景：`platform/vsomeip`（容器/云端）跑同样的顺序往返/并发吞吐/丢包恢复，
  结果写入本报告的对比表。
- 环境差异注明：vsomeip 跑 Linux 容器与 UDP/TCP endpoint，本机 v2 仅 UDP。