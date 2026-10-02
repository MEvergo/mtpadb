# 故障排查

## 找不到 dummy UDC

检查内核配置和模块：

```sh
sudo modprobe dummy_hcd
ls -l /sys/class/udc
```

daemon 只绑定 driver 为 `dummy_udc` 的条目；不会回退到真实硬件 UDC。

## configfs 不可用

```sh
findmnt -T /sys/kernel/config -no FSTYPE
```

需要 `configfs` 支持。daemon 可在尚未挂载时尝试挂载，但必须以 root 运行。

## FunctionFS 挂载或 descriptor 写入失败

确认 `/dev/ffs-mtpadb` 未被其他用途占用，并查看内核日志：

```sh
findmnt -T /dev/ffs-mtpadb -no TARGET,FSTYPE,SOURCE
sudo dmesg | tail -n 80
```

项目只会卸载类型为 `functionfs` 且 source 为 `mtpadb` 的挂载。

## adb 看不到测试 serial

确认 gadget 已绑定到 dummy UDC、`lsusb -v` 显示 interface `ff/42/01`，并确认 `linux/host.conf.example` 中 serial 与测试脚本的期望一致。当前阶段没有 ADB `CNXN`，所以枚举不等于 online；不要用 `adb connect` 替代 USB 枚举测试。

## daemon 异常退出后的清理

```sh
sudo build/linux/mtpadb-gadgetd cleanup
```

如果错误指出 FunctionFS mount source 或 UDC 不匹配，不要强行删除系统节点；先检查实际挂载和 UDC 状态。

## AOSP patch 或构建目标缺失

在设置了 `ANDROID_BUILD_TOP` 的完整 AOSP checkout 中运行部署脚本：

```sh
./android/deploy/prepare-aosp.sh android-11
# 或
./android/deploy/prepare-aosp.sh modular-adb
```

脚本要求 ADB 源码处于对应固定提交，并且 `$ANDROID_BUILD_TOP/external` 已存在。它会在 `external/mtpadb` 创建项目符号链接；如果该路径指向其他 checkout，会直接报错。构建前需在设备 product makefile 中包含 `external/mtpadb/android/deploy/mtpadb_product.mk`，再构建 `mtpadbd`、`mtprpcd`、`mtpadbctl` 和 `mtpadb`。

## `mtpadbctl` 无法启动无线模式

`mtpadbctl` 必须以 effective UID 0 运行，并连接 `/dev/socket/mtpadbd-control`。检查 `mtpadbctl wireless status`、设备上的 init RC，以及 socket 的 mode 和 SELinux label。`pair-start` 会拒绝 wildcard、非数字地址和当前未分配给设备的地址。

AOSP UID/GID、capability 和 SELinux privilege-drop 操作保持原样；project 仅关闭 stock local smart-socket listener。如果所选产品会将 daemon 降为 shell，wireless start 会因 peer store 必须由 root 拥有而拒绝打开监听器；不要通过放宽 AOSP privilege policy 来绕过。

## 配对或连接端点不可用

配对和 TLS 连接使用不同的动态端口。先使用 `mtpadbctl wireless pair-start` 打印的配对端点和一次性验证码，再用单独的连接端点执行 stock `adb connect`。配对成功或等待 120 秒后，pairing listener 会关闭；TLS listener 会持续运行，直到执行 `mtpadbctl wireless stop`。

如果 mDNS 记录没有发布，检查 `mtpadbctl wireless status` 中对应字段，并手动使用显示的 IP/端口。mDNS 失败不会关闭监听器。如果绑定地址从设备移除，状态机将停止监听器，需要显式重新启动。

## `mtprpcd` 退出或 `mtpadb rpc` 认证失败

`mtprpcd` 会 fail closed：`/data/adb/mtpadb/device.id` 必须恰为 16 bytes，`device.key` 必须恰为 32 bytes；两者都必须是 root-owned 普通文件，mode 为 `0600`（group/other 无权限）。host key 必须与设备使用相同的 32-byte PSK，位于 `/etc/mtpadb/devices/<lowercase-hex-device-id>.key`，由当前 host 用户拥有且 mode 为 `0600`。不要把 PSK 写入日志或源码管理。

ADB TLS 连接 online 并不代表 MTPADB RPC 认证成功：这是两层独立安全机制。检查选中的 serial 和密钥是否匹配；认证失败会故意返回通用 RPC 错误。

## SELinux denial

保持 SELinux enforcing，并检查目标设备上的 AVC 记录。确认 product makefile 已包含 `android/deploy/mtpadb_product.mk`，服务路径由 `android/sepolicy/file_contexts` 标注，源码对应固定的 AOSP 分支。不要添加宽泛的 `allow` 规则或切换 permissive 来压制 AVC；应收窄项目策略，并运行目标分支的 `sepolicy_tests` 和 file-context 检查。

Android Soong、init 启动、SELinux 测试、ADB 配对/TLS 和 mDNS 验证需要完整的固定版本 AOSP build 与 Android 设备；Linux 单元测试不覆盖这些行为。
