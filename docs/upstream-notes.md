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

The project patches are based on these exact source revisions:

- Android 11 baseline: [`platform/system/core`, tag `android-11.0.0_r48`, commit `348efca472d810d3152568913da41a081893a4e3`](https://android.googlesource.com/platform/system/core/+/refs/tags/android-11.0.0_r48/); ADB source is under `adb/`.
- Modular ADB: [`platform/packages/modules/adb`, commit `1cf2f017d312f73b3dc53bda85ef2610e35a80e9`](https://android.googlesource.com/platform/packages/modules/adb/+/1cf2f017d312f73b3dc53bda85ef2610e35a80e9/).

`android/deploy/prepare-aosp.sh {android-11|modular-adb}` exposes the current checkout at `$ANDROID_BUILD_TOP/external/mtpadb` using a symlink, then calls `android/mtp-patch/apply.sh`. The patch script verifies the selected repository path and exact commit, preflights the complete numbered patch series, applies it atomically through a temporary index, and recognizes a fully applied series on repeat. A partial or conflicting patch state is rejected. This flow does not copy project files or use `adb push`.

The patch series add a distinct `mtpadbd` Soong target and `mtpadb:rpc` ADB service route, explicit root-controlled pairing/TLS startup using the pinned AOSP pairing/TLS/mDNS implementation, and a Linux AOSP host target named `mtpadb`. Project `mtprpcd` and `mtpadbctl` are real Soong modules. The `mtpadbd` entry path bypasses stock boot listener/auth initialization; AOSP UID/GID, capability, and SELinux privilege-drop operations plus standard service dispatch remain intact. It neither replaces nor starts `/system/bin/adbd`.

The project mDNS helper uses `persist.mtpadb.wifi.guid` and the project property type instead of changing stock `persist.adb.wifi.guid`/`adbd_config_prop`. Wireless mode binds only the explicit local numeric IP, exposes separate dynamic pairing and TLS connection endpoints, and requires effective UID 0 to write the root-owned peer store. The init control socket is local and root-only; the `mtprpcd-adb` socket is SELinux-restricted to the project daemon.

The target product includes `external/mtpadb/android/deploy/mtpadb_product.mk`, which packages `mtpadbd`, `mtprpcd`, and `mtpadbctl` and adds project policy through `PRODUCT_PRIVATE_SEPOLICY_DIRS`. The source build command is:

```sh
source build/envsetup.sh
lunch aosp_arm64-eng
m mtpadbd mtprpcd mtpadbctl mtpadb
```

This command was not run: the workspace has no `ANDROID_BUILD_TOP` or full AOSP/Soong tree. `sepolicy_tests`, file-context validation, init service startup, AVC review, stock `adb pair`/`adb connect`, mDNS, and Android-device behavior were also not run. The Linux CMake/CTest suite and the pinned patch-application/idempotence smoke checks do not substitute for those Android checks. The supplied `tests/adb/mtpadb_host_integration.sh` also requires a built AOSP host target, stock local ADB server, and paired target; it was not run here.

The Linux Stage A path remains independent: it uses only `dummy_hcd` for USB enumeration and does not implement ADB `CNXN`. The MTPX RPC operations currently exercised through the ADB service are `ping` and binary `echo`; no arbitrary physical USB-MTP command bridge is claimed.
