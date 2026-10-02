# Android AOSP integration

This directory contains the project Android modules, init services, SELinux policy, deployment helper, and branch-pinned AOSP patches for a separate `mtpadbd`. It does not replace or start `/system/bin/adbd`; the stock binary and its behavior remain unchanged.

## Pinned source trees

| Integration | ADB repository in the AOSP checkout | Exact revision |
| --- | --- | --- |
| Android 11 baseline | `$ANDROID_BUILD_TOP/system/core`, source under `adb/` | Tag `android-11.0.0_r48`, commit `348efca472d810d3152568913da41a081893a4e3` |
| Modular ADB | `$ANDROID_BUILD_TOP/packages/modules/adb` | Commit `1cf2f017d312f73b3dc53bda85ef2610e35a80e9` |

## Prepare a source build

From a full AOSP checkout, set `ANDROID_BUILD_TOP` and run exactly one adapter:

```sh
export ANDROID_BUILD_TOP=/path/to/aosp
./android/deploy/prepare-aosp.sh android-11
# or: ./android/deploy/prepare-aosp.sh modular-adb
```

The helper exposes this checkout as `$ANDROID_BUILD_TOP/external/mtpadb` with a symlink, then calls `android/mtp-patch/apply.sh`. It does not copy project sources. The patch script checks the exact ADB commit, preflights the complete patch series, and is idempotent for a fully patched checkout; it rejects a partial/conflicting state. Do not apply one branch's patches to the other branch.

Include the package/policy fragment from the target product makefile:

```make
$(call inherit-product, external/mtpadb/android/deploy/mtpadb_product.mk)
```

That fragment adds `mtpadbd`, `mtprpcd`, and `mtpadbctl` to `PRODUCT_PACKAGES`, copies `android/init/mtpadbd.rc` into `/system/etc/init`, and adds `android/sepolicy` through `PRODUCT_PRIVATE_SEPOLICY_DIRS` (Android 11+). `mtprpcd` keeps its Soong `init_rc` declaration. No `adb push` deployment is used.

Build the target modules from the AOSP root:

```sh
source build/envsetup.sh
lunch aosp_arm64-eng
m mtpadbd mtprpcd mtpadbctl mtpadb
```

The first three are device targets; `mtpadb` is a Linux AOSP host target linked to the branch's host ADB API. The project `mtpadbd` bootstrap skips stock startup listeners and remains a separate binary. It starts only the root-protected local control socket at boot; pairing/TLS listeners and project mDNS are enabled only by `mtpadbctl wireless pair-start --bind-address <wifi-ip>`.

## Identity and private data

`mtprpcd` requires root-owned, mode-`0600` files with exact lengths:

- `/data/adb/mtpadb/device.id`: 16 raw bytes.
- `/data/adb/mtpadb/device.key`: 32 raw PSK bytes.

A product must provision these through its protected device-provisioning mechanism before `mtprpcd` can serve RPC. They are not generated from source-controlled defaults. The host key is `/etc/mtpadb/devices/<lowercase-hex-device-id>.key`, 32 bytes, owned by the invoking host user and not accessible to group/other users. Paired ADB peer keys are stored separately under `/data/misc/adb/mtpadb/wireless/`; `wireless stop` retains them and `wireless revoke <fingerprint>` removes only the selected peer.

The project mDNS code uses the project-owned `persist.mtpadb.wifi.guid` property rather than writing the stock `persist.adb.wifi.guid` property. Only the project policy grants access to that property and to `mdnsd` startup/registration.

## Wireless ADB operation

`mtpadbctl` must run with effective UID 0. The init-managed `/dev/socket/mtpadbd-control` is `0600 root:root`; the daemon additionally checks `SO_PEERCRED`. Example flow:

```sh
mtpadbctl wireless status
mtpadbctl wireless pair-start --bind-address <device-wifi-ip>
# On the host, use the printed pairing endpoint and one-time code:
adb pair <device-wifi-ip>:<pairing-port>
# Use the separate printed TLS connection endpoint:
adb connect <device-wifi-ip>:<connect-port>
mtpadbctl wireless status
mtpadbctl wireless stop
```

Pairing and TLS connection use separate dynamic ports, selected by the pinned AOSP implementation. The pairing listener closes after success or the 120-second window. The TLS connection listener persists until stop. The project advertises `_adb-tls-pairing._tcp` and `_adb-tls-connect._tcp` only while wireless mode is active and scopes registration to the selected interface. If registration fails, status reports the missing advertisement while manually connecting to the displayed IP/ports remains possible.

The AOSP UID/GID, capability, and SELinux privilege-drop operations remain unchanged; the project path only suppresses the stock local smart-socket listener. Wireless startup requires the daemon's effective UID to remain root for root-owned peer storage; if the target's AOSP policy drops it to shell, `pair-start` fails closed. Do not bypass AOSP privilege policy. Generic OEM and root-module compatibility is unsupported without branch-specific source, init, and SELinux review.

Stock `adb pair`/`adb connect` establish the official AOSP pairing/TLS transport. The host `mtpadb` tool then asks the stock local ADB server to open `mtpadb:rpc`; `mtpadbd` forwards that byte stream through `/dev/socket/mtprpcd-adb` to `mtprpcd`. MTPADB's own PSK/AEAD authentication is a second layer inside the established ADB TLS channel. The RPC implementation currently provides `ping` and binary `echo`, not a general MTP operation bridge.

## Verification boundary

The pinned Android 11 Soong targets (`mtpadbd`, `mtprpcd`, `mtpadbctl`) and the KernelSU ZIP have been built in a shallow/sparse AOSP tree; ZIP integrity, executable modes, initrc contents, and all three Android 30 AArch64 ELF binaries were checked. This was a target-module build, not a complete system image build: `.installable_files` lists `system/etc/init/mtpadbd.rc`, but its product copy was not materialized by the module-only build. `sepolicy_tests`, file-context checks, actual init service startup, official `adb pair`/`adb connect`, mDNS, and device MTP regression still require product/device validation.

## SukiSU Ultra / KernelSU module

The installable root-module package reuses the AOSP-built daemons and registers them through KernelSU `initrc/` injection. See [`kernelsu/README.md`](kernelsu/README.md) for build, installation, identity provisioning, and runtime requirements.
