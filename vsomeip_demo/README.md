# SOME/IP Demo - vsomeip 量产栈版

基于 **vsomeip**（COVESA，AUTOSAR 生态标准 SOME/IP 实现）的服务发布示例，复用与 Python / 手写 C++ 相同的服务定义（Service `0x1234/0x5678` v1.0，EventGroup `0x0001`），演示量产级 SOME/IP 能力：Method、Event、Field、SD Offer/Subscribe。

## 为什么用 vsomeip

- AUTOSAR/COVESA 维护的**标准实现**，量产车机广泛使用
- 内置完整 SD 状态机、TTL 监测、路由管理、TCP/UDP、SOME/IP-TP
- 本 demo 覆盖：`init -> offer_service -> offer_event -> notify`（服务端），
  `request_service -> availability -> subscribe -> request`（客户端）

## 目录

```
vsomeip_demo/
├── service.cpp      # 发布方：GetVersion/Add Method + Speed Field + Status Event
├── client.cpp       # 订阅方：SD 发现 -> RPC -> 订阅事件
├── vsomeip.json     # 单机配置（unicast 127.0.0.1 + SD multicast）
├── CMakeLists.txt
├── Dockerfile       # Ubuntu 24.04 + libvsomeip-dev 一键构建
└── ../.github/workflows/vsomeip-demo.yml  # 云端自动构建+断言
```

## 运行方式（三选一，均需 Linux + 网络）

### 1. GitHub Actions（零本地环境）
推送后触发 `vsomeip-demo` workflow，云端 Ubuntu 编译 + 跑 service/client + 断言
（availability / GetVersion / Add=7 / Speed=88 / 事件）。

### 2. Docker
```bash
docker build -t someip-demo vsomeip_demo
docker run --rm someip-demo ./build/service &     # 终端1
docker run --rm --network host -e VSOMEIP_CONFIGURATION=/app/vsomeip.json someip-demo ./build/client  # 终端2
```

### 3. 原生 Ubuntu
```bash
sudo apt install -y build-essential cmake libvsomeip-dev
cd vsomeip_demo
cmake -S . -B build && cmake --build build
export VSOMEIP_CONFIGURATION=$PWD/vsomeip.json
./build/service &       # 终端1
./build/client          # 终端2
```

## 预期输出（client）

```
Service 0x1234/0x5678 is available
  [resp 0x0001] GetVersion  rc=0x00 value=16777216
  [resp 0x0002] Add         rc=0x00 value=7
  [resp 0x1000] ReadSpeed   rc=0x00 value=N
  ...
  [resp 0x1001] WriteSpeed  rc=0x00 value=88
  [event 0x8001] status: ts=... speed=...
  [notify 0x1002] speed field = ...
```

## 本机（macOS）现状说明

- 本仓库开发机为 Intel macOS 12 + Homebrew，**官方仅支持 vsomeip 在 Linux/Android/Windows 构建**；
  Homebrew 在本机无 bottle（走源码构建需升级 CLT），且本机访问 GitHub 大文件约 10KB/s，
  因此采用 **云端/容器** 作为执行环境。
- 代码路径已按 Ubuntu 24.04 `libvsomeip-dev` 对齐（库名 `vsomeip3`/`vsomeip3-sd`）。

## 与手写实现的关系

| 能力 | 手写 Python/C++ demo | vsomeip 版 |
| --- | --- | --- |
| SD Offer/Find/Subscribe | 最简 | 完整状态机 + TTL |
| Method | REQUEST/RESPONSE | 支持 + 超时/重试 |
| Event / Field notifier | 有 | 有（ET_EVENT） |
| SOME/IP-TP / TCP | 无 | 内置 |
| 序列化字段 | 手写 | 建议配 vsomeip 序列化/代码生成 |