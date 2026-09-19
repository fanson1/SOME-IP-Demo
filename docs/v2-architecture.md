# SOME/IP 自研栈 v2 —— 架构设计

> 目标：把已验证的 legacy（v1，`platform/python/someip/legacy/` Python +
> `platform/cpp/include/someip/legacy/` C++11）升级为**面向量产方向的自研
> SOME/IP 主栈**，双实现字节级互操作，覆盖 vsomeip 生产特性的核心子集，
> 并以互操作矩阵 + 性能基准持续回归。

## 1. 特性范围（与 vsomeip 能力映射）

| 能力 | v1 现状 | v2 目标 | 参考 vsomeip |
| --- | --- | --- | --- |
| 报文头/RPC 编解码 | 有 | **加固**：length 一致性校验、协议/接口版本检查、上限防御 | `protocol/tcp/udp` |
| SD Offer/Find/Subscribe(Ack/Nack) | 最简(Offer/Find/Subscribe/Ack) | **完整状态机**：TTL 周期刷新与过期、指数退避、SubscribeNack、sawtooth 防错 | `service_discovery` |
| AUTOSAR 序列化 | 手写 struct | **ser 编解码**：u8/u16/u32/u64、string、byte array、array、struct、union（含 length/错误处理） | `vsomeip_plugin` / 代码生成 |
| SOME/IP-TP | 无 | 分段/重组（≤1392B payload），TP 头校验 | `SOMEIP_TP` |
| 传输 | UDP(单播/组播) | UDP + **TCP 可靠通道**，端点抽象 | `endpoints` |
| 高层 API | Service/Client 线程模型 | 保留对外 API 兼容 + 事件回调强化 | `application` |
| 日志/配置 | 打印 | logger 分级 + JSON 配置加载 | `configuration` |
| 测试 | 手工 4 组合 | 单测 + 互操作矩阵脚本 + 基准 | `test` |

**明确不做（写入边界）**：SecOC/IPsec 集成、进程外多应用路由、ARXML 代码生成、
DBus/管理接口。这些依赖环境或需整车集成，自研纯栈仅提供数据面与状态机。

## 2. 分层结构（两套实现同构）

```
┌────────────────────────────────────────────┐
│ app        高层 API: SomeipServiceV2/ClientV2 │
├────────────────────────────────────────────┤
│ sdm        SD 状态机（offer/find/sub/nack、TTL 定时器、退避、sawtooth）
├────────────────────────────────────────────┤
│ tpc        SOME/IP-TP 分段/重组
├────────────────────────────────────────────┤
│ transport  UDP(multicast/unicast) + TCP 端点
├────────────────────────────────────────────┤
│ ser        AUTOSAR wire format 序列化
├────────────────────────────────────────────┤
│ wire       报文头编解码 + 校验（最底层）
├────────────────────────────────────────────┤
│ config/log 配置加载 + 分级日志（横切）
└────────────────────────────────────────────┘
```

### 目录约定

```
platform/python/someip/        # Python 主栈（唯一包 someip，零第三方依赖）
  __init__  types.py  wire.py  ser.py  transport.py  tpc.py  sdm.py  app.py  config.py  log.py  xutil.py
platform/python/someip/legacy/ # v1 兼容层（驱动 demo，v2 完成后退役）
platform/cpp/include/someip/   # C++ 主栈（唯一库 someip，C++17，零第三方依赖，POSIX）
  types.hpp  wire.hpp  ser.hpp  transport.hpp  tpc.hpp  sdm.hpp  app.hpp
platform/cpp/include/someip/legacy/  # v1 兼容层（C++11，namespace someip::legacy）
platform/cpp/{tests,examples}  Makefile  CMakeLists.txt
tests/   互操作与单测脚本（tests/run_interop_matrix.sh，python3 -m unittest discover -s tests -p 'test_py_*.py'）
scripts/ 一键全面测试（scripts/run_all.sh）
bench/   性能基准（benchmarks/bench_rpc.py + bench_report，含 vsomeip 对拍表）
platform/vsomeip/              # 量产对照栈（COVESA vsomeip，Linux/容器/云端 CI）
```

统一布局：按语言收敛到 `platform/`，语言内**单一主命名空间**（`someip`），
版本演化用兼容层 + 语义化版本（`someip.__version__`），不以目录/包名分裂 v1/v2。

## 3. 兼容红线（字节级互操作保证）

1. v2 与 legacy（v1）**线上字节完全兼容**：RequestId(client_id<<16|session)、length、SD entries/options 布局不变。
2. v2↔v2、v2↔legacy、Python↔C++ 全组合互操作矩阵必须全绿才能合入。
3. 新增能力（TP/TCP/Nack）走**可选开关**：默认路径与 legacy 报文一致。

## 4. 关键不变量与防御

- `length == 8 + payload_size`，非法立即丢弃并告警（不 panic）。
- 不带 TP 的 UDP payload 上限 1392B（SOME/IP — UDP/IPv4 MTU 安全值）；超过必须走 TP。
- TP 会话 32 位 key=(service,method,client,session) 去重，重组超时淘汰，防止资源耗尽。
- SD：TTL 到期即从表移除并触发 availability(false)；offer/find 周期可配，重试带指数退避上限。
- 单线程 per-socket 事件循环 + 线程安全回调（lock/wait 原语）。

## 5. 开发顺序（依赖自上而下）

1. wire+ser（本阶段，双栈 + 字节互通单测）
2. transport（UDP/TCP endpoint）
3. tpc（TP 分段重组）
4. sdm（SD 状态机）→ app 组装 → demo 迁移
5. 互操作矩阵 + 基准 → vsomeip 对照（CI 排障后置）