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

fail()
{
    echo "ERROR: $*" >&2
    exit 1
}

show_trace_errors()
{
    echo "Relevant logcat lines:"
    adb -e logcat -d | grep -iE "lavatube|VkLayer_lavatube|vulkan|dlopen|debug layer|gpu_debug" || true
}

require_file()
{
    local path=$1
    local description=$2
    if [ ! -f "$path" ]; then
        fail "$description not found: $path"
    fi
}

validate_layer_for_device()
{
    require_file "$LAYER_PATH" "Layer library"
    require_file "$MANIFEST_PATH" "Layer manifest"

    local abi
    abi=$(adb -e shell getprop ro.product.cpu.abi 2>/dev/null | tr -d '\r')
    if [ -z "$abi" ]; then
        fail "Could not determine emulator ABI with adb. Is the emulator fully booted?"
    fi

    local layer_info
    layer_info=$(file -Lb "$LAYER_PATH")
    echo "Emulator ABI: $abi"
    echo "Layer file: $layer_info"

    if echo "$layer_info" | grep -q "GNU/Linux"; then
        fail "Layer appears to be a desktop Linux build. Use build_android_${abi}/libVkLayer_lavatube.so, not build/libVkLayer_lavatube.so."
    fi

    if ! echo "$layer_info" | grep -q "shared object"; then
        fail "Layer is not an ELF shared object: $LAYER_PATH"
    fi

    case "$abi" in
        x86_64)
            echo "$layer_info" | grep -q "x86-64" || fail "Emulator ABI is x86_64, but layer is not x86-64. Use build_android_x86_64/libVkLayer_lavatube.so."
            ;;
        x86)
            echo "$layer_info" | grep -q "Intel 80386" || fail "Emulator ABI is x86, but layer is not 32-bit x86."
            ;;
        arm64-v8a)
            echo "$layer_info" | grep -q "aarch64" || fail "Emulator ABI is arm64-v8a, but layer is not aarch64. Use build_android_arm64-v8a/libVkLayer_lavatube.so."
            ;;
        armeabi-v7a|armeabi)
            echo "$layer_info" | grep -q "ARM" || fail "Emulator ABI is $abi, but layer is not ARM."
            ;;
        *)
            echo "Warning: unrecognized emulator ABI '$abi'; only checking that layer is an Android-looking shared object."
            ;;
    esac
}

wait_for_emulator_device()
{
    if ! adb devices | awk '$1 ~ /^emulator-/ { found = 1 } END { exit found ? 0 : 1 }'; then
        fail "No emulator detected. Run tests/emulator_setup.sh first, or run this test via ctest so EmulatorFixture starts it."
    fi

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
validate_layer_for_device

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
TRACE_READY=0
for i in {1..30}; do
    if adb -e shell "[ -f '$TRACE_PATH' ]"; then
        TRACE_READY=1
        break
    fi
    sleep 1
done

if [ "$TRACE_READY" -ne 1 ]; then
    echo "Trace was not created at $TRACE_PATH"
    show_trace_errors
    exit 1
fi

# Pull trace
echo "Pulling trace from $TRACE_PATH..."
rm -rf calendar.vk
if ! adb -e pull "$TRACE_PATH" .; then
    echo "Failed to pull trace."
    show_trace_errors
    exit 1
fi

echo "Stopping $APP_NAME..."
adb -e shell am force-stop "$APP_NAME" || true

# Replay on desktop - skipping for now
#echo "Replaying trace on desktop..."
#"$REPLAY_PATH" -V -B calendar.vk

echo "Test successful!"
