# Android 侧移植与 AOSP 集成

仓库现在包含独立的 `mtpadbd` AOSP source patches、`mtprpcd`、`mtpadbctl`、init RC、SELinux policy 和产品集成片段。它们不修改 stock `/system/bin/adbd`；wireless listeners 默认关闭，只能由 root-only control socket 显式开启。

## AOSP source pins 与部署

| Target | ADB source | Pin |
|---|---|---|
| Android 11 reference | `platform/system/core/adb` | `android-11.0.0_r48`, commit `348efca472d810d3152568913da41a081893a4e3` |
| Modular ADB | `platform/packages/modules/adb` | commit `1cf2f017d312f73b3dc53bda85ef2610e35a80e9` |

在完整 AOSP checkout 中：

```sh
export ANDROID_BUILD_TOP=/path/to/aosp
./android/deploy/prepare-aosp.sh android-11
# 或选择 modular-adb
```

部署 helper 将当前 project checkout 以 symlink 暴露到 `$ANDROID_BUILD_TOP/external/mtpadb`，然后调用 `android/mtp-patch/apply.sh`。脚本验证所选 ADB repository 的精确 commit，并预检完整 patch series；混合/冲突状态拒绝修改。不要复制 patch 到另一分支或用 `adb push` 安装模块。

在目标 device product makefile 中包含：

```make
$(call inherit-product, external/mtpadb/android/deploy/mtpadb_product.mk)
```

该片段加入 `mtpadbd`、`mtprpcd`、`mtpadbctl`、产品私有 SELinux policy；AOSP patch 为 `mtpadbd` 增加 Soong `init_rc` 依赖，`mtprpcd` 模块引用项目 RC。构建命令：

```sh
source build/envsetup.sh
lunch aosp_arm64-eng
m mtpadbd mtprpcd mtpadbctl mtpadb
```

Android 侧必须由产品受保护的 provisioning 流程在 `/data/adb/mtpadb/` 提供 16-byte raw `device.id` 和 32-byte raw `device.key`，两者 root-owned、`0600`。项目不在源码或构建日志中生成/嵌入 PSK。host key 文件为 `/etc/mtpadb/devices/<lowercase-hex-device-id>.key`。配对 peer key 单独保存在 `/data/misc/adb/mtpadb/wireless/`。

## Wireless ADB 启停与数据流

AOSP 分支的 official pairing server、TLS ADB listener、host authorization、ADB service dispatch 与 mDNS 被复用。`mtpadbd` entry mode 跳过 stock auth/property observer 及 USB、legacy TCP/VSOCK、smart-socket 和 boot-time mDNS listener startup；AOSP UID/GID、capability 与 SELinux privilege-drop 操作及 event loop/service dispatch 保持不变。系统 `adbd` target 无此 project define，原行为不变。

`mtpadbctl wireless pair-start --bind-address <wifi-ip>` 显式绑定本机非 wildcard 数字 IP，打开两条独立动态端口：临时 pairing 和 TLS connect。pairing timeout 为 120 秒，成功配对后 pairing listener 关闭；connect listener 保持到 stop。由 `mtpadbd` 向所选接口注册 `_adb-tls-pairing._tcp` / `_adb-tls-connect._tcp`。mDNS 错误不关闭已打开的 IP listener，`status` 会显示发布状态，用户可手动使用两个 endpoint。

host stock `adb pair` 和 `adb connect` 建立标准 ADB TLS transport；host AOSP target `mtpadb` 使用 local ADB server 选择 serial，再打开 `mtpadb:rpc`。`mtpadbd` 把 service stream 经私有 AF_UNIX socket 送至 `mtprpcd`。标准 ADB TLS 与 MTPADB PSK/AEAD 是独立的嵌套安全层；当前 RPC 操作只有 `ping` 和 binary `echo`。

`mtpadbd` 保留 AOSP root policy，不在 user build/ROM 中强制变成 root。pair-start 需要 effective UID 0，否则失败关闭；generic OEM 分支和 root-module mode 仍需单独检查 AOSP ABI、init、SELinux 与 privilege policy。真实 Android 设备上的物理 USB-MTP 回归不由本 wireless RPC 实现覆盖。

## MTP FunctionFS 源码研究

`platform/frameworks/av` 主线 commit `e2f098935447ca4945946de5cb69db843fe3f003` 中，`MtpServer` 通过 `IMtpHandle` 分派 MTP container/operation；`MtpFfsHandle` 使用 FunctionFS ep0/ep1/ep2/ep3，interface 为 `06/01/01`，包括 bulk IN、bulk OUT、interrupt IN。ADB FunctionFS (`ff/42/01`) 与 MTP FunctionFS 是不同 USB functions，不能复用 interface/endpoints。

该源码研究没有定义或分配 MTP vendor opcode，也没有证明 OEM MTP 扩展兼容性。当前 MTPX echo RPC 不是 MTP container handler；真实的 physical MTP 扩展仍需要针对目标 AOSP/vendor 源码与设备验证。

## 验证限制

本工作区没有 `ANDROID_BUILD_TOP`、完整 AOSP/Soong tree 或 Android device。Android `m` 构建、`sepolicy_tests`、file-context checks、init service startup、AVC、stock `adb pair/connect`、mDNS 和 physical MTP regression 均未运行。Linux CMake/CTest 只能证明本机协议与 Unix-socket 行为，不能替代 Android 验证。
