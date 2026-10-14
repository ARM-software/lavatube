---
name: android-emulator-trace
description: Run or debug lavatube Vulkan captures on an Android emulator using tests/android_emulator_trace.sh.
---

# Android emulator traces

Use desktop tools from `build/` and an Android-built layer matching the emulator ABI. A desktop
`build/libVkLayer_lavatube.so` cannot be loaded on Android; a missing `libstdc++.so.6` in logcat commonly indicates that
mistake.

Check `adb devices` and `adb -e shell getprop ro.product.cpu.abi`. Typical layer paths are
`build_android_x86_64/libVkLayer_lavatube.so` and `build_android_arm64-v8a/libVkLayer_lavatube.so`. Check the build
configuration/dependencies as well as the ELF architecture; `file` output alone does not prove Android compatibility.

Build the needed desktop tools and matching Android layer when stale or absent. For an existing x86_64 setup, the
capture command from the repository root is:

```bash
tests/emulator_setup.sh
tests/android_emulator_trace.sh build_android_x86_64/libVkLayer_lavatube.so build/lava-replay VkLayer_lavatube.json
```

The trace script targets the emulator, changes its debug-layer settings and local calendar data, and replaces
`calendar.api` in the working directory. Use a disposable test emulator and preserve any existing output needed by the
user. Run `tests/emulator_cleanup.sh` when finished with an emulator you started for the task.

Verify the captured trace, not just script completion:

```bash
build/packtool print metadata.json calendar.api | rg 'VK_ANDROID_frame_boundary|VK_EXT_frame_boundary'
build/lava-print calendar.api | rg 'vkFrameBoundaryANDROID|vkCreateDevice'
```

For missing output or layer failures, inspect logcat for `VkLayer_lavatube`, `vulkan`, `dlopen`, and `lavatube`. If ADB
waits indefinitely, establish that an emulator has booted before retrying. Report capture/metadata verification
separately from replay validation.
