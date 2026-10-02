# MTPADB SukiSU Ultra / KernelSU module

This module starts the existing AOSP-built `mtpadbd` and `mtprpcd` binaries on a rooted device. It does not replace `/system/bin/adbd`; it registers a separate TLS ADB endpoint and preserves the existing init-managed local sockets.

## Module requirements

- SukiSU Ultra or KernelSU installed and granting root access.
- KernelSU runtime mode `built-in` or regular `lkm`. KernelSU's `initrc/` injection is unavailable in `late-load` mode and must not be disabled when patching the kernel. The module has no `system/` overlay, so it does not require a metamodule.
- An AOSP Android build whose ABI matches the target device. The module packages binaries from the selected AOSP `ANDROID_PRODUCT_OUT`; it does not build them itself. The repository's AOSP ports are pinned to Android 11 and one modular ADB revision, so compatibility with a vendor Android 14 ROM still requires device testing.
- The daemons run as `u:r:ksu:s0`. SukiSU Ultra's [KSU policy](https://github.com/SukiSU-Ultra/SukiSU-Ultra/blob/main/kernel/selinux/rules.c) marks this domain permissive and grants it broad access; this module therefore runs as highly privileged root services rather than in the narrower domains from AOSP product integration. SELinux remains enforcing system-wide.

SukiSU Ultra describes itself as a KernelSU fork. Its repository does not publish a separate module-authoring guide; this package follows the upstream [KernelSU module guide](https://kernelsu.org/guide/module.html), including `module.prop`, module boot scripts, and `initrc/` service injection. The [SukiSU Ultra project](https://github.com/SukiSU-Ultra/SukiSU-Ultra) documents its kernel/root installation requirements.

## Build the installable ZIP

First prepare one of the AOSP branches documented in [`../README.md`](../README.md), include `external/mtpadb/android/deploy/mtpadb_product.mk` in the product, and build the Android modules:

```sh
source build/envsetup.sh
lunch aosp_arm64-eng
m mtpadbd mtprpcd mtpadbctl
./android/kernelsu/package-module.sh \
  --product-out "$ANDROID_PRODUCT_OUT" \
  --output "$ANDROID_PRODUCT_OUT/mtpadb-kernelsu.zip"
```

The packager refuses to overwrite an existing ZIP and fails if any required executable is missing. Install the ZIP from the SukiSU Ultra / KernelSU manager, then reboot so the early `initrc/` services are registered.

## CI artifacts and manual release

Every push runs the host test suite and builds the Android 11 AArch64 module. The workflow uploads `mtpadb-kernelsu.zip` and its SHA-256 file as the `mtpadb-kernelsu` Actions artifact, retained for 30 days. To publish a GitHub Release, run **Android 11 module CI** from Actions and provide a tag such as `v1.0.0`; the selected commit is built and the ZIP plus checksum are attached to the release.

## Provision RPC identity

The module never embeds or invents a shared RPC secret. Before `mtprpcd` can serve requests, provision these root-owned files through a trusted device provisioning path:

- `/data/adb/mtpadb/device.id`: exactly 16 raw bytes.
- `/data/adb/mtpadb/device.key`: exactly 32 raw bytes, mode `0600`.

The host must have the matching 32-byte key at `/etc/mtpadb/devices/<lowercase-hex-device-id>.key`. The module secures the containing directory as `root:root` mode `0700`; the daemon independently rejects files with the wrong owner, permissions, or length. If the files are not present when the module's late-start script runs, only the RPC daemon is left stopped. After provisioning them while Android is already running, start it with:

```sh
su -c 'setprop ctl.start mtpadb_ksu_rpcd'
```

## Pair and connect

Run the control client as root, substituting the phone's Wi-Fi address:

```sh
su -c '/data/adb/modules/mtpadb/bin/mtpadbctl wireless status'
su -c '/data/adb/modules/mtpadb/bin/mtpadbctl wireless pair-start --bind-address <phone-wifi-ip>'
```

Use the printed one-time code and pairing endpoint with the host's stock ADB client, then connect to the separate TLS endpoint:

```sh
adb pair <phone-wifi-ip>:<pairing-port>
adb connect <phone-wifi-ip>:<connect-port>
```

mDNS advertisement requires a usable `mdnsd` service and compatible AOSP runtime behavior. If it is unavailable, the control output still provides explicit IP/port values for manual pairing and connection. The module's `mtprpcd` service is `oneshot` and is started only when identity files are present; the AOSP-built service validates them again before accepting RPC.

Uninstalling the module stops its init services but intentionally preserves `/data/adb/mtpadb` and paired ADB peer keys. Remove those secrets explicitly if you want to revoke the device identity.
