# 协议范围

## ADB wireless transport 与 MTPADB RPC 分层

Android wireless entry 使用所选 AOSP 分支的官方 `adb pair` pairing server、TLS ADB connection server、ADB packet framing 和标准 service dispatch。配对端口与 TLS connection 端口分离且由 AOSP 动态分配；`mtpadbctl wireless status` 显示两个端点。启用时，`mtpadbd` 注册 `_adb-tls-pairing._tcp` 和 `_adb-tls-connect._tcp`；关闭时不创建这些 listener 或项目 mDNS record。

stock host `adb` 完成配对与 TLS 连接后，`mtpadb` 通过本机 ADB server 选择 serial 并请求 `OPEN("mtpadb:rpc")`。ADB TLS 保护整个 ADB transport；`mtpadbd` 仅把该 service 的字节流交给私有 `/dev/socket/mtprpcd-adb` Unix socket。MTPADB 的 PSK `AUTH` 和 AEAD 是 TLS 之上的第二层认证/加密，不替代 ADB 配对，也不新增网络 listener。

`mtprpcd` 当前支持 MTPX `PING` 和 `echo` stream。`echo` 以 `OPEN` payload `echo` 建立流，再以 `TX`/`RX` 回显二进制数据；这不是任意 MTP opcode、MTP 容器或 USB-MTP transaction bridge。物理 USB-MTP 路径保持不变。

## MTPX v1 frame transport

MTPX 是独立于 ADB packet framing 的 32-byte frame header 加 payload。所有多字节整数均为 network byte order（big-endian）；payload 紧随 header，长度由 `payload_length` 指定。

| Offset | Bytes | 字段 | 约束 |
|---:|---:|---|---|
| 0 | 4 | magic | ASCII `MTPX` |
| 4 | 2 | version | 当前仅接受 `1` |
| 6 | 2 | type | 下表定义的 frame type |
| 8 | 4 | flags | 当前必须为零 |
| 12 | 4 | stream_id | 0 表示控制帧；数据流使用非零 ID |
| 16 | 8 | sequence | 单向 session frame 序号 |
| 24 | 4 | payload_length | payload 字节数，最大 1 MiB |
| 28 | 4 | reserved | 必须为零 |

| Type ID | 名称 | 当前用途 |
|---:|---|---|
| 1 | `HELLO` | 双方公开 nonce 与设备 ID |
| 2 | `AUTH` | host PSK 认证 |
| 3 | `OPEN` | 打开当前支持的 `echo` stream |
| 4 | `CLOSE` | 关闭 stream |
| 5 | `TX` | host 向设备提交 stream 数据 |
| 6 | `RX` | 设备回传 stream 数据 |
| 7 | `STATUS` | 成功状态或 `PONG` |
| 8 | `PING` | session 控制 ping |
| 9 | `ERROR` | 通用失败 |

共享 C++17 codec 支持 frame 编码，以及对分段 header/payload 和多个合并 frame 的增量解码。解码器在检查 magic、version、type、reserved 与 payload 上限后才会按声明长度预留 payload 存储。

## MTPX PSK handshake 与加密

- Host `HELLO` payload 为 `protocol_version_be16 || host_nonce[32]`。
- Device `HELLO` payload 为 `device_id[16] || device_nonce[32]`。
- Host `AUTH` 是 `HMAC-SHA256(PSK, protocol_version_be16 || host_nonce || device_nonce || device_id)`。
- 会话密钥由 `HKDF-Extract(salt=host_nonce || device_nonce, IKM=PSK)` 和 `HKDF-Expand(PRK, info="MTPADB-RPC-v1" || device_id, L=64)` 派生；前 32 bytes 用于 host-to-device，后 32 bytes 用于 device-to-host。
- 已认证 payload 使用 ChaCha20-Poly1305。96-bit nonce 为方向前缀 `0`/`1`（32-bit big-endian）加单调 64-bit frame sequence；序列化 frame header 是 AEAD associated data。线上加密 payload 后附 16-byte tag。

Handshake 只接受 `HELLO -> AUTH -> encrypted frames`。认证错误使用通用 `ERROR`，不会记录 PSK、pairing code 或 session key。stock ADB TLS 认证与此 MTPADB PSK 是两个独立凭据层。

## 已实现操作与资源上限

- `PING`：stream 0、空 payload，回复 encrypted `STATUS("PONG")`。
- `OPEN("echo")`：stream 0，成功回复分配的非零 stream ID 和成功状态。
- `TX`/`RX`：使用已打开的 stream ID，二进制 chunk 最大 256 KiB；空输入也可以正常关闭。
- `CLOSE`：关闭指定 stream，响应状态。

共享接口公开上限：单 frame payload `kMaxPayloadBytes = 1 MiB`（加密后其中包含 AEAD tag）；每 session 最多 `kMaxStreams = 16` 条 stream；每 session 排队数据上限 `kMaxQueueBytes = 4 MiB`；默认数据分块 `kDefaultDataChunkBytes = 256 KiB`。daemon 对 handshake 设置 deadline，对已认证输出使用有界队列；不支持的帧/操作会关闭 session 并返回通用错误。

`mtpadb` 使用 stock local ADB server（默认 `tcp:5037`）选择 paired serial 并打开 service；它不直接连接无线 pairing/TLS 端口，也不自行实现 ADB TLS 或 ADB packet framing。
