# Android replayer

Lavatube provides a single-surface NativeActivity APK which runs the same replay
engine as the desktop and Android shell `lava-replay` executables. The APK is
intentionally separate from the shell executable so existing adb-driven CTests
continue to work.

## Build

Build one ABI at a time through the launcher:

```sh
scripts/lava-replay-android.py build --abi x86_64
scripts/lava-replay-android.py build --abi arm64-v8a
```

The APK contains `liblava-replay.so`, `android_native_app_glue`, and the shared
C++ runtime. It uses app-specific external storage and does not request broad
storage permissions.

## Run

The launcher detects the selected adb device, installs the matching APK, pushes
the trace, starts the NativeActivity, and pulls result and screenshot files:

```sh
scripts/lava-replay-android.py run traces/demo_computeraytracing.api -- --screenshots 0
```

Arguments after `--` are normal `lava-replay` arguments. The Android entry point
receives them through the intent `args` extra and uses the app external-files
directory as its working directory.

The script fetches any generated artifacts like screenshots and puts them into
the `artifacts` folder.

## Replay service

Start a paused replay and forward the service port to the host:

```sh
scripts/lava-replay-android.py run --service trace.api
build/lava-cli -H 127.0.0.1 -P 11901 status
build/lava-cli -H 127.0.0.1 -P 11901 info trace
build/lava-cli -H 127.0.0.1 -P 11901 step 0 calls 1
scripts/lava-replay-android.py stop
```

Android system-log collection is not yet implemented; the ordinary replay controls
and replay log commands remain available.
