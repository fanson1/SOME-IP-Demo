# SOME-IP Demo 平台

`platform/` 下按语言收敛三套实现，**每种语言一套主命名空间**：

```
platform/
├── python/        # 唯一包 `someip`：v2 主栈（app/sdm/tpc/transport/wire/ser +
│                  #   config/log/watchdog）+ someip.legacy 兼容层（v1）
├── cpp/           # 唯一库 `someip`：v2 主栈头文件 + legacy 兼容层 + 双 demo 示例
└── vsomeip/       # 量产对照（COVESA vsomeip，Linux/容器/云端 CI）
```

- **主栈（v2）**是开发主线：wire → ser → transport → tpc → sdm → app，外加横切的
  config/log/watchdog 与背压上限；Python 与 C++ 双实现，字节级一致。
- **legacy 兼容层（v1）**：字节兼容、驱动旧 demo；互操作矩阵保证 v2 与 legacy 的线上
  字节完全一致（golden bytes + 4 组合回归）。
- **对齐目标**是 vsomeip 开源参考实现这一档（差距矩阵见 `docs/vsomeip-gap-analysis.md`，
  性能对拍见 `docs/bench_report.md`）。

## Python

```bash
cd platform/python
pip install -e .            # 可选：按包安装（pyproject.toml 已就绪）
python3 service_demo.py     # 终端1：发布方（v2 app / watchdog）
python3 client_demo.py      # 终端2：订阅方（v2 app / resubscribe）
```

主栈使用：`from someip.app import SomeipServiceV2, ClientV2`。
v2 分层、单测与 C++ 对照见各平台 README 与 `docs/v2-architecture.md`。

## C++

```bash
cd platform/cpp
make        # bin/service_demo、bin/client_demo（v2 示例）
make test   # 8 套主栈单测（wire/ser/transport/tpc/sdm/config/log/watchdog/app）
# 有 cmake 的环境：cmake -S . -B build && cmake --build build && ctest --test-dir build
```

头文件布局：`include/someip/{types,wire,ser,transport,tpc,sdm,app}.hpp` +
`{config,log,watchdog}.hpp`（namespace `someip`）、
`include/someip/legacy/`（兼容层，namespace `someip::legacy`）。

## 一键全量

```bash
bash scripts/run_all.sh
# 1/4 Python v2 单测 → 2/4 C++ 编译+单测 → 3/4 互操作矩阵（4 组合）→ 4/4 RPC 基准
```

## vsomeip（量产对照，需 Linux）

```bash
cd platform/vsomeip
docker build -t someip-demo .            # 或走 .github/workflows/vsomeip-demo.yml 云端验证
```

- GitHub Actions 推送 `platform/vsomeip/**` 自动编译 + 运行 + 断言
  （availability / GetVersion / Add=7 / Speed=88 / 事件 + bench `BENCH PASS`）。
- 当前状态：**CI 全绿**。修复过三处根因——SD 组播锚定 loopback（`gen_config.sh` 动态
  unicast）、JSON 配置插件依赖解析（`LD_LIBRARY_PATH`）、路由 manager 与托管服务端点
  端口同址冲突；`platform/vsomeip/bench.cpp` 已在 CI 产出 RTT p50/p90 + 吞吐基线
  （见 `docs/bench_report.md` 对拍表）。
- 详见 `platform/vsomeip/README.md`。