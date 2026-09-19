# SOME/IP - C++ 平台

零第三方依赖（仅系统 socket API）的 C++ SOME/IP 实现，单一库 `someip`，v2 主栈为开发主线：

- **主栈（v2，namespace `someip`）**：`include/someip/{types,wire,ser}.hpp` 为协议核心，
  `transport.hpp`（UDP 端点）、`tpc.hpp`（SOME/IP-TP）、`sdm.hpp`（SD 状态机）、
  `app.hpp`（Service/Client 高层 API），横切 `config.hpp / log.hpp / watchdog.hpp`。
  防御性解析（越界抛异常），与 Python 侧 `platform/python/someip` 字节级一致。
- **兼容层（v1，namespace `someip::legacy`）**：`include/someip/legacy/{someip,sd,net}.hpp`
  旧版实现，字节互通仍由回归保护。

## 构建

```bash
cd platform/cpp
make            # 生成 bin/service_demo、bin/client_demo（v2 示例）
make test       # 编译并跑 8 套单测（wire/ser/transport/tpc/sdm/config/log/watchdog/app）
make clean
```

要求：clang++/g++ 支持 C++17（Makefile 用 `-std=c++17 -Iinclude`）。无 cmake 也可用。

有 cmake 的环境（CI 等）：

```bash
cmake -S platform/cpp -B platform/cpp/build -DCMAKE_BUILD_TYPE=Release
cmake --build platform/cpp/build
ctest --test-dir platform/cpp/build
```

## 运行（v2 示例）

两个终端（支持 `-c/--config` 指定 JSON 配置，样例 `config/someip_demo.json`）：

```bash
./bin/service_demo     # 终端 1：发布方（Method/Field/Event + watchdog + 背压）
./bin/client_demo      # 终端 2：订阅方（发现 + RPC + 订阅 + 停流 resubscribe）
```

同一网络内可与 Python 端互通：

| 服务方 | 客户端 | 结果 |
| --- | --- | --- |
| `bin/service_demo` | `platform/python/client_demo.py` | 通过 |
| `platform/python/service_demo.py` | `bin/client_demo` | 通过 |

自动化的全部 4 组合回归见 `tests/run_interop_matrix.sh`（`scripts/run_all.sh` 一键执行，
随后跑 `benchmarks/bench_rpc.py` RPC 基准）。

## 文件结构

```
platform/cpp/
├── Makefile
├── CMakeLists.txt          # 标准构建（供有 cmake 的环境/CI）
├── include/someip/
│   ├── types.hpp           # 主栈：常量/异常
│   ├── wire.hpp            # 主栈：报文头 + 帧
│   ├── ser.hpp             # 主栈：AUTOSAR wire format
│   ├── transport.hpp       # 主栈：UDP/TCP 端点（unicast/multicast、线程安全发送）
│   ├── tpc.hpp             # 主栈：SOME/IP-TP 分段/重组
│   ├── sdm.hpp             # 主栈：SD 状态机（TTL/退避/StopOffer）
│   ├── app.hpp             # 主栈：v2 Service/Client 高层 API（含 resubscribe）
│   ├── config.hpp / log.hpp / watchdog.hpp   # 横切（JSON/分级日志/看门狗）
│   └── legacy/
│       ├── someip.hpp      # v1：消息头编解码 + 工具
│       ├── sd.hpp          # v1：SD entries/options
│       └── net.hpp         # v1：UDP/multicast socket
├── tests/                  # test_{wire_ser,transport,tpc,sdm,config,log,watchdog,app}.cpp
└── examples/
    ├── service_demo.cpp    # v2 demo：Method + Field + Event
    └── client_demo.cpp     # v2 demo：发现 + RPC + 订阅
```

## 已知限制（对比 vsomeip）

与 Python 版一致：单进程内嵌式（无多进程 routing manager daemon）；TCP 端点已实现并在单测
（loopback）覆盖，互操作矩阵与 demo 网络路径走 UDP；TP 会话/背压有界；未实现 E2E 保护 /
SecOC（P3 backlog，见 `docs/vsomeip-gap-analysis.md`）。