# MTPADB

MTPADB contains two related but independently bounded tracks:

- An opt-in Android AOSP `mtpadbd` target that reuses official wireless ADB pairing/TLS and exposes the private `mtpadb:rpc` service.
- A Linux Stage A configfs + FunctionFS gadget used to test ADB USB interface enumeration on `dummy_hcd`.

The Android source integration, protocol libraries, and Linux tests are present, but no full AOSP build or Android-device interoperability run has been performed in this workspace. The private RPC implementation currently supports authenticated `ping` and binary `echo`; it is not yet a general MTP command bridge. Physical USB-MTP behavior remains outside the wireless implementation.

## Android AOSP wireless ADB

Pins are limited to Android 11 `platform/system/core` tag `android-11.0.0_r48` and modular ADB commit `1cf2f017d312f73b3dc53bda85ef2610e35a80e9`. From a full AOSP checkout, make the current source available at `external/mtpadb` and apply the matching, revision-checked patch series:

```sh
export ANDROID_BUILD_TOP=/path/to/aosp
./android/deploy/prepare-aosp.sh android-11
# or: ./android/deploy/prepare-aosp.sh modular-adb
```

The helper symlinks this checkout into `external/mtpadb` (it does not copy sources), then invokes `android/mtp-patch/apply.sh`. Include the product fragment from the device product makefile:

```make
$(call inherit-product, external/mtpadb/android/deploy/mtpadb_product.mk)
```

Then build the Android daemon, local controller, RPC daemon, and host client from the AOSP tree:

```sh
source build/envsetup.sh
lunch aosp_arm64-eng
m mtpadbd mtprpcd mtpadbctl mtpadb
```

Before starting `mtprpcd`, the target must receive a 16-byte raw device ID and a 32-byte raw PSK at `/data/adb/mtpadb/device.id` and `/data/adb/mtpadb/device.key`; both files must be root-owned and mode `0600`. Provision them through the product's protected provisioning process, not source control or an ad-hoc `adb push`. The matching host key is `/etc/mtpadb/devices/<lowercase-hex-device-id>.key`, 32 bytes, owned by the invoking host user, and inaccessible to group/other users.

Wireless ADB is disabled until an explicit root command starts it. `mtpadbctl wireless pair-start --bind-address <device-wifi-ip>` prints separate dynamic pairing and TLS connection endpoints plus a one-time code. Use stock host `adb pair` with the pairing endpoint and `adb connect` with the connection endpoint. The pairing listener closes after a successful pairing or 120 seconds; the TLS connection listener stays open until `mtpadbctl wireless stop`. mDNS advertises `_adb-tls-pairing._tcp` and `_adb-tls-connect._tcp` only while wireless mode is enabled; if mDNS registration is unavailable, the displayed IP/ports remain usable manually.

After stock ADB has a paired TLS transport, the host-side AOSP target can use the private PSK-protected RPC layer:

```sh
mtpadb rpc --serial <wireless-serial> ping
mtpadb rpc --serial <wireless-serial> echo <input-file> > <output-file>
```

Standard ADB TLS authenticates the paired host and protects the ADB connection. MTPADB then performs a separate PSK `AUTH` and ChaCha20-Poly1305 session inside `mtpadb:rpc`; there is no additional MTPADB TCP port. `mtpadb` contacts only the local stock ADB server (default port 5037), not the device's pairing or TLS ports.

`mtpadbd` retains AOSP's normal privilege-drop path and fails closed if wireless startup does not run with effective UID 0. Do not weaken that path to make an unsupported user build or OEM/root-module port work. SELinux stays enforcing; the product fragment includes the project policy and package targets. See [Android integration](android/README.md), [security](docs/security.md), and [troubleshooting](docs/troubleshooting.md).

## Linux Stage A gadget

Stage A advertises the ADB interface class/subclass/protocol `0xff/0x42/0x01` through FunctionFS, but does not implement ADB `CNXN`, authentication, shell, sync, or MTP RPC. Enumeration is not an online ADB transport. The gadget refuses to bind any UDC other than `dummy_hcd` and does not create a TCP ADB transport.

### Build and unit tests

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

Requirements include CMake 3.20+, a C++20 compiler, OpenSSL development files, and Linux kernel support for configfs, FunctionFS, `dummy_hcd`, and `libcomposite`.

### Run Stage A

Review `linux/host.conf.example` and choose VID/PID values assigned for your test use; do not impersonate a real vendor. On a test Linux host:

```sh
sudo modprobe dummy_hcd
sudo modprobe libcomposite
sudo build/linux/mtpadb-gadgetd run --config linux/host.conf.example
```

The daemon stays in the foreground and cleans its project-owned configfs gadget and FunctionFS mount on SIGINT/SIGTERM. In another terminal, inspect enumeration with `lsusb` and `adb devices -l`; the device is expected to remain offline because Stage A does not send `CNXN`.

To remove stale project resources after an interrupted run:

```sh
sudo build/linux/mtpadb-gadgetd cleanup
```

The privileged integration check uses an isolated ADB server on port 5038 and requires `sudo`:

```sh
sudo tests/integration/stage-a-adb-usb.sh
```

It never connects to or stops the default ADB server. If port 5038 is occupied, set `MTPADB_TEST_ADB_PORT` to another free port.

## systemd

For a conventional `/usr` installation, configure paths during CMake setup; do not relocate the build with `cmake --install --prefix`:

```sh
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_SYSCONFDIR=/etc
sudo cmake --install build
if [ ! -e /etc/mtpadb/host.conf ]; then
    sudo install -Dm0640 /etc/mtpadb/host.conf.example /etc/mtpadb/host.conf
fi
sudoedit /etc/mtpadb/host.conf
sudo systemctl daemon-reload
sudo systemctl start mtpadb-gadgetd.service
```

The service is installed under `/usr/lib/systemd/system`; enable it at boot only if persistent Stage A startup is intended.

## Design notes

- [Architecture](docs/architecture.md)
- [Linux host setup](docs/linux-host.md)
- [Protocol boundary](docs/protocol.md)
- [Android porting and AOSP pins](docs/android-porting.md)
- [Security boundary](docs/security.md)
- [Troubleshooting](docs/troubleshooting.md)
- [Benchmark policy](docs/benchmark.md)
- [Third-party license notes](LICENSES/README.md)
- [Upstream source notes](docs/upstream-notes.md)
