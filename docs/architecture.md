# MTPADB 架构

仓库里有两条互不替代的实现路径：Android 侧的无线 ADB/MTPADB 私有 RPC，以及 Linux Stage A 的虚拟 USB ADB 接口枚举。无线实现不改变 Android 设备的物理 USB-MTP 配置；Stage A 也不模拟 Android 无线服务。

## Android 无线与私有 RPC 数据流

```text
Android root shell
  mtpadbctl wireless pair-start --bind-address <wifi-ip>
       │ root-only AF_UNIX /dev/socket/mtpadbd-control
       ▼
  mtpadbd: explicit AOSP pairing/TLS listeners + mDNS
       │
       ├── official pairing port (temporary, separate port)
       └── official TLS ADB connection port
                    ▲
                    │ stock adb pair / adb connect
Linux host         │
  stock adb client/server (local smart socket, default 5037)
       │ select the paired serial; OPEN("mtpadb:rpc")
       ▼
  mtpadbd ADB service dispatcher
       │ private AF_UNIX stream /dev/socket/mtprpcd-adb
       ▼
  mtprpcd: MTPX PSK authentication, AEAD, streams, bounded queues
       ▲
       │
  AOSP host target `mtpadb`: MTPX client and protected host PSK
```

The host client does not connect directly to the device's pairing or TLS port. The stock ADB server owns the wireless TLS transport and device selection. The ADB TLS connection carries both ordinary ADB services and the custom `mtpadb:rpc` service.

## 独立安全层

1. **ADB wireless layer:** the pinned AOSP branch implements official pairing, host authorization, TLS transport, standard ADB packet framing, and shell/sync services. Pairing and connection endpoints are distinct, dynamic ports.
2. **MTPADB RPC layer:** after `OPEN("mtpadb:rpc")`, `mtpadbd` forwards the byte stream to the private `mtprpcd` Unix socket. The host and `mtprpcd` then perform a separate PSK handshake and ChaCha20-Poly1305 session using MTPX frames.
3. **Local control boundary:** `mtpadbctl` communicates with `mtpadbd` through a root-only init socket. Wireless listeners and mDNS are absent until an explicit `pair-start`; the private RPC daemon does not bind a network socket.

There is no legacy ADB TCP 5555 listener, second MTPADB TCP/UDP RPC endpoint, direct host-to-device custom TLS client, or replacement of `/system/bin/adbd`. `mtpadbd` retains the selected AOSP branch's privilege-drop path. Pairing fails closed if the process does not retain effective UID 0 for the root-owned peer store.

The current MTPADB RPC operations are authenticated `ping` and binary `echo`. This path is not yet an implementation of arbitrary USB-MTP commands or a physical MTP transaction bridge; the real USB-MTP function remains unchanged.

## Linux Stage A 边界

Stage A creates a Linux configfs + FunctionFS gadget on `dummy_hcd`. It advertises ADB interface class/subclass/protocol `0xff/0x42/0x01`, one bulk OUT endpoint, and one bulk IN endpoint. FunctionFS owns endpoint assignment; userspace addresses endpoints in descriptor order. The daemon holds ep0 and both endpoint files open until shutdown.

Stage A does not implement ADB `CNXN`, ADB authentication, shell, sync, wireless pairing, MTP RPC, Android code, or physical-phone access. Enumeration is not an online ADB transport, and Stage A verification does not prove Android AOSP build or wireless interoperability.

The gadget manager requires configfs, FunctionFS, `libcomposite`, and a UDC whose driver is `dummy_udc`. It creates only its project-owned `mtpadb` gadget and `/dev/ffs-mtpadb` mount; cleanup does not reset unrelated gadgets or kill desktop MTP processes.

## AOSP deployment and verification boundary

The project checkout is exposed at `$ANDROID_BUILD_TOP/external/mtpadb` by `android/deploy/prepare-aosp.sh`; it creates a symlink rather than copying source. It applies the exact Android 11 `system/core` or modular ADB patch series, and the product fragment adds the project binaries and private SELinux policy.

The exact AOSP pins and apply/build steps are in [Android integration](../android/README.md) and [upstream notes](upstream-notes.md). Soong, SELinux, init, pairing/TLS, mDNS, and Android device checks require a full pinned AOSP tree/device and have not been run in this workspace.
