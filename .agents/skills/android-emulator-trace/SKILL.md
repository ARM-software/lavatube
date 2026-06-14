---
name: android-emulator-trace
description: Run or debug lavatube's Android emulator capture workflow, especially tests/android_emulator_trace.sh, emulator setup, Android layer ABI selection, and verifying android traces.
metadata:
  short-description: Run lavatube Android emulator traces correctly
---

# Android Emulator Trace

Use this skill when working with `tests/android_emulator_trace.sh`, Android UI captures, Android debug Vulkan layers, or emulator trace failures.

## Core Rule

Use desktop tools from `build/`, but push an Android-built Vulkan layer from `build_android_<ABI>/`.

Do not push `build/libVkLayer_lavatube.so` to Android. It is a desktop Linux shared object and will fail in logcat with a missing `libstdc++.so.6` or another host-library dependency.

## Typical x86_64 Emulator Flow

1. Build desktop tools if needed:
   `make -C build -j6`
2. Build the Android emulator layer:
   `make -C build_android_x86_64 -j6`
3. Start the emulator:
   `tests/emulator_setup.sh`
4. Run the trace:
   `tests/android_emulator_trace.sh build_android_x86_64/libVkLayer_lavatube.so build/lava-replay VkLayer_lavatube.json`
5. Stop the emulator when done:
   `tests/emulator_cleanup.sh`

## Checks Before Running

- `adb devices` should list one emulator.
- `adb -e shell getprop ro.product.cpu.abi` should match the layer:
  - `x86_64` -> `build_android_x86_64/libVkLayer_lavatube.so`
  - `arm64-v8a` -> `build_android_arm64-v8a/libVkLayer_lavatube.so`
- `file <layer>` should say `SYSV`, not `GNU/Linux`.

## Trace Verification

After a successful script run, inspect the trace you captured from the repo root (here assuming that the trace
is named `calendar.vk`):

```bash
build/packtool print metadata.json calendar.vk | rg "VK_ANDROID_frame_boundary|VK_EXT_frame_boundary"
build/lava-print calendar.vk | rg "vkFrameBoundaryANDROID|vkCreateDevice"
```

## Common Failures

- `adb -e wait-for-device` hangs: no emulator is running. Run `tests/emulator_setup.sh` first.
- `dlopen failed: library "libstdc++.so.6" not found`: a desktop `build/libVkLayer_lavatube.so` was pushed; rebuild/use `build_android_<ABI>/libVkLayer_lavatube.so`.
- Trace path missing after launch: check logcat for `VkLayer_lavatube`, `vulkan`, `dlopen`, and `lavatube`.
