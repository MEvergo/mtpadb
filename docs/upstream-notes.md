# Upstream and host compatibility notes

## Linux USB gadget interfaces

Inspected upstream Linux sources at commit [`72d3fcf802c45d00b300f25b848a93c3a2bd7c7e`](https://github.com/torvalds/linux/commit/72d3fcf802c45d00b300f25b848a93c3a2bd7c7e), whose commit message is `Linux 7.3-rc5` (2026-09-27):

- [`Documentation/usb/gadget_configfs.rst`](https://github.com/torvalds/linux/blob/72d3fcf802c45d00b300f25b848a93c3a2bd7c7e/Documentation/usb/gadget_configfs.rst): gadget identity/configuration/function links are configured before writing the selected UDC name to `UDC`; teardown unbinds before removing links and nodes.
- [`Documentation/usb/functionfs.rst`](https://github.com/torvalds/linux/blob/72d3fcf802c45d00b300f25b848a93c3a2bd7c7e/Documentation/usb/functionfs.rst): userspace opens ep0 and writes descriptors/strings; data endpoint files follow descriptor order; closing all FunctionFS files disables the function.
- [`Documentation/usb/functionfs-desc.rst`](https://github.com/torvalds/linux/blob/72d3fcf802c45d00b300f25b848a93c3a2bd7c7e/Documentation/usb/functionfs-desc.rst) and [`include/uapi/linux/usb/functionfs.h`](https://github.com/torvalds/linux/blob/72d3fcf802c45d00b300f25b848a93c3a2bd7c7e/include/uapi/linux/usb/functionfs.h): the v2 descriptor header is little-endian and carries per-speed descriptor counts; descriptor and string blobs are written to ep0.
- [`drivers/usb/gadget/udc/dummy_hcd.c`](https://github.com/torvalds/linux/blob/72d3fcf802c45d00b300f25b848a93c3a2bd7c7e/drivers/usb/gadget/udc/dummy_hcd.c): `dummy_hcd` supplies a virtual host controller and UDC without USB hardware; controller instance names are runtime-created.

## ADB USB contract

Fetched `platform/packages/modules/adb` branch `main` from `android.googlesource.com` at commit [`1cf2f017d312f73b3dc53bda85ef2610e35a80e9`](https://android.googlesource.com/platform/packages/modules/adb/+/1cf2f017d312f73b3dc53bda85ef2610e35a80e9/). Its `adb.h` defines `ADB_CLASS = 0xff`, `ADB_SUBCLASS = 0x42`, and `ADB_PROTOCOL = 0x01`. The current upstream `client/usb_libusb.cpp` was also inspected from the AOSP Gitiles `main` source page (blob `085276ebc4d2344e447bfd077a3a6137d5744663`).

The local unmodified platform-tools executable reports Android Debug Bridge 1.0.41, version `37.0.0-android-tools`. Whether this exact installed build claims the virtual FunctionFS interface is verified by the Stage A runtime check, not inferred from source inspection.

## Current host

- Kernel: `7.2.6-1-cachyos` (`x86_64`).
- `configfs` is mounted at `/sys/kernel/config`.
- Kernel modules are available for `dummy_hcd`, `libcomposite`, and FunctionFS (`usb_f_fs`).
- Before loading the gadget modules, `/sys/class/udc` had no entries. After directly loading `dummy_hcd` and `libcomposite`, the kernel exposed a UDC. Its observed name is not a configuration constant; code must enumerate `/sys/class/udc` at runtime.
- `libusb-1.0` development package version: `1.0.30`.

## Android MTP sources and behavior

Fetched `platform/frameworks/av` branch `main` at commit [`e2f098935447ca4945946de5cb69db843fe3f003`](https://android.googlesource.com/platform/frameworks/av/+/e2f098935447ca4945946de5cb69db843fe3f003/).

- `media/mtp/MtpFfsHandle.cpp`, `MtpDescriptors.cpp`, and their headers use `/dev/usb-ffs/mtp/ep0` through `ep3`. `MtpServer` selects this FunctionFS backend when `ep0` is accessible; otherwise it uses `MtpDevHandle`, which opens `/dev/mtp_usb`.
- `IMtpHandle` is the shared byte/file/event interface implemented by both backends. Keep transport selection below the MTP transaction and RPC layers.
- The AOSP interface is Still Image class/subclass/protocol `0x06/0x01/0x01` with three data endpoints: bulk IN at ep1, bulk OUT at ep2, and interrupt IN at ep3. Current descriptors include FS, HS, SS, and Microsoft OS descriptors. Endpoint packet sizes are 64, 512, and 1024 bytes by speed.
- `mtp.h` defines a 12-byte little-endian container header. `MtpRequestPacket::read` accepts a command container with 0–5 32-bit parameters. `MtpServer::run` serializes command, optional data, and response transactions; `MtpDataPacket` uses a 16 KiB buffer. The RPC frame therefore needs its own fragmentation and bounded reassembly.
- `MtpFfsHandle::write` requests a terminating zero packet when the transfer length is an exact endpoint-packet multiple; packet size comes from `FUNCTIONFS_ENDPOINT_DESC`.
- `mtp.h` declares Android operations `0x95C1`–`0x95C5`. It does not declare `0x95A0`–`0x95AF`; `MtpServer::handleRequest` responds to unknown operations with `MTP_RESPONSE_OPERATION_NOT_SUPPORTED`. This is only an AOSP-main collision check, not an OEM collision check or authorization to reserve that range.

No Android device or target ROM source is present, so OEM-specific operation collisions, runtime endpoint paths, stock MTP regressions, and hardware behavior remain unverified.

## AOSP `mtpadbd` integration pins

Inspected the exact pinned AOSP sources and target definitions before preparing the branch-specific patches:

- Android 11 baseline: `platform/system/core`, tag `android-11.0.0_r48`, commit [`348efca472d810d3152568913da41a081893a4e3`](https://android.googlesource.com/platform/system/core/+/refs/tags/android-11.0.0_r48/). The ADB source path is `adb/`; `adb/Android.bp` defines the existing `adbd` binary from `daemon/main.cpp`, and `adb/daemon/main.cpp` is its entry path.
- Modular ADB: `platform/packages/modules/adb` at commit [`1cf2f017d312f73b3dc53bda85ef2610e35a80e9`](https://android.googlesource.com/platform/packages/modules/adb/+/1cf2f017d312f73b3dc53bda85ef2610e35a80e9/). The root `Android.bp` defines `adbd_binary_defaults` and `adbd` using `daemon/main.cpp`.

Both pinned daemon entry paths call `adbd_auth_init()`, which initializes stock ADB auth and invokes `adbd_wifi_init()`; the Android implementation starts a property observer for `persist.adb.tls_server.enable`. The AOSP startup path also starts USB ADB when FunctionFS is available and otherwise honors legacy TCP properties or defaults to TCP/VSOCK, then initializes mDNS with those addresses. The Android 11 `drop_privileges()` path also installs the local ADB smart-socket listener when running with privileges. The `MTPADB_PROJECT_DAEMON`-only mode added by these patches is set before `adbd_main()`: it skips stock auth/property-observer initialization and all boot-time USB, TCP/VSOCK, smart-socket, and mDNS listener startup while retaining AOSP privilege dropping, service dispatch, and the branch's AOSP TLS/pairing dependencies. The ordinary `adbd` target does not receive the project define and keeps its existing behavior. Pairing mDNS remains owned by `mtpadbd` through the pinned AOSP mDNS implementation when a later root-only project control path explicitly enables wireless mode.

The project checkout path for these integrations is `$ANDROID_BUILD_TOP/external/mtpadb`. Apply one adapter from that checkout with `android/mtp-patch/apply.sh android-11` or `android/mtp-patch/apply.sh modular-adb`; the script checks the selected source repository path and exact commit, checks every patch before applying the series, and reports fully applied patches on re-run. The patches add only the `mtpadbd` binary and its target-specific entry mode; they do not add unresolved project-module dependencies or start `/system/bin/adbd`.

Recorded Android build commands (not run in this task):

```sh
source build/envsetup.sh && lunch aosp_arm64-eng && m mtpadbd
```

Run the command in a full AOSP checkout after applying each corresponding patch. The Android Soong build and device/runtime checks were not run under the controller's task constraint; the project workspace has no full Android build tree or Android device. `mtprpcd` and `mtpadbctl` do not yet have real Soong modules and are deferred to the later implementation tasks.
