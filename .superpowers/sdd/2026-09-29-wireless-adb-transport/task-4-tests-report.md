## Task 4 test scaffold report

- Initial scaffold commits: `cda465fb1a7d797186de85e52a259292b7843fe8`, `323e41938c40577899422e088f44f3fd126370af`.
- The initial scaffold commits changed only `protocol/CMakeLists.txt` and `tests/adb/mtprpcd_socket_test.cpp`; this report was appended for the review fixes.
- Added a production-service socketpair test covering wire HELLO/AUTH, encrypted PING/PONG, echo including NUL, 1 MiB chunked roundtrip, wrong-PSK rejection, replay rejection, oversized declared length, 16-stream cap/slot release, and stalled-reader queue bound.
- RED evidence from controller: `cmake -S . -B .cache/build-task4 -DBUILD_TESTING=ON` configured successfully; `cmake --build .cache/build-task4 --target mtprpcd_socket_test` failed at the expected missing `mtprpcd/service.h` include before implementation.
- No implementation or test execution has occurred yet.

## Review fix round 1

- `write_all_until` now checks the deadline before every send attempt, including retries after short writes and `EINTR`.
- The oversized-header probe now starts from a valid sealed PING header and changes only its declared payload length. The client also rejects decoded response lengths above `kMaxPayloadBytes` before payload allocation; the shared decoder rejects oversized headers before payload reservation.
- The client now checks the HELLO device ID against the deterministic test device ID before constructing AUTH.
- No checks were run for this review fix.