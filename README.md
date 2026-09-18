# SOME-IP Demo

车机以太网 SOME/IP 服务发布示例，同一 Service 定义用**多语言多代实现**：v1 两套零依赖手写栈（Python/C++）可跨栈互操作，v1→v2 为兼容升级（字节级兼容），量产对照 vsomeip 版（云端/容器运行）。

- Service: `0x1234/0x5678`（Major v1.0，Minor 0x00000001）
- Method: `GetVersion(0x0001)`、`Add(0x0002)`
- Field: `Speed(0x1000)` getter/setter/notifier(0x1002)
- Event: `Status(0x8001)`，EventGroup `0x0001`
- UDP：method `30500`，event `30501`，SD 组播 `224.244.224.245:30490`

## 仓库布局

```
SOME-IP-Demo/
├── python/                  # 全部 Python 实现
│   ├── someip/              #   v1 协议栈（最简/学习参考）
│   ├── someip2/             #   v2 升级栈（生产方向，wire/ser 已落地）
│   ├── service_demo.py      #   v1 示例服务
│   └── client_demo.py       #   v1 示例客户端
├── cpp/                     # 全部 C++ 实现（零第三方依赖，POSIX）
│   ├── v1/                  #   v1：someip.hpp/sd.hpp/net.hpp + 两个 demo（C++11）
│   └── v2/                  #   v2：include/someip2/{types,wire,ser}.hpp + 单测（C++17）
├── vsomeip/                 # 量产对照：vsomeip（COVESA）C++ 实现（Linux/容器）
├── tests/                   # 统一测试：协议层单测 + v1 互操作矩阵脚本
├── scripts/run_all.sh       # 一键全面测试（单测 + 构建 + 互操作矩阵）
├── bench/                   # 性能基准（预留）
└── .github/workflows/       # vsomeip 云端构建验证（issue 自动诊断）
```

## 实现代际

| 目录 | 栈 | 语言 | 状态 |
| --- | --- | --- | --- |
| `python/someip/` | v1 | Python（零依赖） | 完成，与 C++ v1 互操作已验证 |
| `cpp/v1/` | v1 | C++11（零依赖） | 完成，与 Python v1 互操作已验证 |
| `python/someip2/` | v2 | Python（零依赖） | 进行中（wire/ser，单测通过） |
| `cpp/v2/` | v2 | C++17（零依赖） | 进行中（wire/ser，单测通过） |
| `vsomeip/` | 量产对照 | C++17 + vsomeip | 工程/CI 齐全，运行验证挂起（见下方状态） |

v1 → v2 兼容红线：`v2 与 v1 线上字节完全兼容`，新增能力（TP/TCP/Nack）走可选开关。详细设计见 `docs/v2-architecture.md`。

## 快速启动（v1）

```bash
# Python：两个终端
cd python && python3 service_demo.py    # 终端1
python3 client_demo.py                  # 终端2

# C++：两个终端
cd cpp/v1 && make && ./bin/service_demo # 终端1
./bin/client_demo                       # 终端2
```

客户端预期输出：`Discover service -> GetVersion v1.0 -> Add(3,4)=7 -> Speed 88 -> 订阅事件/通知`。

## 全面测试

一键跑全部本机可测项：

```bash
./scripts/run_all.sh
```

它依次执行：

| 步骤 | 命令/内容 | 判定 |
| --- | --- | --- |
| Python v2 协议单测 | `python3 -m unittest tests/test_py_wire.py tests/test_py_ser.py` | 16 用例全绿（含 v1↔v2 golden bytes） |
| C++ v2 编译+单测 | `make -C cpp/v2 test` | ALL PASSED |
| C++ v1 构建 | `make -C cpp/v1` | 编译通过 |
| v1 互操作矩阵 | `tests/run_interop_matrix.sh` | 4 组合全绿（两端各自输出断言） |

### v1 互操作矩阵（4 组合）

| 发布方 | 客户端 | 断言点 |
| --- | --- | --- |
| Python service_demo | Python client_demo | Discover / GetVersion / Add=7 / Speed 读 88 / Event+notify |
| C++ service_demo | C++ client_demo | 同上 |
| C++ service_demo | Python client_demo | 同上（跨栈） |
| Python service_demo | C++ client_demo | 同上（跨栈） |

单独跑：`bash tests/run_interop_matrix.sh`。

> 说明：Multicast 依赖本机网卡支持；矩阵脚本使用本机 IP，若公司网络禁组播请换回环网段验证。

## 当前状态

- ✅ v1 Python/C++ 全组合互操作已验证
- ✅ v2 wire/ser 双栈落地 + 单测通过（下一步：SD 完整状态机）
- ⏸ vsomeip 云端 CI 已能完成源码编译与 demo 编译，但 service/client 运行阶段 availability 未触发（client.log 为空）；已自动化 issue 诊断，待 v2 推进后再处理

## 协议要点

- SOME/IP 头部 16 字节：Message ID、Length（=8+payload）、Request ID、Protocol(0x01)、Interface、Message Type、Return Code
- SD 固定 Service ID `0xFFFF` / Method ID `0x8100`，报文 = Entries 数组 + Options 数组，各按 4 字节对齐
- IPv4 Endpoint Option(type=0x04)：`length=9,[type|reserved|ipv4|proto|port]`