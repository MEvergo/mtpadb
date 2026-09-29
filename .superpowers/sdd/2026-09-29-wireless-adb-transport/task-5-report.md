## Task 5 implementation report

Commit: `6ffe884` (`Wire mtpadbd RPC service connector and AOSP routes`).

### Changed paths

- `android/mtpadbd/rpc_service.h` and `android/mtpadbd/rpc_service.cpp`: added the `mtpadb::mtpadbd` connector API. The path overload validates the filesystem socket path against `sun_path`, opens only an `AF_UNIX` `SOCK_STREAM` with close-on-exec, connects, and transfers successful-fd ownership to the caller; the no-argument overload uses `/dev/socket/mtprpcd-adb`.
- `android/mtpadbd/Android.bp`: added `libmtpadbd_rpc_service`, exported its header, restricted visibility to the two AOSP ADB packages, and allowed platform plus `com.android.adbd` variants.
- `protocol/CMakeLists.txt`: added the production connector source to `mtpadbd_rpc_connector_test` so it links the real helper.
- `android/mtp-patch/android-11/0001-mtpadbd-listener-disabled-target.patch` and `android/mtp-patch/modular-adb/0001-mtpadbd-listener-disabled-target.patch`: add a project-mode-gated real `daemon/services.cpp` route, link its connector dependency where the dispatcher is compiled, and register the real-dispatcher test only in a dedicated opt-in integration target.

### Branch integration

The dispatcher routes recognize exactly `mtpadb:rpc` only after the project-mode flag is enabled; with its default false value, `mtpadb:` names are rejected before stock shell/exec dispatch. The Android-only connector uses the filesystem socket path rather than reserved-namespace lookup, and all unrelated service dispatch remains on the existing AOSP paths.

### Verification status and limitation

No build, test, patch-preflight, lint, or formatter command was run in this agent, as requested; controller validation remains pending. A full Soong/AOSP build cannot be performed in the available workspace because the full AOSP build environment is absent. The existing connector and real-dispatcher tests remain the intended verification paths; the dispatcher test additionally requires the Task 4 `mtprpcd` init service and provisioned identity.

### Review remediation history

Commit `ba5eacb` separated the real dispatcher integration test from the stock `adbd_test` suite. Review then identified that the service dispatcher is compiled into shared AOSP code also used by stock `adbd`, and that reusing the stock test XML launches the wrong module.

### Final correction

Both branch patches now keep the RPC dispatcher disabled by default in shared service code. Only the `mtpadbd` entry path, under `MTPADB_PROJECT_DAEMON`, enables it immediately before `adbd_main()`. The actual dispatcher test resets the mode to stock/disabled, asserts `mtpadb:rpc` is rejected, then explicitly enables project mode and exercises the real connector through the live `mtprpcd` HELLO exchange. The fixture resets the mode after each test.

The test remains a separate opt-in `mtpadbd_rpc_service_integration_test`, outside stock test suites, and each branch patch adds `mtpadbd_rpc_service_integration_test.xml`. That config pushes and invokes the correctly named module and states the prepared-image requirements: root, running `mtprpcd`, and provisioned `/data/adb/mtpadb/device.id` plus `device.key`. Stock `adbd_test` retains only the connector-library link needed to resolve its compiled dispatcher; `libmtprpc_protocol` is linked only by the dedicated integration target.

The AOSP source contexts and cumulative hunk offsets were derived from the pinned Android 11 and modular source snapshots. No build, test, patch-preflight, lint, or formatter command was run for the initial correction. The later patch-application result is recorded below; Soong target/config discovery remains unverified, including custom-suite discovery and platform/APEX dependency resolution.

On a full prepared AOSP checkout, build with `source build/envsetup.sh && lunch aosp_arm64-eng && m mtpadbd_rpc_service_integration_test`, then run `atest mtpadbd_rpc_service_integration_test`. The config requires a prepared project image; it does not create identity secrets or start `mtprpcd`.
Final correction commit: `4cd5393` (`Gate mtpadbd RPC route to project daemon`).

### Patch application follow-up

Controller validation initially failed at Android 11 `adb/Android.bp:676` and `adb/daemon/services.cpp:50`, and modular `Android.bp:965`, `daemon/main.cpp:57`, and `daemon/services.cpp:52`. The modular main.cpp include context, hunk source line/count, and entry-context indentation were corrected; the remaining merged hunks received trailing unchanged source anchors. The modular Android.bp hunk retains the `],` source context with `@@ -965,7 +986,45 @@`.

Final application checks from the pinned repository roots passed:

- `git apply --check ../../android/mtp-patch/android-11/0001-mtpadbd-listener-disabled-target.patch` — Android 11 pinned source.
- `git apply --check ../../android/mtp-patch/modular-adb/0001-mtpadbd-listener-disabled-target.patch` — modular ADB pinned source.

No Soong build, tests, lint, or formatter was run; Soong target/config discovery remains unverified.
