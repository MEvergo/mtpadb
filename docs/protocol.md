# 协议范围

## 当前 Stage A

USB gadget 使用 ADB interface class/subclass/protocol `0xff/0x42/0x01`，通过 FunctionFS 提供 bulk OUT 与 bulk IN endpoint。该层仅建立 USB 枚举面；当前 daemon 不读写 ADB transport 消息，不发送 `CNXN`，也不实现认证、shell、sync 或 pairing。

因此 `adb devices -l` 中出现设备只证明 USB 设备发现，不代表 transport 可用。设备不能被描述为完整 ADB endpoint。

## 后续 MTP-RPC 边界

ADB transport framing/auth、MTPX frame transport 与 MTP 命令/RPC 语义必须分层；本次只实现 MTPX v1 frame codec，不实现 ADB transport，也不定义 RPC 操作。MTP payload 仍须使用 AOSP MTP 容器格式，不能把 MTP transaction 当作任意字节流；当前没有加入 MTP-RPC opcode 或 payload 语义。

### 已实现：MTPX v1 frame transport

MTPX 是独立于 ADB packet framing 的 32-byte frame header 加 payload。所有多字节整数均为 network byte order（big-endian）；payload 紧随 header，长度由 `payload_length` 指定。

| Offset | Bytes | 字段 | 约束 |
|---:|---:|---|---|
| 0 | 4 | magic | ASCII `MTPX` |
| 4 | 2 | version | 当前仅接受 `1` |
| 6 | 2 | type | 下表定义的 frame type |
| 8 | 4 | flags | 由上层解释 |
| 12 | 4 | stream_id | stream 标识 |
| 16 | 8 | sequence | frame 序号 |
| 24 | 4 | payload_length | payload 字节数，最大 1 MiB |
| 28 | 4 | reserved | 必须为零 |

| Type ID | 名称 |
|---:|---|
| 1 | `HELLO` |
| 2 | `AUTH` |
| 3 | `OPEN` |
| 4 | `CLOSE` |
| 5 | `TX` |
| 6 | `RX` |
| 7 | `STATUS` |
| 8 | `PING` |
| 9 | `ERROR` |

共享 C++17 codec 支持完整 frame 编码，以及对分段 header/payload 和多个合并 frame 的增量解码。解码器在检查 magic、version、type、reserved 与 payload 上限后才会按声明长度预留 payload 存储。

共享接口公开以下上限：单 frame payload `kMaxPayloadBytes = 1 MiB`；每 session 最多 `kMaxStreams = 16` 条 stream；每 session 排队数据上限 `kMaxQueueBytes = 4 MiB`；默认数据分块 `kDefaultDataChunkBytes = 256 KiB`。stream 数与 queue 上限供后续 session/RPC 层执行；本次 codec 仅执行单 frame payload 上限，不实现 stream 管理、queue、AUTH/加密或 RPC 操作。

此 framing 不赋予任何 MTP opcode，也不规定 payload 中的 RPC 语义。MTP 命令分派仍须遵循目标 Android 分支的 AOSP/vendor 实现；MTP transaction 不能被解释为任意字节流。

AOSP `frameworks/av/media/mtp` 当前主线源码使用标准 MTP 命令分派；本仓库的上游笔记记录了核对的 commit 和调用链。此核对不能证明厂商扩展 opcode 在所有 Android 分支中没有冲突。拟议扩展 opcode 在对照目标设备的 AOSP/vendor 源码并获得设备侧验证前，不应视为已分配或兼容。
