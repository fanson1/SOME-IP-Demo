# SOME-IP Demo 平台

`platform/` 下按语言收敛三套实现，**每种语言一套主命名空间、无 v1/v2 目录割裂**：

```
platform/
├── python/        # 唯一包 `someip`：v2 主栈（wire/ser/types）+ someip.legacy 兼容层（v1，驱动 demo）
├── cpp/           # 唯一库 `someip`：include/someip/{types,wire,ser}.hpp 主栈 + legacy 兼容层 + 双 demo 示例
└── vsomeip/       # 量产对照（COVESA vsomeip，Linux/容器/云端）
```

- **主栈（v2）**是开发主线：wire（报文头/帧校验）、ser（AUTOSAR wire format）、后续 transport/tpc/sdm/app 按 `docs/v2-architecture.md` 补齐。
- **legacy 兼容层（v1）**字节兼容、驱动可运行 demo，v2 高层完成前保留、完成后退役，绝不让"同一协议两套顶层包并存"。
- 兼容红线：v2 与 legacy 线上字节完全一致，golden bytes + 互操作矩阵持续回归。

## Python

```bash
cd platform/python
pip install -e .            # 可选：按包安装（pyproject.toml 已就绪）
python3 service_demo.py     # 终端1：发布方（driven by someip.legacy）
python3 client_demo.py      # 终端2：订阅方
```

主栈使用：`from someip import Header, Message, Writer, Reader`。
v2 分层与单测见 `tests/`；C++ 对照见 `platform/cpp`。

## C++

```bash
cd platform/cpp
make        # bin/service_demo、bin/client_demo（legacy 兼容层示例）
make test   # 主栈 wire/ser 单测
# 有 cmake 的环境：cmake -S . -B build && cmake --build build && ctest --test-dir build
```

头文件布局：`include/someip/{types,wire,ser}.hpp`（主栈，namespace `someip`）、
`include/someip/legacy/{someip,sd,net}.hpp`（兼容层，namespace `someip::legacy`）。

## vsomeip（量产对照，需 Linux）

```bash
cd platform/vsomeip
docker build -t someip-demo .            # 或走 .github/workflows/vsomeip-demo.yml 云端验证
```

- GitHub Actions 推送 `platform/vsomeip/**` 自动编译+运行+断言（availability/GetVersion/Add=7/Speed=88/事件）。
- 详见 `platform/vsomeip/README.md`。
- 当前状态：CI 可完成源码编译与 demo 编译，运行阶段 availability 未触发（日志为空）已挂起，转 issue 自动诊断，待 v2 推进后再处理。