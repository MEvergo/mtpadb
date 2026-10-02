# 安全边界

## Android wireless ADB 与 MTPADB RPC

- `mtpadbd` 是从 pinned AOSP ADB 源码独立构建的二进制；不替换或启动 `/system/bin/adbd`，不开放 USB ADB、legacy TCP 5555 或单独的 MTPADB TCP/UDP listener。
- 默认状态不打开 wireless pairing/TLS listener，也不注册项目 mDNS。只有 root `mtpadbctl wireless pair-start --bind-address <local-wifi-ip>` 才能启动；地址必须是本机显式数字 IP，拒绝 wildcard。配对端口与 TLS connect 端口不同，均由 AOSP 动态分配。
- 官方 ADB pairing/TLS 负责已配对 host 的身份验证和 ADB transport 加密。`mtpadb:rpc` 在该 TLS transport 内另行执行 MTPADB PSK `AUTH`、HKDF 派生与 ChaCha20-Poly1305；通过 ADB pairing 本身不足以认证 MTPADB 私有 RPC。
- 配对窗口为 120 秒；成功配对或窗口到期后关闭 pairing listener，TLS connect listener 继续运行直到 `wireless stop`。`wireless stop` 移除 listener/mDNS 但保留 peer；只有 `wireless revoke <fingerprint>` 删除单个 peer。
- `_adb-tls-pairing._tcp` 与 `_adb-tls-connect._tcp` 由 `mtpadbd` 使用 pinned AOSP mDNS 实现注册，并绑定到所选地址所在接口。mDNS 失败会在 status 中显示；手动 IP/端口仍可用。
- `/dev/socket/mtpadbd-control` 是 `0600 root:root`，control server 还校验 `SO_PEERCRED.uid == 0`。`/dev/socket/mtprpcd-adb` 是 init 提供的本地 stream socket；SELinux 只授权 `mtpadbd` 连接，普通 shell 即使能通过 Unix socket 文件组权限也没有相应 domain 规则。
- Android PSK 与设备 ID 必须分别存放在 `/data/adb/mtpadb/device.key`（32 raw bytes）和 `/data/adb/mtpadb/device.id`（16 raw bytes），root-owned、mode `0600`；host key 位于 `/etc/mtpadb/devices/<lowercase-hex-device-id>.key`，同样为 32 bytes 且不可被 group/other 读取。`mtprpcd` 缺少或拒绝不安全的文件时失败关闭。
- 配对 peer public key 存放于 `/data/misc/adb/mtpadb/wireless/`，与 PSK 分离且由 `mtpadbd` 受限访问。status 只给出 peer fingerprint，不返回公钥、私钥、PSK、pairing code 或 session key。
- SELinux policy 为 `mtpadbd`、`mtprpcd`、私有数据及 init socket 定义专用类型；网络权限只允许 TCP listener bind/accept、接口枚举，以及对 AOSP `mdnsd` 的本地访问。SELinux 必须保持 enforcing，不要以 permissive mode 掩盖 denial。

AOSP 的 UID/GID、capability 和 SELinux privilege-drop 操作保持原样；project 仅关闭 stock local smart-socket listener。无线启用要求 `mtpadbd` 的 effective UID 为 0，以便写入 root-only peer store；如果设备构建按 stock policy 把 daemon 降为 shell，`pair-start` 会拒绝启动，项目不会额外提权或绕过 AOSP 策略。部分 user build、OEM ROM 和 root-module mode 因此不在兼容保证内。

## Linux Stage A

- Linux gadget daemon 需要 root 来创建 configfs gadget、挂载 FunctionFS 并绑定 UDC。
- UDC 选择严格限制为 `dummy_hcd`；禁止误绑定真实硬件 UDC。
- VID/PID 与 serial 来自本地配置。示例 `ffff:ffff` 仅用于测试，不代表 MTPADB 拥有该 VID/PID，也不应伪装真实厂商。
- Stage A 只验证 USB interface enumeration，不完成 ADB `CNXN`，不提供认证或可用 ADB 会话。
- configfs gadget 名和 FunctionFS mountpoint 固定为项目独占路径；检测到挂载类型/来源不符时清理失败关闭。

Android Soong、SELinux、init、pairing/TLS、mDNS 和设备行为需要完整 pinned AOSP tree/Android target；当前 Linux 结果不证明这些平台行为已经通过。
