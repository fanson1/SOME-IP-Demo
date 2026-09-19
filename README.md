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
│   │   │                 #   主栈 v2 = app/sdm/tpc/transport/wire/ser/types
│   │   │                 #   横切 = config(JSON)/log(分级)/watchdog
│   │   ├── pyproject.toml #   可 pip install -e .
│   │   └── service_demo.py / client_demo.py
│   ├── cpp/               # 唯一库 `someip`（C++17 头文件）：
│   │   │                 #   主栈 v2 = include/someip/{app,sdm,tpc,transport,wire,ser}.hpp
│   │   │                 #   横切 = include/someip/{config,log,watchdog}.hpp
│   │   ├── Makefile
│   │   ├── tests/         # test_{wire_ser,transport,tpc,sdm,config,log,watchdog,app}.cpp
│   │   └── examples/service_demo.cpp / client_demo.cpp
│   └── vsomeip/           # 量产对照（COVESA vsomeip，Linux/容器/云端 CI）
├── tests/                 # 协议层单测 + v2/v1 互操作矩阵脚本
├── benchmarks/bench_rpc.py# RPC 对拍基准（延迟/吞吐/丢包退避/背压）
├── config/someip_demo.json# JSON 配置样例（-c/--config）
├── scripts/run_all.sh     # 一键全面测试
├── docs/                  # 架构说明 / 差距矩阵 / 基准报告
└── .github/workflows/     # vsomeip 云端构建+断言（issue 自动诊断）
```

`platform/README.md` 是三种实现的入口说明；「v1/v2 不再以顶层目录/包分裂的版本」见下方代际与文档。

## 实现代际

| 目录 | 代 | 语言 | 命名空间 | 状态 |
| --- | --- | --- | --- | --- |
| `platform/python/someip` | v2 主栈 | Python（零依赖） | `someip.*` | 完成：app/sdm/tpc/transport/wire/ser/types + config/log/watchdog，全量单测+互操作矩阵绿 |
| `platform/cpp/include/someip` | v2 主栈 | C++17（零依赖） | namespace `someip` | 完成：与 Python v2 同构，单测+矩阵绿 |
| `platform/vsomeip` | 量产对照 | C++17 + vsomeip | — | 工程/CI 齐全，云端 CI 全绿（见状态） |

兼容红线：`v2 与 v1/legacy 线上字节完全兼容`，新增能力（TP/TCP/Nack）走可选开关。详见 `docs/v2-architecture.md`。

## 快速启动（v2 demo）

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
from someip.app import SomeipServiceV2, ClientV2   # pip install -e platform/python
from someip import config, log                     # JSON 配置 + 分级日志
```

```cpp
#include <someip/app.hpp>     // -I platform/cpp/include, link Threads
#include <someip/config.hpp>  // AppConfig + 零依赖 JSON 解析
#include <someip/log.hpp>
#include <someip/watchdog.hpp>
```

JSON 配置：`config/someip_demo.json`，demo 用 `-c/--config` 指定；日志级别由
`SOMEIP_LOG_LEVEL` 或 config `log.level` 控制；服务/客户端 `stop()` 均 join 工作
线程，发布循环与事件推送带 watchdog 与在途请求背压上限。

## 全面测试

一键跑全部本机可测项：

```bash
./scripts/run_all.sh
```

依次执行：

| 步骤 | 内容 | 判定 |
| --- | --- | --- |
| Python v2 协议单测 | `python3 -m unittest discover -s tests -p "test_py_*.py"` | 83 用例全绿（含 config/log/watchdog/背压） |
| C++ v2 编译+单测 | `make -C platform/cpp test` | 8 个测试组 ALL PASSED |
| 互操作矩阵 | `tests/run_interop_matrix.sh` | 4 组合全绿（两端各自断言） |
| RPC 基准 | `python3 -u benchmarks/bench_rpc.py` | PASS（见 `docs/bench_report.md`） |

### 互操作矩阵（4 组合）

| 发布方 | 客户端 | 断言点 |
| --- | --- | --- |
| Python service_demo | Python client_demo | Discover / GetVersion / Add=7 / Speed 读 88 / Event+notify |
| C++ service_demo | C++ client_demo | 同上 |
| C++ service_demo | Python client_demo | 同上（跨栈） |
| Python service_demo | C++ client_demo | 同上（跨栈） |

单独跑：`bash tests/run_interop_matrix.sh`（依赖本机 IPv4 multicast，公司网络禁组播时请用受控网段）。

## 当前状态

- ✅ v2 主栈 Python + C++ 全模块落地（app/sdm/tpc/transport/wire/ser + config/log/watchdog/背压），
  全套单测 + 4 组合互操作矩阵 + RPC 基准全绿（`docs/bench_report.md` 基线）
- ✅ 优雅关闭（stop 并 join 线程）、发布/事件 watchdog、在途请求背压上限 1024
- ✅ 差距矩阵 `docs/vsomeip-gap-analysis.md`：P1 全部达成，P2 bench 入库（含 vsomeip 对拍），P3 为专项 backlog
- ✅ vsomeip 云端 CI（`platform/vsomeip`）恢复全绿：修复了三处根因——(1) SD 组播监听锚定
  loopback（unicast 改动态真实接口 IP，`gen_config.sh`）；(2) JSON 配置插件
  `libvsomeip-cfg.so` 的依赖在非默认前缀下无法 dlopen（`LD_LIBRARY_PATH`）；(3) 路由
  manager 应用端口与所托管服务端点同址冲突（分离为 30510/30511 + 服务 30500）。
  Actions 全程断言 availability/GetVersion/Add/Speed/事件 + bench `BENCH PASS`
- ✅ vsomeip 同场景基准对拍完成（`platform/vsomeip/bench.cpp`，RTT p50 1.08ms / p90 1.09ms，
  对比表见 `docs/bench_report.md`）
- 已知差异：本机 macOS 上 UDP/TCP 均单测覆盖（loopback），互操作矩阵与 demo 网络路径走 UDP；
  多进程 daemon 模式在边界外

## 协议要点

- SOME/IP 头部 16 字节：Message ID、Length（=8+payload）、Request ID、Protocol(0x01)、Interface、Message Type、Return Code
- SD 固定 Service ID `0xFFFF` / Method ID `0x8100`，报文 = Entries 数组 + Options 数组，各按 4 字节对齐
- IPv4 Endpoint Option(type=0x04)：`length=9,[type|reserved|ipv4|proto|port]`