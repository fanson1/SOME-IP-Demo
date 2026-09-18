# SOME/IP Service 发布 Demo

纯 Python 实现的 SOME/IP（Scalable service-Oriented MiddlewarE over IP）协议 demo，含完整协议栈与 Service/Client 示例，零第三方依赖，适合车机以太网 SOME/IP 开发者理解和二次开发。

## 功能特性

- SOME/IP over UDP，16 字节标准头部编解码（Message ID / Length / Request ID / Protocol / Interface / Type / Return Code）
- Service Discovery（SD）
  - 服务端周期广播 `OfferService`
  - 客户端 `FindService` 自动发现服务
  - 客户端 `SubscribeEventgroup` → 服务端 `SubscribeEventgroupAck`
  - IPv4 Endpoint Option（0x04）
- Method（请求/响应）RPC 调用
- Field（字段）：getter / setter / notifier（notification）
- Event（周期事件通知）
- 精细 SD 报文结构（Entries + Options）

## 目录结构

```
platform/python/             # 全部 Python 实现（单一包 someip）
├── someip/                  # 主栈（v2，生产方向）
│   ├── __init__.py          # 导出 Header/Message/Writer/Reader + __version__
│   ├── types.py             # 类型 / 常量 / 异常
│   ├── wire.py              # 报文头编解码（带强制校验、防溢出）
│   └── ser.py               # AUTOSAR wire format 序列化
│   └── legacy/              # v1 兼容层（驱动下面两个 demo，v2 完成后退役）
│       ├── __init__.py      # 导出 SomeIpService / SomeIpClient
│       ├── constants.py     # 消息类型 / 返回码 / SD 常量
│       ├── header.py        # SOME/IP 消息头编解码
│       ├── sd.py            # Service Discovery 报文（Entries/Options）
│       ├── net.py           # UDP / Multicast socket 封装
│       ├── service.py       # SomeIpService（服务端）
│       └── client.py        # SomeIpClient（客户端）
├── pyproject.toml           # 可 pip install -e . 安装
├── service_demo.py          # 示例服务：发布 Method + Field + Event（legacy）
└── client_demo.py           # 示例客户端：发现 + 调用 + 订阅（legacy）
```

v2 分层与进度见 `../../docs/v2-architecture.md`；C++ 对照见 `../cpp`。

## 环境要求

- Python 3.8+（本机已用 3.9 验证）
- 无任何第三方依赖
- 本机网卡支持 IPv4 Multicast（macOS/Linux 均可）

## 快速开始

两个终端分别运行：

```bash
cd platform/python

# 终端 1：启动服务（发布方）
python3 service_demo.py

# 终端 2：启动客户端（订阅方）
python3 client_demo.py
```

## 如何测试

### 方式一：自带 client 联调（推荐）

1. 启动 `service_demo.py`，看到如下输出表示服务已上线：

```
SOME/IP Service started:
  service_id    = 0x1234
  instance_id   = 0x5678
  method        = udp 192.168.x.x:30500
  event         = udp 192.168.x.x:30501
  sd            = 224.244.224.245:30490
  methods       = GetVersion(0x0001), Add(0x0002)
  field         = Speed(0x1000, getter/setter/notifier)
  event         = Status(0x8001)
  eventgroup    = 0x0001
Waiting for clients... (Ctrl+C to quit)
```

2. 启动 `client_demo.py`，预期依次输出：

```
Searching service 0x1234/0x5678 ...
Discovered service at 192.168.x.x:30500
GetVersion -> service=0x1234 instance=0x5678 v1.0
Add(3, 4) -> rc=0x00 result=7
Read Speed  -> rc=0x00 value=NN km/h
Subscribed eventgroup 0x0001, listening...
Write Speed(88) -> rc=0x00
Read Speed  -> rc=0x00 value=88 km/h
  [event 0x8001] status: ts=... speed=NN km/h
  [notify 0x1002] speed field = NN km/h
```

验证点：

| 检查项 | 预期 |
| --- | --- |
| FindService/OfferService | 客户端能打印 Discovered service |
| Method `GetVersion` | 返回 0x1234 / 0x5678 / v1.0 |
| Method `Add` | 3+4=7，rc=0x00 |
| Field getter/setter | 写入 88 后读回 88 |
| 订阅事件 | 每秒收到 Status 与 Speed 通知 |

### 方式二：Wireshark 抓包验证

用 Wireshark 监听本机网卡（macOS 下需填入 root 密码），输入过滤器：

```
udp.port == 30490 || udp.port == 30500 || udp.port == 30501
```

可观察到的报文：

- SD 组播 `224.244.224.245:30490` 上的 `SOME/IP-SD` OfferService / FindService / Subscribe / Ack（Wireshark 内置 SOME/IP 解析器可直接解读）
- `30500` 上的 Method 请求（0x1234/0x0001 REQUEST）与 Response（RESPONSE）
- `30501` 上的 NOTIFICATION（0x1234/0x8001、0x1234/0x1002）

### 方式三：单元级自检（协议层）

```bash
python3 - <<'EOF'
from someip.legacy import SomeIpService, SomeIpClient   # v1 兼容层接口
from someip.constants import SD_MULTICAST_ADDRESS
print("multicast:", SD_MULTICAST_ADDRESS)
print("imports ok")
EOF
```

### 方式四：无网络/虚拟网卡环境

默认取本机主 IP 作为互通地址，若网卡无可用 IPv4 会回退 `127.0.0.1`。仅本机联调时可显式指定 loopback：

```bash
# service_demo.py 里 SomeIpService(interface_ip="127.0.0.1")
# client_demo.py 里 SomeIpClient(interface_ip="127.0.0.1")
```

> 注意：Multicast 依赖网卡支持与 `IP_MULTICAST_LOOP`，若公司网络禁组播，请在受控环境（本机 loopback 或隔离子网）测试。

## 协议要点速览

SOME/IP 头部（16 字节）：

| 字段 | 长度 | 说明 |
| --- | --- | --- |
| Message ID | 4 | Service ID(16) + Method/Event ID(16) |
| Length | 4 | 从 Request ID 到 payload 结束 |
| Request ID | 4 | Client ID(16) + Session ID(16) |
| Protocol Version | 1 | 固定 0x01 |
| Interface Version | 1 | 接口版本 |
| Message Type | 1 | REQUEST=0x00 / NOTIFICATION=0x02 / RESPONSE=0x80 |
| Return Code | 1 | E_OK=0x00 |

SD 固定使用 Service ID `0xFFFF`、Method ID `0x8100`，跑在 UDP 组播 `224.244.224.245:30490`。

MessageType 与返回码定义见 `someip/constants.py`。

## 开发扩展点

- 新增 Method：`service.add_method(method_id, handler)`，handler 签名 `(payload: bytes, addr) -> (return_code, response: bytes)`
- 新增 Event：`service.add_event(event_id, eventgroup_id)` 后调用 `service.publish_event(event_id, payload)`
- 新增 Field：`service.add_uint32_field(field_id, eventgroup_id, initial)`（getter=id, setter=id+1, notifier=id+2）
- 客户端订阅：`client.subscribe(eventgroup_id)`，事件回调 `client.on_event(event_id, callback)`

## 已知限制（学生版 / 演示版）

- 仅 UDP，未实现 TCP 传输与 SOME/IP-TP（大包分片）
- SD 未实现 StopOfferService 主动下线、TTL 超时刷新与 monitoring
- Client 为单请求串行（Session 递增，不并发复用）
- 未处理 Firewall / Security（SecOC）