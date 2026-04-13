#!/bin/bash
# scripts/adb-run.sh
# Standard wrapper for executing cross-compiled binaries on Android.
# Automatically invoked by CTest via CMAKE_CROSSCOMPILING_EMULATOR.

BINARY_PATH=$1
BINARY_NAME=$(basename $BINARY_PATH)

echo "--- Executing $BINARY_NAME on Android device ---"

# Ensure device is ready with a timeout to fail fast
if ! timeout 10 adb wait-for-device; then
    echo "Error: No Android device found via ADB (timed out after 10s)."
    exit 1
fi

# Push binary to a writable location
echo "Pushing $BINARY_NAME to /data/local/tmp/..."
adb push "$BINARY_PATH" /data/local/tmp/ > /dev/null

# Always try to push libc++_shared.so if NDK is available
if [ -n "$ANDROID_NDK_HOME" ]; then
    # Determine architecture of the binary
    ARCH_HINT=""
    if file "$BINARY_PATH" | grep -q "ARM aarch64"; then
        ARCH_HINT="aarch64"
    elif file "$BINARY_PATH" | grep -q "x86-64"; then
        ARCH_HINT="x86_64"
    fi

    if [ -n "$ARCH_HINT" ]; then
        # Refine search to avoid matching host path segments like 'linux-x86_64'
        LIBCXX=$(find "$ANDROID_NDK_HOME" -name libc++_shared.so | grep "$ARCH_HINT-linux-android" | head -n 1)
        if [ -n "$LIBCXX" ]; then
            echo "Pushing libc++_shared.so ($ARCH_HINT)..."
            adb push "$LIBCXX" /data/local/tmp/ > /dev/null
        fi
    fi
fi

# Execute binary on the device and capture its exit status
echo "Running /data/local/tmp/$BINARY_NAME..."
# We set LD_LIBRARY_PATH to /data/local/tmp so it finds libc++_shared.so
adb shell "chmod +x /data/local/tmp/$BINARY_NAME && cd /data/local/tmp && LD_LIBRARY_PATH=. ./$BINARY_NAME ${@:2}"
EXIT_CODE=$?

echo "--- $BINARY_NAME exited with code $EXIT_CODE ---"
exit $EXIT_CODE
