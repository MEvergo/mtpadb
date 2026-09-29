# 无线 ADB 与 MTPADB 扩展设计

> 状态：设计已批准，实施计划已保存。  
> 日期：2026-09-29
> `mtpadbctl` 与 `mtpadb` 命令名是设计接口，当前仓库尚未实现。

## 决策摘要

在 AOSP Android 11+ source-build target 上构建独立的 `mtpadbd`；Android 11 基线代码位于 `platform/system/core/adb`，模块化 AOSP 分支位于 `platform/packages/modules/adb`。复用目标分支的现代无线配对、TLS transport、mDNS 与标准 ADB service 实现。stock ADB 与 MTPADB 私有 RPC 共用同一个 TLS ADB **连接端口**；配对端口仍按官方流程单独开放。项目不启动系统 `/system/bin/adbd`，也不新增独立 MTPADB TCP RPC listener。

MTPADB 私有功能通过 ADB `OPEN` service `mtpadb:rpc` 暴露。`mtpadbd` 将该 service 的双向字节流转交给 `mtprpcd` 的本地 Unix socket；`mtprpcd` 在 Android 侧实现项目定义的 MTPX PSK `AUTH`、AEAD、stream 与 backpressure。当前仓库尚无这些 Android 组件，它们属于本功能需新增的实现。真实 USB-MTP 通道保持不变。

## 背景与范围

当前仓库只有 Linux Stage A gadget implementation，没有 Android daemon、Android build files 或目标设备。当前架构把 Android wireless transport 列为非目标，并禁止项目 TCP listener；本设计按用户后续要求改为支持无线 ADB，但只开放由用户显式启动的 ADB TLS pairing/connect 流程。

### 本设计包含

首轮实现必须包含以下组件和验收面。

- AOSP Android 11+ source-build reference target。
- 项目自有 `mtpadbd`，提供官方兼容的 `adb pair`、TLS ADB connection、mDNS 与 stock ADB services。
- ADB service `mtpadb:rpc`，复用已配对的 TLS ADB connection port。
- Android 本地管理命令及项目独立证书存储。
- Linux host `mtpadb` RPC client，通过本机 stock ADB server 打开 `mtpadb:rpc`。
- 最小 Android init、Android.bp、file contexts 与 SELinux 集成。
- 更新 README 与 architecture/protocol/security/android-porting/troubleshooting 文档，移除已被新设计取代的“无 wireless/无 TCP listener”描述；保留物理 USB-MTP 与 Stage A 边界。

### 本设计不包含

以下能力不属于本次无线设计。

- Android 10 或更早版本的现代 wireless pairing 回移植。
- 调用或替换系统 `/system/bin/adbd`。
- 旧式 `adb tcpip 5555` 或明文 ADB TCP fallback。
- 单独的 MTPADB TCP/UDP RPC listener、私有发现协议或第二个 RPC 网络端口。
- Android GUI 配对界面；配对操作由 root-only 本地命令开启并显示配对信息。
- 任意 OEM ROM 或 root-module mode 的兼容性保证。AOSP source-build 是首个实现和验证目标；root-module 移植继续按具体 ROM 单独验证。

## 方案比较

下表比较可选实现路线及其兼容性、维护成本和决策。

| 方案 | 兼容性与维护成本 | 决定 |
|---|---|---|
| 从目标 AOSP branch 的 ADB 源码复用（Android 11 基线位于 `platform/system/core/adb`，模块化分支位于 `platform/packages/modules/adb`） | 最大化 stock ADB 行为一致性；需跟踪 AOSP 分支依赖并保留 Apache-2.0 attribution | 采用 |
| 从零实现 ADB transport、pairing 与 TLS | 依赖少，但协议、安全和平台行为容易偏离官方实现 | 不采用 |
| 把无线连接委托给系统 `adbd` | 代码量较小，但破坏独立 daemon 要求，并让 MTPADB 服务依赖系统 adbd | 不采用 |

## 组件与数据流

`mtpadbd` 使用目标 AOSP branch 的 ADB 组件处理 pairing/TLS、ADB packet transport、mDNS 和标准 service dispatch；它不依赖系统 adbd binary。AOSP 通常由 framework `AdbDebuggingManager` 注册 pairing mDNS；本项目由 `mtpadbd` 通过 pinned branch 的 AOSP mDNS 实现注册与撤销 pairing service，不依赖 `AdbDebuggingManager`、Settings 或 framework pairing UI。`mtprpcd` 保持 MTPADB RPC 的认证、加密和 stream 管理边界，不直接创建网络 listener。

