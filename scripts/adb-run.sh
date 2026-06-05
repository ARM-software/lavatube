#!/bin/bash
# scripts/adb-run.sh
# Standard wrapper for executing cross-compiled binaries on Android.
# Automatically invoked by CTest via CMAKE_CROSSCOMPILING_EMULATOR.

BINARY_PATH=$1
BINARY_NAME=$(basename $BINARY_PATH)

echo "--- Executing $BINARY_NAME on Android device ---"

show_emulator_errors()
{
    if [ -f /tmp/emulator.log ]; then
        echo "--- Recent emulator errors ---"
        tail -n +"$EMULATOR_LOG_START" /tmp/emulator.log | grep -iE "FATAL|ERROR|Unhandled Vulkan structure type|abort|crash" | tail -n 20 || true
        echo "--- End emulator errors ---"
    fi
}

# Determine architecture of the binary to decide if we should boot emulator
ARCH_HINT=""
if file "$BINARY_PATH" | grep -q "ARM aarch64"; then
    ARCH_HINT="aarch64"
elif file "$BINARY_PATH" | grep -q "x86-64"; then
    ARCH_HINT="x86_64"
fi

# Ensure device is ready with a timeout
if ! timeout 2 adb wait-for-device &> /dev/null; then
    if [ "$ARCH_HINT" == "x86_64" ]; then
        echo "No emulator detected. Attempting to start it..."
        SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
        if [ -f "$SCRIPT_DIR/../tests/emulator_setup.sh" ]; then
            "$SCRIPT_DIR/../tests/emulator_setup.sh"
        fi
        # Now wait longer since we just started it
        echo "Waiting for emulator to become responsive..."
        if ! timeout 180 adb wait-for-device; then
            echo "Error: Emulator failed to start in time."
            exit 1
        fi
    else
        echo "Error: No Android device found via ADB (timed out after 2s)."
        exit 1
    fi
fi

# Push binary to a writable location
echo "Pushing $BINARY_NAME to /data/local/tmp/..."
adb push "$BINARY_PATH" /data/local/tmp/ > /dev/null

# Always try to push libc++_shared.so if NDK is available
if [ -n "$ANDROID_NDK_HOME" ] && [ -n "$ARCH_HINT" ]; then
    # Refine search to avoid matching host path segments like 'linux-x86_64'
    LIBCXX=$(find "$ANDROID_NDK_HOME" -name libc++_shared.so | grep "$ARCH_HINT-linux-android" | head -n 1)
    if [ -n "$LIBCXX" ]; then
        echo "Pushing libc++_shared.so ($ARCH_HINT)..."
        adb push "$LIBCXX" /data/local/tmp/ > /dev/null
    fi
fi

# Execute binary on the device and capture its exit status
echo "Running /data/local/tmp/$BINARY_NAME..."
# We set LD_LIBRARY_PATH to /data/local/tmp so it finds libc++_shared.so
EMULATOR_LOG_START=1
if [ -f /tmp/emulator.log ]; then
    EMULATOR_LOG_START=$(($(wc -l < /tmp/emulator.log) + 1))
fi
adb shell "chmod +x /data/local/tmp/$BINARY_NAME && cd /data/local/tmp && LD_LIBRARY_PATH=. ./$BINARY_NAME ${@:2}"
EXIT_CODE=$?

if [ "$ARCH_HINT" == "x86_64" ]; then
    DEVICE_STATE=$(adb -e get-state 2>/dev/null || true)
    if [ "$DEVICE_STATE" != "device" ]; then
        if [ -z "$DEVICE_STATE" ]; then
            DEVICE_STATE="missing"
        fi
        echo "Error: Android emulator is no longer available after running $BINARY_NAME (adb state: $DEVICE_STATE)."
        show_emulator_errors
        if [ "$EXIT_CODE" -eq 0 ]; then
            EXIT_CODE=1
        fi
    fi
fi

echo "--- $BINARY_NAME exited with code $EXIT_CODE ---"
exit $EXIT_CODE
