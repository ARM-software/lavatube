#!/bin/bash
# tests/android_emulator_trace.sh <layer_path> <replay_path> <manifest_path>
# Assumes emulator is already running (e.g. via EmulatorFixture)

set -e

LAYER_PATH=$1
REPLAY_PATH=$2
MANIFEST_PATH=$3

if [ -z "$LAYER_PATH" ] || [ -z "$REPLAY_PATH" ] || [ -z "$MANIFEST_PATH" ]; then
    echo "Usage: $0 <layer_path> <replay_path> <manifest_path>"
    exit 1
fi

# Ensure device is ready
echo "Waiting for device to be ready..."
adb -e wait-for-device

# Push layer and manifest
echo "Pushing layer to /data/local/tmp/..."
adb -e push "$LAYER_PATH" /data/local/tmp/libVkLayer_lavatube.so
adb -e push "$MANIFEST_PATH" /data/local/tmp/VkLayer_lavatube.json

# Enable layer globally for the app
APP_NAME="com.google.android.calendar"
adb -e shell settings put global enable_gpu_debug_layers 1
adb -e shell settings put global gpu_debug_app "$APP_NAME"
adb -e shell settings put global gpu_debug_layers VK_LAYER_ARM_lavatube
adb -e shell settings put global gpu_debug_layer_path /data/local/tmp

# Force SkiaVk to ensure Vulkan is used for UI
adb -e shell setprop debug.hwui.renderer skiavk
# Set destination for lavatube
TRACE_PATH="/sdcard/Download/calendar.vk"
adb -e shell "mkdir -p /sdcard/Download && chmod 777 /sdcard/Download"
adb -e shell setprop debug.vulkan.lavatube.destination "$TRACE_PATH"

# Launch app
echo "Launching $APP_NAME..."
if ! adb -e shell monkey -p "$APP_NAME" -c android.intent.category.LAUNCHER 1; then
    echo "Failed to launch $APP_NAME, trying fallback (com.android.settings)..."
    APP_NAME="com.android.settings"
    adb -e shell settings put global gpu_debug_app "$APP_NAME"
    adb -e shell monkey -p "$APP_NAME" -c android.intent.category.LAUNCHER 1
fi

# Wait for trace to be generated
echo "Tracing for 15 seconds..."
sleep 15

# Stop app
echo "Stopping $APP_NAME..."
adb -e shell am force-stop "$APP_NAME"

# Pull trace
echo "Pulling trace from $TRACE_PATH..."
rm -rf calendar.vk
if ! adb -e pull "$TRACE_PATH" .; then
    echo "Failed to pull trace. Checking logcat for errors..."
    adb -e logcat -d | grep -i "lavatube" || true
    exit 1
fi

# Replay on desktop
echo "Replaying trace on desktop..."
"$REPLAY_PATH" -V -B calendar.vk

echo "Test successful!"
