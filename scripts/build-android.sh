#!/bin/bash
set -e

# Default values
ABIS=("arm64-v8a" "x86_64")
PLATFORM="android-35"
NDK_PATH=${ANDROID_NDK_HOME:-"/usr/lib/android-sdk/ndk/28.0.13004108"}

if [ -z "$ANDROID_NDK_HOME" ]; then
    echo "Warning: ANDROID_NDK_HOME is not set. Using default: $NDK_PATH"
    export ANDROID_NDK_HOME=$NDK_PATH
fi

if [ ! -d "$ANDROID_NDK_HOME" ]; then
    echo "Error: ANDROID_NDK_HOME directory not found at $ANDROID_NDK_HOME"
    exit 1
fi

# Allow overriding ABIs via command line
if [ $# -gt 0 ]; then
    ABIS=("$@")
fi

# Define script directory for relative path resolution
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

for ABI in "${ABIS[@]}"; do
    BUILD_DIR="build_android_${ABI}"
    echo "--- Building for Android (ABI: $ABI, Platform: $PLATFORM) ---"

    mkdir -p "$BUILD_DIR"
    pushd "$BUILD_DIR" > /dev/null

    cmake .. \
        -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchain/android.cmake \
        -DANDROID_ABI="$ABI" \
        -DANDROID_PLATFORM="$PLATFORM" \
        -DNO_XCB=ON

    make -j$(nproc)

    # Proactively start the emulator for x86 builds to avoid CTest discovery timeouts
    if [[ "$ABI" =~ ^x86 ]]; then
        echo "--- Starting emulator for $ABI tests ---"
        "$SCRIPT_DIR/../tests/emulator_setup.sh"
    fi

    popd > /dev/null
    echo "--- Android build complete for $ABI! Files located in $BUILD_DIR/ ---"
    done

