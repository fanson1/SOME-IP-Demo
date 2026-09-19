# SOME/IP Demo - vsomeip 量产栈版

基于 **vsomeip**（COVESA，AUTOSAR 生态标准 SOME/IP 实现）的服务发布示例，复用与 Python / 手写 C++ 相同的服务定义（Service `0x1234/0x5678` v1.0，EventGroup `0x0001`），演示量产级 SOME/IP 能力：Method、Event、Field、SD Offer/Subscribe。

## 为什么用 vsomeip

- AUTOSAR/COVESA 维护的**标准实现**，量产车机广泛使用
- 内置完整 SD 状态机、TTL 监测、路由管理、TCP/UDP、SOME/IP-TP
- 本 demo 覆盖：`init -> offer_service -> offer_event -> notify`（服务端），
  `request_service -> availability -> subscribe -> request`（客户端）

## 目录

```
platform/vsomeip/
├── service.cpp      # 发布方：GetVersion/Add Method + Speed Field + Status Event
├── client.cpp       # 订阅方：SD 发现 -> RPC -> 订阅事件
├── bench.cpp        # RPC 基准：N=100 顺序往返，RTT p50/p90 + req/s（CI 断言 BENCH PASS）
├── vsomeip.json     # 单机配置模板（unicast 会被 gen_config.sh 按主机重写）
├── gen_config.sh    # 按本机主接口 IP 生成 vsomeip.json（SD 组播依赖真实接口）
├── entrypoint.sh    # 容器入口：先根据运行时网络命名空间再生成配置再 exec
├── CMakeLists.txt
├── Dockerfile       # Ubuntu 24.04：apt 依赖 + 源码编译 vsomeip + 构建 demo
└── ../.github/workflows/vsomeip-demo.yml  # 云端自动构建+断言
```

> 注意：vsomeip 的 SD 组播监听会**锚定到 unicast 对应的网卡**。若 unicast 写成
> `127.0.0.1`，组播加入在 loopback，而 FIND/OFFER 报文从真实网卡发出，内核不会
> 回环到 loopback —— 表现为“availability 一直不触发”。因此配置由
> `gen_config.sh` 按 `hostname -I`（或 `ip`）取主接口 IPv4 动态生成，不再手写 IP。

## 运行方式（三选一，均需 Linux + 网络）

### 1. GitHub Actions（零本地环境）
推送后触发 `vsomeip-demo` workflow，云端 Ubuntu 编译 + 跑 service/client + 断言
（availability / GetVersion / Add=7 / Speed=88 / 事件）。

### 2. Docker
service / client 需**共享网络命名空间**（同机组播 SD + 进程内路由），请都加
`--network host`；配置会在容器启动时由 `entrypoint.sh` 针对宿主接口重新生成。

```bash
docker build -t someip-demo platform/vsomeip
docker run --rm --network host someip-demo                       # 终端1（service）
docker run --rm --network host someip-demo ./build/client        # 终端2（client）
```

### 3. 原生 Ubuntu（vsomeip 需源码编译，Ubuntu 官方未打包 libvsomeip-dev）
```bash
sudo apt install -y build-essential cmake git \
    libboost-dev libboost-system-dev libboost-filesystem-dev libboost-thread-dev
git clone --depth 1 --branch 3.7.6 https://github.com/COVESA/vsomeip /tmp/vsomeip
cmake -S /tmp/vsomeip -B /tmp/vsomeip/build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build /tmp/vsomeip/build -j && sudo cmake --install /tmp/vsomeip/build
cd platform/vsomeip
cmake -S . -B build && cmake --build build
./gen_config.sh vsomeip.json          # 按本机接口 IP 生成配置
export VSOMEIP_CONFIGURATION=$PWD/vsomeip.json
# vsomeip 的 JSON 配置解析是动态加载的插件（libvsomeip-cfg.so），其依赖
# libvsomeip 位于非默认前缀时需显式 LD_LIBRARY_PATH（源码安装到 /usr/local 时
# 可改为 sudo ldconfig，仅需其一）：
export LD_LIBRARY_PATH=/usr/local/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
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
- Ubuntu 官方仓库**未打包** vsomeip（Debian 有），故 Docker/Actions 均改为先 **git clone vsomeip 3.7.6 源码编译安装**，再编译本 demo。
- 云端 CI 此前全红，排查出三处根因并已修复，Actions 现已全绿：
  1. **SD 组播锚定 loopback**：`unicast=127.0.0.1` 使组播加入在 loopback，FIND/OFFER 从真实
     网卡发出后无法回环 → availability 不触发。改由 `gen_config.sh` 动态生成真实接口 IP。
  2. **配置模块加载失败**（`Configuration module could not be loaded!`，服务启动即退出）：
     vsomeip 的 JSON 解析是 dlopen 加载的插件 `libvsomeip-cfg.so`，其依赖 `libvsomeip.so.3`
     位于非默认前缀，需 `LD_LIBRARY_PATH` 显式给出（demo 二进制靠 RPATH 能跑，插件不能）。
  3. **路由 manager 端口与托管服务端点端口同址**：`someip-service` 既作进程内路由 manager
     又托管服务 0x1234@30500，其自身 client 端口改为 30510，client 应用 30511，避免同址冲突。
- 诊断手段：CI 失败时自动把 client.log / service.log 发到 GitHub issue（Actions 内 gh create），
  无需额外权限即可事后定位。

## 与手写实现的关系

| 能力 | 手写 Python/C++ demo | vsomeip 版 |
| --- | --- | --- |
| SD Offer/Find/Subscribe | 最简 | 完整状态机 + TTL |
| Method | REQUEST/RESPONSE | 支持 + 超时/重试 |
| Event / Field notifier | 有 | 有（ET_EVENT） |
| SOME/IP-TP / TCP | 无 | 内置 |
| 序列化字段 | 手写 | 建议配 vsomeip 序列化/代码生成 |