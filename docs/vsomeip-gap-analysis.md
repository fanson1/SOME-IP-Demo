# vsomeip 对齐差距分析（v2 自研栈 vs 量产参考）

> 定位声明：本项目为**自研 SOME/IP 协议栈（AUTOSAR 字节兼容，开源/学习级）**。
> 对齐目标锁定为 **vsomeip 开源参考实现这一档**，而非 Tier1/供应商"车规认证包"
> （后者还需 ISO 26262 / A-SPICE / 一致性认证等流程体系，见 §4）。

## 1. vsomeip 能力集（事实基线）

vsomeip（COVESA）拆分为 4 个库，模块边界即架构范本：

| 库 | 职责 |
| --- | --- |
| `libvsomeip3` | SOME/IP 协议 + 路由（routing manager）+ 传输/端点 + TP |
| `libvsomeip3-sd` | Service Discovery 状态机 |
| `libvsomeip3-cfg` | JSON 配置系统 |
| `libvsomeip3-e2e` | E2E 保护（CRC/计数器校验） |
| — | **无序列化**（官方明文：由 CommonAPI/用户层承担） |

其他生产特性：多进程路由（routingmanagerd daemon）、TCP/UDP 双端点、
SOME/IP-TP（可配 max-segment-length/separation-time）、SD 指数退避/TTL/
ttl_factor/offer_watchdog、分级日志 + DLT、nPDU（Zugverfahren 帧合并）、
SIGINT/SIGTERM 优雅停止、多宿主/IPv6、发送缓冲与部分发送。

## 2. 功能差距矩阵（v2 现状）

| 能力 | vsomeip | v2 现状 | 差距 |
|---|---|---|---|
| 报文头编解码 | ✅ | ✅ wire（更严格） | 对齐 |
| AUTOSAR wire 序列化 | ❌（CommonAPI 管） | ✅ ser | **超出** |
| UDP/TCP 端点 | ✅ | ✅ transport | P0 |
| SOME/IP-TP 分段/重组 | ✅ | ✅ tpc | P0 |
| SD 完整状态机（Offer/Find/Subscribe Ack·Nack/StopOffer/TTL/退避） | ✅ | ✅ sdm | P0 |
| app：v2 Service/Client API（lib） | ✅ | ✅ app（Python+C++ 双实现） | P0 |

| 高层 demo / 互操作矩阵 | ✅ | ✅ 4 组合全绿（legacy 已退役） | P0 |
| 多进程路由 / daemon | ✅ | ⛔ 边界内不做（单库式） | 差异 |
| JSON 配置系统 | ✅（cfg） | ✅ config（Python + C++ 双实现，vsomeip 子集） | P1 |
| 分级日志 + DLT | ✅ | ✅ log（Python + C++，debug/info/warn/error，env/config） | P1 |
| 优雅关闭 + watchdog + 背压 | ✅ | ✅ carrier/watchdog（看门狗双实现，service 发布循环 + client 事件停流检测并 resubscribe）+ 关闭时 join 线程 + request 在途上限 1024 | P1 |
| 性能基准 | ✅（CH 线程/nPDU/缓冲池） | ✅ bench_rpc（RTT/吞吐/丢包退避/背压）+ bench_report 基线，vsomeip 同场景预留 | P2 |
| E2E 保护 | ✅（e2e） | ❌（未规划细节） | P3 backlog |

## 3. 架构对照

| 维度 | vsomeip | v2 |
|---|---|---|
| 进程模型 | 多进程 + 路由 manager（远端 socket/IPC） | 单进程内嵌（明确边界） |
| 分层 | app → router → endpoint/TP → sd/cfg/e2e | wire → ser → transport → tpc → sdm → app（+config/log 横切） |
| 线程 | io-context 多线程池、发送合并线程 | per-endpoint 事件循环 + 回调（文档 §4 不变量） |
| 配置 | 运行时 JSON 合并 | 计划 JSON（vsomeip schema 子集） |

分层方向一致；v2 以"单库、字节兼容、可测试"为目标裁剪掉多进程路由。

## 4. "量产车标准"真实含义（不含糊）