```text
Android 本地 root shell
  mtpadbctl wireless pair-start --bind-address <wifi-ip>
       │ 临时配对端口 + 一次性配对信息
       ▼
Host stock adb ── adb pair ── AOSP-compatible pairing/TLS
       │
       ├── adb connect <device-ip>:<connect-port>
       │       └── TLS ADB connection listener ── mtpadbd
       │               ├── OPEN shell:/exec:/sync:... → ADB services
       │               └── OPEN mtpadb:rpc
       │                       └── private AF_UNIX stream
       │                               └── mtprpcd: MTPX AUTH/AEAD/RPC
       │
       └── stock adb client/server 的本机 smart socket
               └── mtpadb host client 选择 serial 并请求 mtpadb:rpc
```

现代 ADB pairing port 与 TLS connection port 是两个不同端口，并按 AOSP 实现动态分配；`wireless status` 显示当前 endpoint。stock ADB services 与 `mtpadb:rpc` 共用 connection port 和同一套已配对 TLS trust；不存在额外的 MTPADB 网络端口。wireless mode 开启时，`mtpadbd` 注册官方 `_adb-tls-pairing._tcp` 与 `_adb-tls-connect._tcp` service。

`mtpadb` host client 使用 stock ADB server smart-socket transport 选择设备（`host:transport:<serial>`），然后请求 `mtpadb:rpc` service。ADB server 管理 TLS 连接和 pairing state；service 建立后 host client 读写该 service 的 raw byte stream。该客户端不直接连接 Android 网络端口，也不实现一套新的 ADB TLS client。

该 client 仅使用本机 stock ADB server smart socket（默认 `5037`）；这是 stock ADB 的本地 API，不是 MTPADB 到 Android 的无线 transport。

## 身份验证与密钥

官方 ADB wireless pairing/TLS 负责识别已配对的 host，并保护整个 ADB connection。标准 ADB services 在 TLS ADB authorization 通过后按 AOSP 行为工作。

`mtpadb:rpc` 在 TLS stream 内继续运行 MTPADB RPC 的 `MTPX` framing、PSK `AUTH`、HKDF 派生和 AEAD。这样，配对 ADB host 仍不能只凭 ADB pairing 使用项目私有 RPC；custom host client 需要同时使用 stock ADB pairing state 与对应设备的 MTPADB PSK。USB-MTP 和无线入口共用同一套 MTPADB RPC 认证语义。

项目 TLS peer state 与 MTPADB PSK 存在项目自有 root-only data directory，不读取或修改系统 `/data/misc/adb/adb_keys`。host PSK 延续 `/etc/mtpadb/devices/<device-id>.key` 配置。任何配对码、私钥、PSK 或 session key 都不得写入 daemon log；认证失败只返回通用错误。文件权限和 SELinux label 必须限制为 daemon 所需的最小范围。

## 启停与网络暴露

`mtpadbd` 的独立 entry mode 在进入 `adbd_main` 前启用，默认不启动 USB ADB、legacy TCP、mDNS 或任何 listener；它保留 AOSP privilege dropping 和标准 service dispatch。该模式跳过 stock `adbd_auth_init()`，因此不会启动 `persist.adb.tls_server.enable` observer 或 stock ADB auth context；后续仅 root-only project control socket 可初始化项目 trust context 并显式开启 pairing/TLS transports。普通 `/system/bin/adbd` 的行为保持不变。

- `wireless pair-start`：绑定配置的 Wi-Fi address，打开临时 pairing listener，输出 pairing address、port 和一次性 pairing code；默认 pairing window 为 120 秒，成功配对后立即关闭 pairing listener。
- pairing 完成后，TLS ADB connection listener 保持开启，直到 `wireless stop`。已配对的 client certificate 会保留到显式 `wireless revoke <peer-id>`。
- `wireless status`：显示 listener 状态、pairing/connect endpoints、已配对 peer fingerprint 和 mDNS 状态，不显示密钥。
- `wireless stop`：关闭 pairing/connect listeners 并撤销 mDNS advertisement；不删除已配对证书。

启用 wireless mode 必须给出本机当前 Wi-Fi address；daemon 不默认绑定 `0.0.0.0`。pairing/connect 使用 AOSP wireless ADB 的端口协商和 mDNS service 名称。地址不再属于设备时，listener 停止并要求重新启动，不自动改绑到其他网卡。默认关闭时不得有项目 ADB TCP listener 或 mDNS advertisement。

Android init service、control socket 和 TLS/RPC data directory 使用专用 SELinux type；SELinux 保持 enforcing。新增 allow rule 仅覆盖实际需要的网络、socket、证书文件与 MTPADB daemon 访问。

## 错误与恢复

以下规则定义配对、认证和停止失败时的清理行为。

