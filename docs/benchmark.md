# Benchmark policy

Correctness comes before performance. Stage A only exposes USB descriptors and has no ADB `CNXN`, shell, push/pull, or MTP-RPC data path. Consequently, shell RTT, file throughput, CPU cost of data transfers, MTP transactions per second, and queue depth are not measurable yet; this repository reports no performance numbers.

When an end-to-end transport exists, each report should record the exact host/device, kernel and `adb` versions, USB2/USB3 link, workload and payload sizes, repetitions, and the measured results for:

- shell round-trip time;
- `adb push` and `adb pull` throughput;
- CPU usage;
- MTP transactions per second;
- context switches and queue depth.

Do not assume expected speed or optimize based on intuition. Compare reproducible measurements before and after a change, and preserve correctness tests alongside performance results. USB2 and USB3 results must be reported separately when both are actually exercised.