| 量产要求 | 说明 | 本项目 |
|---|---|---|
| 协议一致性 | AUTOSAR PRS 696 / SOME/IP-TP SWS；extensible data structs | 未做一致性验证（仅自测互操作） |
| 功能安全 | ISO 26262-6 软件层、ASIL 分级、Safety Manual、FMEDA、工具认证 | 无 |
| 开发流程 | A-SPICE 流程证据、追溯、配置管理（import/compliance level） | 无 |
| 安全 | E2E / SecOC / IPsec / 密钥管理 | 边界外（E2E 可作 backlog） |
| 生态集成 | AUTOSAR BSW / vSOMEIP / CommonAPI / PDU Router / DoIP | 无 |
| 资源/部署 | 嵌入式 RTOS、内存约束、确定性调度、看门狗协作 | POSIX/macOS 假设 |

结论：**本项目目标 = 开源参考级对齐 vsomeip**（可在功能/性能/架构上逐项与
vsomeip 对拍评测），**不等于"可装进量产 ECU 的认证软件"**——后者需流程与认证投入。

## 5. 执行路线（P0 → P3）

每阶段完成后更新本文件状态，「对齐↔自证」以互操作矩阵 + 基准数据为准。

### P0 功能主线（当前进行）
- [x] wire / ser 双栈落地（Python + C++）
- [x] transport：UDP 端点抽象（unicast/multicast、select/poll 超时、线程安全 send）+ TCP 流式分帧
- [x] tpc：SOME/IP-TP 分段/重组（会话 key=(service,method,client,session)、超时淘汰）
- [x] sdm：SD 状态机（Offer/Find/Subscribe Ack/Nack、TTL 周期刷新与过期、
      指数退避、initial delay、StopOffer）
- [x] app：v2 Service/Client API + demo 迁移（Python + C++，legacy 退役）
- [x] 互操作矩阵 v2：Python↔C++ 主栈 4 组合全绿
- [x] 文档：`docs/vsomeip-gap-analysis.md`（本文件）

### P1 健壮性
- [x] config：JSON 加载（vsomeip schema 子集：unicast/端口/SD 参数）
  Python `someip/config.py` + C++ 头文件 `someip/config.hpp`（零依赖 RFC 8259 子集
  解析器），demo 支持 `-c/--config`；`config/someip_demo.json` 样例。
- [x] log：分级 logger（debug/info/warn/error，组件前缀，stderr）
  Python `someip/log.py` + C++ `someip/log.hpp`（线程安全）；级别由
  `SOMEIP_LOG_LEVEL` 环境变量或 config `log.level` 指定。
- [x] 优雅关闭（join 线程、资源释放）+ offer watchdog
  Python `someip/watchdog.py` + C++ `someip/watchdog.hpp`（on_expired 回调、pet 抑制、
  线程安全）；service_demo 发布循环看守、client_demo 事件停流检测 + `resubscribe()`
  恢复；`stop()` 均 join 工作线程。
- [x] 发送背压与缓冲上限（防内存耗尽）
  两栈 `MAX_IN_FLIGHT_REQUESTS = 1024`，request 超限抛异常（Python `SomeIpError` /
  C++ `AppError`）；TPC re-assembler 已有 max_sessions 上限。

### P2 性能可测
- [x] bench_rpc：延迟/吞吐/丢包/退避对拍（vsomeip 预留同场景）
  `benchmarks/bench_rpc.py`：RTT p50/p90、并发吞吐、void handler 注入丢包 + 超时
  退避、背压上限命中；阈值判定可挂 CI。
- [x] bench_report 基线入库
  `docs/bench_report.md`：基线与历史表；vsomeip 同场景对拍列为 P2 预留对比项。

### P3 backlog（需专项立项，含流程投入）
- [ ] E2E 保护 profile（CRC/计数器）
- [ ] SOME/IP 一致性测试抽样（对照 PRS 696）
- [ ] （不承诺）ISO 26262 / A-SPICE / SecOC / 生态集成

## 6. 验收标准

- 每层：Python + C++ 双实现、单测引用字节级一致性（golden bytes 与 legacy 互通）。
- P0 完：`scripts/run_all.sh` 全绿，矩阵含 v2 主栈×legacy 全组合。
- P2 完：`bench/bench_report.md` 记录延迟/吞吐基线并与 vsomeip 场景可复现对比。