- Pairing 未显式启动、配对码过期或认证失败时，pairing 请求失败，不创建持久信任。
- 未配对的 TLS client 无法建立 ADB session；撤销后的证书不能重新连接。
- 已配对但 MTPADB PSK 无效的 client 可以完成标准 ADB TLS transport，但 `mtpadb:rpc` 返回通用认证失败；不得影响普通 ADB service 或 MTP server。
- 端口冲突或指定 Wi-Fi address 不属于本机时，wireless mode 启动失败。mDNS 初始化失败时必须报告状态，但仍允许通过 `status` 给出的 IP/port 手动连接；不得回退到明文 TCP、wildcard bind 或其他端口。
- `wireless stop` 关闭所有由本项目创建的 listener、pairing state 的临时内容和 mDNS registration；持久 paired certificates 仅由显式 revoke 删除。

## 验收标准

验收需要同时覆盖 stock ADB、MTPADB 私有 service、AOSP 部署和现有 USB 回归。

### Android 与 stock ADB

在 AOSP Android 11+ target 上使用未修改的 stock platform-tools：

1. wireless mode 关闭时，设备上没有项目 ADB 网络 listener 或项目 mDNS service。
2. 用 `mtpadbctl wireless pair-start --bind-address <device-ip>` 启动后，stock `adb pair <device-ip>:<pair-port>` 可用显示的一次性配对码完成配对。
3. stock `adb connect <device-ip>:<connect-port>` 建立 TLS ADB transport；`adb devices -l` 显示无线 serial。
4. `adb shell id`、`adb shell getprop`、`adb push`、`adb pull`、`adb install`、`adb logcat`、`adb exec-out` 和 `adb forward` 等原项目目标 ADB services 工作。
5. 未配对与已撤销 client 被拒绝；配对超时后 pairing listener 不再接受新 pairing；停止后 connection listener 与 mDNS service 均消失。

### MTPADB custom service

这些检查验证自定义 ADB service 复用 TLS transport 并保留独立的 MTPADB PSK 边界。

1. `mtpadb` host client 使用本机 stock ADB server 对目标 serial 请求 `mtpadb:rpc`，不直连设备的无线端口。
2. 有效 MTPADB PSK 下，`PING`、`echo` 和 1 MiB binary roundtrip 成功。
3. 错误 PSK、重复 sequence、超限 frame 或未授权 RPC 请求被拒绝；认证秘密不进入日志。
4. `mtprpcd` 不新增 TCP/UDP listener；RPC 数据只通过 TLS ADB service 和私有 AF_UNIX socket 到达。

### 回归与部署

这些检查确保无线变更不破坏现有 USB gadget，并可重复部署到 AOSP target。

- Linux Stage A gadget、FunctionFS descriptors 与现有 integration tests 保持通过。
- Android `Android.bp`、init、file_contexts 和 sepolicy 纳入可重复的 AOSP source build；不得以手工复制文件代替部署目标。
- 同步更新相关文档中的 transport、安全、启停和验收说明，不把旧的 no-wireless 限制留在已生效的项目文档中。
- 目标 Android 构建与真机 Wi-Fi 验证必须记录 AOSP branch、platform-tools 版本、listener address/ports、mDNS 结果和测试命令。

## 实施前置条件与风险

本仓库目前没有 Android source tree 或 Android device。因此源码集成可以在仓库内完成，但 Android Soong build、SELinux 验证、真实 Wi-Fi/mDNS 行为和 physical MTP 回归都必须在 AOSP Android 11+ target 上执行，不能由当前 Linux-only build 代替。

ADB pairing/TLS 内部 API 会随 Android branch 变化。实施固定并记录目标 AOSP branch/commit：Android 11 基线使用 `platform/system/core/adb` 的 `android-11.0.0_r48`，模块化分支使用对应的 `platform/packages/modules/adb`。实现和测试必须针对实际目标源码；不能把当前 `main` 的私有 API 假定为所有 OEM branch 的稳定 ABI。root-module mode 需要另行针对 ROM 检查 ABI、init 和 SELinux 集成。

## 上游依据

以下官方文档与源码定义 pairing、TLS 和 host smart-socket 的行为。

- [Android Developers: ADB wireless debugging](https://developer.android.com/tools/adb)：`adb pair` 与 `adb connect` 的官方使用流程。
- [AOSP: Architecture of ADB Wi-Fi](https://android.googlesource.com/platform/packages/modules/adb/+/HEAD/docs/dev/adb_wifi.md)：pairing 与 TLS connect service 的职责边界和 mDNS service。
- [AOSP Android 11: pairing connection implementation](https://android.googlesource.com/platform/system/core/+/refs/tags/android-11.0.0_r48/adb/pairing_connection/pairing_connection.cpp)：Android 11 基线的官方 pairing 实现。
- [AOSP: ADB host client implementation](https://android.googlesource.com/platform/packages/modules/adb/+/refs/heads/main/client/adb_client.cpp)：stock host ADB server 的 transport/service 请求路径。
- [当前仓库架构说明](../../architecture.md)：原有 USB/MTP 与 ADB service 分层；本设计仅新增 ADB wireless ingress 和 `mtpadb:rpc` service，不替换真实 USB-MTP 数据路径。
