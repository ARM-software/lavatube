---
name: android-usb-diagnostics
description: Diagnose physical Android device USB link speed, USB 2 vs. USB 3 negotiation, udev permissions, and cable/port bottlenecks
metadata:
  short-description: Diagnose Android USB speed and udev permissions
---

# Android USB Diagnostics

Use this skill when diagnosing physical Android device connection speeds, USB 2 vs USB 3 negotiation issues, trace capture/replay
transfer performance, or raw USB / udev permissions.

## Quick Connection Check

Build and run the USB diagnostic tool:
```bash
make -C build usbconnection
./build/usbconnection
```

## Interpreting Diagnostics

1. **Negotiated Speed vs. Host Port**:
  - If negotiated speed is **480 Mbps (USB 2.0)** but the host port supports **5000+ Mbps (USB 3)**:
    - The device is bottlenecked by the USB cable (e.g., standard phone in-box charging cable) or an intermediate USB 2.0 hub.
    - Swap to a certified USB 3.0+ / USB-C SuperSpeed cable.
2. **Device Hardware Capability**:
   - Modern Pixels (Pixel 8a, 9, 9a) support USB 3.2 Gen 1 (5 Gbps) and DisplayPort Alt Mode (`dwc3` controller in `adb shell dumpsys usb`).
3. **Host udev Rules**:
   - Check if the tool reports `CONFIGURED (Non-root user has Read/Write access)`.
   - On Debian/Ubuntu systems, distribution rules are installed in `/usr/lib/udev/rules.d/51-android.rules` (from package `android-udev-rules`).
   - If missing, create `/etc/udev/rules.d/51-android.rules` with:
     - `SUBSYSTEM=="usb", ATTR{idVendor}=="18d1", MODE="0666", TAG+="uaccess"`
     - and reload with `sudo udevadm control --reload-rules && sudo udevadm trigger`.

## High-Throughput Data Path (ADB vs Raw USB)

- **ADB Forwarding**: High CPU overhead due to double userspace proxying across loopback. Practical throughput caps at ~35-40 MB/s
  (USB 2) or ~80-140 MB/s (USB 3).
- **Raw USB / AOA Composite Mode**: Android supports composite mode `VID 0x18D1, PID 0x2D01` (Interface 0 = Raw Bulk Stream,
  Interface 1 = ADB). This enables high throughput (>350 MB/s) while keeping ADB active for control commands.
