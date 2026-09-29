# Android AOSP integration

This directory contains branch-pinned source patches for building a separate, listener-disabled `mtpadbd` binary from AOSP ADB. It does not add an init service, product package, Android project daemon modules, or a replacement for `/system/bin/adbd`. The later Android implementation tasks add the real project modules and explicitly controlled wireless startup.

## Pinned source trees

| Integration | ADB repository in the AOSP checkout | Exact revision |
| --- | --- | --- |
| Android 11 baseline | `$ANDROID_BUILD_TOP/system/core`, source under `adb/` | Tag `android-11.0.0_r48`, commit `348efca472d810d3152568913da41a081893a4e3` |
| Modular ADB | `$ANDROID_BUILD_TOP/packages/modules/adb` | Commit `1cf2f017d312f73b3dc53bda85ef2610e35a80e9` |

Keep this project checkout at `$ANDROID_BUILD_TOP/external/mtpadb`. The source patches are scoped to the corresponding pinned ADB repository and do not modify the platform's existing `adbd` target.

## Apply a source patch

Use a full AOSP checkout containing the selected repository at exactly the pinned revision. From the project checkout, select exactly one adapter:

```sh
cd "$ANDROID_BUILD_TOP/external/mtpadb"
./android/mtp-patch/apply.sh android-11
# or, for a modular-ADB checkout:
./android/mtp-patch/apply.sh modular-adb
```

`apply.sh` requires `ANDROID_BUILD_TOP`, checks that the expected repository and daemon/build files exist, and compares the repository `HEAD` with the exact pinned commit before changing files. It runs `git apply --check` across the complete selected patch series before applying anything. Re-running against a fully patched checkout prints `already applied: ...`; a mixed or conflicting patch state is rejected without applying more files. Do not copy either patch into the other branch's ADB source path.

## Build the safe target

On the pinned Android 11 source tree, after applying its patch:

```sh
cd "$ANDROID_BUILD_TOP"
source build/envsetup.sh && lunch aosp_arm64-eng && m mtpadbd
```

Repeat the same build command in the AOSP checkout that contains the pinned modular `packages/modules/adb` integration target, after applying `modular-adb`.

Each adapter adds `mtpadbd` as a separate Soong `cc_binary` based on that branch's ADB daemon target and existing AOSP service, pairing, and TLS dependencies. It does not alias or replace `adbd`, install an init rule, or add a product package. Building the module alone does not start it or make it the system daemon.

The dedicated `MTPADB_PROJECT_DAEMON` entry mode is set in `main()` before `adbd_main()`. In this bootstrap target it bypasses stock ADB auth initialization (including the `persist.adb.tls_server.enable` observer), local ADB smart-socket setup where present, USB ADB startup, property-driven legacy TCP/VSOCK startup, and their mDNS setup. It still executes the AOSP privilege-drop path and event loop/service dispatch, and it links the pinned AOSP TLS and pairing implementation. The existing `adbd` binary builds without the project define and retains its AOSP behavior. Wireless pairing/connect listeners and project trust initialization are intentionally not started here; later tasks expose them only through the root-only project control socket.

No project-module dependencies are added here: `mtprpcd` and `mtpadbctl` do not exist yet, so this patch has no placeholder Soong modules. Their real targets belong in the later implementation tasks.

## Verification boundary

The required Android Soong build command is recorded above for both source pins. It is not run in the Linux-only project workspace; verify it in a complete AOSP checkout before integrating the binary into a product. A successful module build is not a device/runtime verification of init, SELinux, pairing, TLS, or mDNS behavior.
