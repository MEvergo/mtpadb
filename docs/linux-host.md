# Linux 主机：Stage A

## 运行条件

需要内核启用 configfs、FunctionFS、`dummy_hcd` 和 `libcomposite`。项目不会自动绑定任意物理 UDC：`mtpadb-gadgetd` 只接受其 driver 链接名为 `dummy_udc` 的 UDC。这个限制保护现有 USB 控制器和真实设备。

确认 configfs 已挂载：

```sh
findmnt -T /sys/kernel/config -no FSTYPE
```

装载驱动并在前台启动：

```sh
sudo modprobe dummy_hcd
sudo modprobe libcomposite
sudo modprobe usb_f_fs
sudo build/linux/mtpadb-gadgetd run --config linux/host.conf.example
```

程序在 `/sys/kernel/config/usb_gadget/mtpadb` 建立设备，在 `/dev/ffs-mtpadb` 挂载 FunctionFS，并只在 `dummy_hcd` 的 UDC 上绑定。退出时解除绑定、卸载自己的 FunctionFS 挂载并移除本项目创建的 configfs 项。

## 检查

在另一终端运行：

```sh
lsusb -v
adb devices -l
```

VID/PID 是测试样例值，正式测试请换成你获配的 ID，并用该 ID 在 `lsusb -v` 输出中定位 gadget。ADB interface descriptor 是 `ff/42/01`，包含 bulk OUT、bulk IN；当前阶段不实现 ADB 消息流，因此不会完成 `CNXN`。

端到端自动检查：

```sh
sudo tests/integration/stage-a-adb-usb.sh
```

该脚本需要 root 管理 gadget，并启动临时 home 下的独立 stock adb server（默认端口 5038），设置 `ADB_TRACE=usb`，关闭 mDNS 与模拟器 TCP 端口扫描。它先记录虚拟 gadget 接入前的 USB 序列号列表，再确认新 serial 出现；只向本次 server PID 发送 SIGTERM，不访问或停止默认 5037 server。临时 home 会在退出时删除。端口占用时可指定空闲端口：

```sh
sudo env MTPADB_TEST_ADB_PORT=5039 tests/integration/stage-a-adb-usb.sh
```

## 配置

`linux/host.conf.example` 支持 `manufacturer`、`product`、`serial`、`vid`、`pid` 和可选 `udc`。`vid`/`pid` 接受十进制或 `0x` 十六进制。显式 UDC 仍须是 `dummy_hcd`。

## 手动清理

daemon 正常收到 SIGINT/SIGTERM 时会清理。若异常退出后遗留资源：

```sh
sudo build/linux/mtpadb-gadgetd cleanup
```

清理逻辑仅处理 `/sys/kernel/config/usb_gadget/mtpadb` 和 `/dev/ffs-mtpadb`；会拒绝来源/类型不匹配的挂载。不要手动删除其它 gadget 或卸载其它 FunctionFS 实例。
