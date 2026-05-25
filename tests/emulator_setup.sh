#!/bin/bash
# tests/emulator_setup.sh
# Boots the Android emulator and waits for it to be ready.

set -e

AVD_NAME="TestDevice"

# Check if emulator is already running
if adb -e get-state &> /dev/null; then
    echo "Emulator is already running."
    exit 0
fi

# Ensure adb and emulator are in PATH
if ! command -v adb &> /dev/null || ! command -v emulator &> /dev/null; then
    echo "adb or emulator not found"
    exit 1
fi

# Check for AVD presence
if ! emulator -list-avds | grep -q "$AVD_NAME"; then
    echo "Creating AVD $AVD_NAME..."
    # Fallback image if not provided as argument
    IMAGE="system-images;android-33;google_apis;x86_64"
    if ! echo "no" | avdmanager create avd -n "$AVD_NAME" -k "$IMAGE" --force; then
        echo "Failed to create AVD"
        exit 1
    fi
fi

# Boot emulator in background
# -gpu host is critical for Vulkan
echo "Booting emulator $AVD_NAME in background..."
# Using setsid to start the emulator in a new session so it survives CTest setup test exit
setsid emulator -avd "$AVD_NAME" -gpu host -no-window -no-audio -no-snapshot-load > /tmp/emulator.log 2>&1 &

# Wait for boot completion
echo "Waiting for device to boot..."
MAX_WAIT=300
COUNT=0
# Loop until sys.boot_completed is 1
until [ "$(adb -e shell getprop sys.boot_completed | tr -d '\r')" == "1" ]; do
    sleep 5
    COUNT=$((COUNT + 5))
    if [ $COUNT -gt $MAX_WAIT ]; then
        echo "Timeout waiting for boot"
        exit 1
    fi
done

echo "Emulator is ready."
