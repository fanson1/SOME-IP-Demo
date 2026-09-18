# SOME-IP Demo

车机以太网 SOME/IP 服务发布示例，同一 Service 定义用两种技术栈实现，可跨栈互操作。

- Service: `0x1234/0x5678`（Major v1.0，Minor 0x00000001）
- Method: `GetVersion(0x0001)`、`Add(0x0002)`
- Field: `Speed(0x1000)` getter/setter/notifier(0x1002)
- Event: `Status(0x8001)`，EventGroup `0x0001`
- UDP：method `30500`，event `30501`，SD 组播 `224.244.224.245:30490`

## 两个实现

| 目录 | 技术栈 | 特点 |
| --- | --- | --- |
| `someip_demo/` | 纯 Python（零依赖） | 面向快速原型、学习协议 |
| `someip_demo_cpp/` | C++11（仅系统 socket，零依赖） | 更贴近量产方向，可直接嵌入 |

各自的构建/运行/测试说明见子目录 README（`someip_demo/README.md`、`someip_demo_cpp/README.md`）。

## 跨栈互操作（已验证）

| 发布方 | 客户端 | 结果 |
| --- | --- | --- |
| `someip_demo/service_demo.py` | `someip_demo/client_demo.py` | 通过 |
| `someip_demo_cpp/bin/service_demo` | `someip_demo_cpp/bin/client_demo` | 通过 |
| `someip_demo_cpp/bin/service_demo` | `someip_demo/client_demo.py` | 通过 |
| `someip_demo/service_demo.py` | `someip_demo_cpp/bin/client_demo` | 通过 |

四个组合依赖相同的字节级协议定义，验证了两套栈对 SOME/IP 头、SD entries/options 的编码互认。

## 快速启动

Python：`cd someip_demo && python3 service_demo.py`，另开 `python3 client_demo.py`

C++：`cd someip_demo_cpp && make && ./bin/service_demo`，另开 `./bin/client_demo`

## 协议要点

- SOME/IP 头部 16 字节：Message ID、Length（=8+payload）、Request ID、Protocol(0x01)、Interface、Message Type、Return Code
- SD 固定 Service ID `0xFFFF` / Method ID `0x8100`，报文 = Entries 数组 + Options 数组，各按 4 字节对齐
- IPv4 Endpoint Option(type=0x04)：`length=9,[type|reserved|ipv4|proto|port]`