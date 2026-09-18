# SOME/IP - C++ 平台

零第三方依赖（仅系统 socket API）的 C++ SOME/IP 实现，单一库 `someip`，两代共存分层：

- **主栈（v2，namespace `someip`）**：`include/someip/{types,wire,ser}.hpp`
  多层（后续 transport/tpc/sdm/app 按 `docs/v2-architecture.md` 补齐），防御性解析（越界抛 `MalformedMessage`）。
- **兼容层（v1，namespace `someip::legacy`）**：`include/someip/legacy/{someip,sd,net}.hpp`
  即原 v1 栈（SOME/IP 头 + SD + UDP/multicast socket），驱动 `examples/` 双 demo，与 Python 侧 `platform/python/someip/legacy` 字节互通。

## 构建

```bash
cd platform/cpp
make            # 生成 bin/service_demo、bin/client_demo（legacy 示例）
make test       # 主栈 wire/ser 单测（C++17）
make clean
```

要求：clang++/g++ 支持 C++17（Makefile 用 `-std=c++17 -Iinclude`）。无 cmake 也可用。

有 cmake 的环境（CI 等）：

```bash
cmake -S platform/cpp -B platform/cpp/build -DCMAKE_BUILD_TYPE=Release
cmake --build platform/cpp/build
ctest --test-dir platform/cpp/build
```

## 运行（legacy 兼容层 demo）

两个终端：

```bash
./bin/service_demo     # 终端 1：发布方
./bin/client_demo      # 终端 2：订阅方
```

同一网络内可与 Python 端互通：

| 服务方 | 客户端 | 结果 |
| --- | --- | --- |
| `bin/service_demo` | `platform/python/client_demo.py` | 通过 |
| `platform/python/service_demo.py` | `bin/client_demo` | 通过 |

自动化的全部 4 组合回归见 `tests/run_interop_matrix.sh`（`scripts/run_all.sh` 一键执行）。

## 文件结构

```
platform/cpp/
├── Makefile
├── CMakeLists.txt          # 标准构建（供有 cmake 的环境/CI）
├── include/someip/
│   ├── types.hpp           # 主栈：常量/异常
│   ├── wire.hpp            # 主栈：报文头 + 帧
│   ├── ser.hpp             # 主栈：AUTOSAR wire format
│   └── legacy/
│       ├── someip.hpp      # v1：消息头编解码 + 工具
│       ├── sd.hpp          # v1：SD entries/options
│       └── net.hpp         # v1：UDP/multicast socket
├── tests/test_wire_ser.cpp # 主栈单测（含 v1 字节互通）
└── examples/
    ├── service_demo.cpp    # legacy demo：Method + Field + Event
    └── client_demo.cpp     # legacy demo：发现 + RPC + 订阅
```

## 已知限制

与 Python 版一致：仅 UDP、无 SOME/IP-TP 分片、无 TTL 超时监测（主栈 v2 计划补齐）；client 单请求串行。