---
name: android-usb-diagnostics
description: Diagnose physical Android USB negotiation, host permissions, and capture/replay transfer bottlenecks.
---

# Android USB diagnostics

Use the local diagnostic tool to inspect negotiated speed, host-port capability, and device-node access:

```bash
cmake --build build --target usbconnection
build/usbconnection
```

Reuse an up-to-date binary; for review-only tasks, inspect code and existing evidence without building or running it.

A 480 Mbps link on a SuperSpeed-capable host port can reflect the cable, hub, device capability, or negotiation. Verify
the device's capability and test a known suitable cable/direct port before assigning the cause. Do not infer sustained
transfer throughput from link speed.

For permission failures, inspect the reported device node and effective udev rules. On Debian/Ubuntu, check rules from
`android-udev-rules` before proposing a local rule. Scope any permission fix to the actual device; prefer session access
via `TAG+="uaccess"` or an appropriate group over world-writable USB access.

For transfer measurements, use the relevant existing helpers:

- `tests/lava_cli_save_buffer_test.py` launches the Android replay service, checks mapped/device-local buffer contents,
  and reports throughput. Inputs include `--serial`, `--apk`, and `--verbose`.
- `scripts/lava-replay-android.py run --service` starts a service;
  `tests/lava_cli_save_buffer_perf.py --external 127.0.0.1:12345 --sizes 16M,64M --csv android-perf.csv` benchmarks an
  existing forwarded service. Use each script's `--help` for current arguments.

Separate GPU readback, device socket transfer, and end-to-end transport when interpreting measurements. Record the
device, negotiated speed, transport, buffer sizes, and measured results; avoid treating past device-specific throughput
as a universal baseline.
