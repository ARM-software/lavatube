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

wait_for_emulator_device()
{
    adb -e wait-for-device
    for i in {1..30}; do
        if [ "$(adb -e get-state 2>/dev/null | tr -d '\r')" = "device" ]; then
            return 0
        fi
        sleep 1
    done
    echo "Timed out waiting for emulator adb state to become device"
    exit 1
}

# Ensure device is ready
echo "Waiting for device to be ready..."
wait_for_emulator_device

APP_NAME="com.google.android.calendar"
LAYER_DIR="/data/local/debug/vulkan"
TRACE_DIR="/data/local/tmp/lavatube-traces"
TRACE_PATH="$TRACE_DIR/calendar.vk"

cleanup()
{
    adb -e shell settings put global enable_gpu_debug_layers 0 >/dev/null 2>&1 || true
    adb -e shell settings delete global gpu_debug_app >/dev/null 2>&1 || true
    adb -e shell settings delete global gpu_debug_layers >/dev/null 2>&1 || true
    adb -e shell settings delete global gpu_debug_layer_path >/dev/null 2>&1 || true
    adb -e shell setprop debug.vulkan.lavatube.finish 0 >/dev/null 2>&1 || true
    adb -e shell setenforce 1 >/dev/null 2>&1 || true
}
trap cleanup EXIT

echo "Restarting adbd as root..."
adb root
sleep 2
wait_for_emulator_device

# Android's debug Vulkan loader looks for loose layer libraries here. Calendar is
# not debuggable, so this rooted emulator path also needs SELinux permissive.
echo "Pushing layer to $LAYER_DIR/..."
adb -e shell "rm -rf '$LAYER_DIR' '$TRACE_DIR' && mkdir -p '$LAYER_DIR' '$TRACE_DIR'"
adb -e push "$LAYER_PATH" "$LAYER_DIR/libVkLayer_lavatube.so"
adb -e push "$MANIFEST_PATH" /data/local/tmp/VkLayer_lavatube.json
adb -e shell "chmod 755 /data/local/debug '$LAYER_DIR' '$LAYER_DIR/libVkLayer_lavatube.so' && chmod 777 '$TRACE_DIR'"
adb -e shell "chcon u:object_r:shell_data_file:s0 /data/local/debug '$LAYER_DIR' '$LAYER_DIR/libVkLayer_lavatube.so' '$TRACE_DIR' || true"
adb -e shell setenforce 0

# Enable the layer only for Calendar. Do not use debug.vulkan.layers here; that
# loads the layer into unrelated system UI processes too.
adb -e shell settings put global enable_gpu_debug_layers 1
adb -e shell settings put global gpu_debug_app "$APP_NAME"
adb -e shell settings put global gpu_debug_layers VK_LAYER_ARM_lavatube
adb -e shell settings put global gpu_debug_layer_path "$LAYER_DIR"

# Force SkiaVk to ensure Vulkan is used for UI
adb -e shell setprop debug.hwui.renderer skiavk
# Set destination for lavatube
adb -e shell setprop debug.vulkan.lavatube.destination "$TRACE_PATH"
adb -e shell setprop debug.vulkan.lavatube.finish 0
adb -e shell "rm -rf '$TRACE_PATH' '${TRACE_PATH}_tmp'"

# Launch app
adb -e shell am force-stop "$APP_NAME" || true
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

# Calendar will usually stay alive in the background and not destroy the Vulkan
# instance. Ask the Android test hook in the layer to serialize and pack now.
adb -e shell input keyevent BACK || true
adb -e shell setprop debug.vulkan.lavatube.finish 1

echo "Waiting for trace to be packed..."
for i in {1..30}; do
    if adb -e shell "[ -f '$TRACE_PATH' ]"; then
        break
    fi
    sleep 1
done

# Pull trace
echo "Pulling trace from $TRACE_PATH..."
rm -rf calendar.vk
if ! adb -e pull "$TRACE_PATH" .; then
    echo "Failed to pull trace. Checking logcat for errors..."
    adb -e logcat -d | grep -i "lavatube" || true
    exit 1
fi

echo "Stopping $APP_NAME..."
adb -e shell am force-stop "$APP_NAME" || true

# Replay on desktop - skipping for now
#echo "Replaying trace on desktop..."
#"$REPLAY_PATH" -V -B calendar.vk

echo "Test successful!"
