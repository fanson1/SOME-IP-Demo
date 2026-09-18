# SOME/IP Demo - C++ 实现

纯 C++（C++11，仅系统 socket API，零第三方依赖）的 SOME/IP 服务发布实现，与 `../python`（纯 Python）同协议并存，可跨语言互操作。

## 特性

- SOME/IP over UDP，16 字节头部手工编解码（大端序）
- Service Discovery：OfferService / FindService / SubscribeEventgroup / SubscribeEventgroupAck
- Method（GetVersion / Add）
- Field Speed(0x1000) 读写 + notifier
- Event Status(0x8001) 周期通知
- 多线程：method / sd / offer / publish / event 各自独立线程

## 构建

```bash
cd cpp/v1
make            # 生成 bin/service_demo、bin/client_demo
make clean
```

要求：clang++/g++ 支持 C++11，无需 cmake/boost。

## 运行

```bash
./bin/service_demo     # 终端 1：发布方
./bin/client_demo      # 终端 2：订阅方
```

## 跨语言互操作

同一网络内可以和 Python 实现互通：

| 服务方 | 客户端 | 结果 |
| --- | --- | --- |
| `bin/service_demo` | `python/client_demo.py` | 通过 |
| `python/service_demo.py` | `bin/client_demo` | 通过 |

验证项与 Python 版一致：SD 发现、GetVersion、Add(3+4=7)、Speed 读改 88、事件/通知接收。

## 文件结构

```
cpp/v1/
├── Makefile
├── someip.hpp      # SOME/IP 消息头编解码 + 基础工具
├── sd.hpp          # Service Discovery 报文（entries/options）
├── net.hpp         # UDP/multicast socket 封装
├── service_demo.cpp
└── client_demo.cpp
```

## 已知限制

与 Python 版相同：仅 UDP、无 SOME/IP-TP 分片、无 TTL 超时监测、client 单请求串行。