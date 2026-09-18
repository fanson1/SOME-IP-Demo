# SOME-IP Demo

车机以太网 SOME/IP 服务发布示例，同一 Service 定义用**多语言多代实现**：自研 v1/v2 字节兼容栈（Python + C++ 双实现，可跨栈互操作），v2 为主开发主线、v1 为 legacy 兼容层；另有量产对照 vsomeip 版（云端/容器运行）。

- Service: `0x1234/0x5678`（Major v1.0，Minor 0x00000001）
- Method: `GetVersion(0x0001)`、`Add(0x0002)`
- Field: `Speed(0x1000)` getter/setter/notifier(0x1002)
- Event: `Status(0x8001)`，EventGroup `0x0001`
- UDP：method `30500`，event `30501`，SD 组播 `224.244.224.245:30490`

## 仓库布局

```
SOME-IP-Demo/
├── platform/
│   ├── python/            # 唯一包 `someip`：
│   │   │                 #   主栈 v2 = types/wire/ser.py
│   │   │                 #   兼容层 = someip/legacy/（v1，驱动两个 demo）
│   │   ├── pyproject.toml #   可 pip install -e .
│   │   └── service_demo.py / client_demo.py
│   ├── cpp/               # 唯一库 `someip`：
│   │   │                 #   主栈 v2 = include/someip/{types,wire,ser}.hpp
│   │   │                 #   兼容层 legacy = include/someip/legacy/（v1 demo 示例）
│   │   ├── Makefile / CMakeLists.txt
│   │   ├── tests/test_wire_ser.cpp
│   │   └── examples/service_demo.cpp / client_demo.cpp
│   └── vsomeip/           # 量产对照（COVESA vsomeip，Linux/容器/云端 CI）
├── tests/                 # 协议层单测 + v1 互操作矩阵脚本
├── scripts/run_all.sh     # 一键全面测试
├── bench/                 # 性能基准（预留）
├── docs/v2-architecture.md
└── .github/workflows/     # vsomeip 云端构建+断言（issue 自动诊断）
```

`platform/README.md` 是三种实现的入口说明；「v1/v2 不再以顶层目录/包分裂的版本」见下方代际与文档。

## 实现代际

| 目录 | 代 | 语言 | 命名空间 | 状态 |
| --- | --- | --- | --- | --- |
| `platform/python/someip` | v2 主栈 | Python（零依赖） | `someip.wire/.ser/.types` | 进行中（wire/ser，单测通过） |
| `platform/python/someip/legacy` | v1 兼容层 | Python（零依赖） | `someip.legacy.*` | 完成，驱动 demo，与 C++ legacy 互操作已验证 |
| `platform/cpp/include/someip` | v2 主栈 | C++17（零依赖） | namespace `someip` | 进行中（wire/ser，单测通过） |
| `platform/cpp/include/someip/legacy` | v1 兼容层 | C++11（零依赖） | namespace `someip::legacy` | 完成，与 Python legacy 互操作已验证 |
| `platform/vsomeip` | 量产对照 | C++17 + vsomeip | — | 工程/CI 齐全，运行验证挂起（见状态） |

兼容红线：`v2 与 legacy 线上字节完全兼容`，新增能力（TP/TCP/Nack）走可选开关。详见 `docs/v2-architecture.md`。

## 快速启动（legacy demo）

```bash
# Python：两个终端
cd platform/python && python3 service_demo.py    # 终端1
python3 client_demo.py                          # 终端2

# C++：两个终端
cd platform/cpp && make && ./bin/service_demo   # 终端1
./bin/client_demo                               # 终端2
```

客户端预期输出：`Discover service -> GetVersion v1.0 -> Add(3,4)=7 -> Speed 88 -> 订阅事件/通知`。
Python/C++ 可在同一网络中互相跨栈调用（见测试矩阵）。

## 主栈（v2）使用

```python
from someip import Header, Message, Writer, Reader   # pip install -e platform/python
```

```cpp
#include <someip/wire.hpp>   // -I platform/cpp/include, link Threads
```

## 全面测试

一键跑全部本机可测项：

```bash
./scripts/run_all.sh
```

依次执行：

| 步骤 | 内容 | 判定 |
| --- | --- | --- |
| Python v2 协议单测 | `python3 -m unittest discover -s tests -p "test_py_*.py"` | 16 用例全绿（含 v2↔legacy golden bytes） |
| C++ v2 编译+单测 | `make -C platform/cpp test` | ALL PASSED |
| C++ legacy 构建 | `make -C platform/cpp all` | 编译通过 |
| 互操作矩阵 | `tests/run_interop_matrix.sh` | 4 组合全绿（两端各自断言） |

### 互操作矩阵（4 组合）

| 发布方 | 客户端 | 断言点 |
| --- | --- | --- |
| Python service_demo | Python client_demo | Discover / GetVersion / Add=7 / Speed 读 88 / Event+notify |
| C++ service_demo | C++ client_demo | 同上 |
| C++ service_demo | Python client_demo | 同上（跨栈） |
| Python service_demo | C++ client_demo | 同上（跨栈） |

单独跑：`bash tests/run_interop_matrix.sh`（依赖本机 IPv4 multicast，公司网络禁组播时请用受控网段）。

## 当前状态

- ✅ legacy（v1）Python/C++ 全组合互操作已验证，回归脚本一键可跑
- ✅ v2 wire/ser 双栈落地 + 单测通过（下一步：SD 完整状态机）
- ⏸ vsomeip 云端 CI 能完成源码编译与 demo 编译，运行阶段 availability 未触发（client.log 为空）；已自动化 issue 诊断，待 v2 推进后再处理

## 协议要点

- SOME/IP 头部 16 字节：Message ID、Length（=8+payload）、Request ID、Protocol(0x01)、Interface、Message Type、Return Code
- SD 固定 Service ID `0xFFFF` / Method ID `0x8100`，报文 = Entries 数组 + Options 数组，各按 4 字节对齐
- IPv4 Endpoint Option(type=0x04)：`length=9,[type|reserved|ipv4|proto|port]`