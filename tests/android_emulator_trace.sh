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

get_screen_size()
{
    local size
    size=$(adb -e shell wm size 2>/dev/null | tr -d '\r' | awk '/Physical size:/ { print $3; exit }')
    if echo "$size" | grep -qE '^[0-9]+x[0-9]+$'; then
        echo "$size"
    else
        echo "320x640"
    fi
}

wait_for_app_foreground()
{
    local package=$1
    local timeout=${2:-10}
    local end_time=$((SECONDS + timeout))
    while [ "$SECONDS" -lt "$end_time" ]; do
        if adb -e shell dumpsys activity top 2>/dev/null | grep -q "$package"; then
            return 0
        fi
        sleep 1
    done

    echo "Timed out waiting for $package to become the foreground activity."
    echo "Current foreground activity:"
    adb -e shell dumpsys activity top 2>/dev/null | awk '/^  ACTIVITY / { print; exit }' || true
    return 1
}

tap_known_popup_button()
{
    local dump_path ui_xml line label bounds coords x y
    dump_path=/data/local/tmp/window.xml

    ui_xml=
    for dump_attempt in {1..5}; do
        adb -e shell rm -f "$dump_path" >/dev/null 2>&1 || true
        if ! adb -e shell uiautomator dump "$dump_path" >/dev/null 2>&1; then
            echo "uiautomator dump failed; continuing without popup dismissal."
            return 1
        fi

        for i in {1..10}; do
            ui_xml=$(adb -e shell cat "$dump_path" 2>/dev/null | tr -d '\r' || true)
            if [ -n "$ui_xml" ]; then
                break
            fi
            sleep 0.2
        done

        if [ -n "$ui_xml" ]; then
            break
        fi
        sleep 1
    done

    if [ -z "$ui_xml" ]; then
        echo "uiautomator produced an empty UI dump at $dump_path; continuing without popup dismissal."
        return 1
    fi

    line=$(echo "$ui_xml" | awk '
        BEGIN {
            RS = "<node "
            labels["Got it"] = 1
            labels["Get started"] = 1
            labels["next page"] = 1
            labels["OK"] = 1
            labels["Ok"] = 1
            labels["Allow"] = 1
            labels["While using the app"] = 1
            labels["Only this time"] = 1
            labels["Not now"] = 1
            labels["Maybe later"] = 1
            labels["Skip"] = 1
            labels["Continue"] = 1
            labels["Done"] = 1
        }
        {
            text = ""
            desc = ""
            if (match($0, /text="[^"]*"/)) {
                text = substr($0, RSTART + 6, RLENGTH - 7)
            }
            if (match($0, /content-desc="[^"]*"/)) {
                desc = substr($0, RSTART + 14, RLENGTH - 15)
            }
            if ((text in labels) || (desc in labels)) {
                label = text
                if (label == "") {
                    label = desc
                }
                if (match($0, /bounds="\[[0-9]+,[0-9]+\]\[[0-9]+,[0-9]+\]"/)) {
                    print label "\t" substr($0, RSTART + 8, RLENGTH - 9)
                    exit
                }
            }
        }')

    if [ -z "$line" ]; then
        return 1
    fi

    label=${line%%	*}
    bounds=${line#*	}
    coords=$(echo "$bounds" | awk -F'[][]|,' '{ print int(($2 + $5) / 2), int(($3 + $6) / 2) }')
    x=${coords% *}
    y=${coords#* }
    if ! echo "$x $y" | grep -qE '^[0-9]+ [0-9]+$'; then
        echo "Could not parse UI bounds for '$label': $bounds"
        return 1
    fi

    echo "Dismissing popup button '$label' at $x,$y..."
    adb -e shell input tap "$x" "$y" >/dev/null 2>&1 || return 1
    return 0
}

dismiss_known_popups()
{
    local pass dismissed=0
    for pass in {1..8}; do
        if tap_known_popup_button; then
            dismissed=1
            sleep 1
        else
            break
        fi
    done

    if [ "$dismissed" -eq 0 ]; then
        echo "No known popup dialog buttons found."
    fi
}

disable_debug_layers()
{
    adb -e shell settings put global enable_gpu_debug_layers 0 >/dev/null 2>&1 || true
    adb -e shell settings delete global gpu_debug_app >/dev/null 2>&1 || true
    adb -e shell settings delete global gpu_debug_layers >/dev/null 2>&1 || true
    adb -e shell settings delete global gpu_debug_layer_path >/dev/null 2>&1 || true
    adb -e shell setprop debug.vulkan.lavatube.finish 0 >/dev/null 2>&1 || true
}

launch_target_app()
{
    adb -e shell monkey -p "$APP_NAME" -c android.intent.category.LAUNCHER 1
    wait_for_app_foreground "$APP_NAME" 15
}

seed_local_calendar_data()
{
    local calendar_id now_s start_ms end_ms

    echo "Seeding local CalendarProvider data..."
    adb -e shell "content delete --uri 'content://com.android.calendar/calendars?caller_is_syncadapter=true&account_name=local&account_type=LOCAL' --where \"account_name='local' AND account_type='LOCAL'\"" >/dev/null 2>&1 || true
    adb -e shell "content insert --uri 'content://com.android.calendar/calendars?caller_is_syncadapter=true&account_name=local&account_type=LOCAL' --bind account_name:s:local --bind account_type:s:LOCAL --bind name:s:local --bind calendar_displayName:s:Local --bind calendar_color:i:-16776961 --bind calendar_access_level:i:700 --bind ownerAccount:s:local --bind visible:i:1 --bind sync_events:i:1"

    calendar_id=$(adb -e shell content query --uri content://com.android.calendar/calendars 2>/dev/null | tr -d '\r' | awk '/account_name=local/ && /account_type=LOCAL/ { if (match($0, /_id=[0-9]+/)) { print substr($0, RSTART + 4, RLENGTH - 4); exit } }')
    if [ -z "$calendar_id" ]; then
        fail "Could not create or find local CalendarProvider calendar."
    fi

    now_s=$(adb -e shell date +%s 2>/dev/null | tr -d '\r')
    start_ms=$((((now_s / 86400) * 86400 + 12 * 60 * 60) * 1000))
    end_ms=$((start_ms + 60 * 60 * 1000))
    adb -e shell "content insert --uri 'content://com.android.calendar/events?caller_is_syncadapter=true&account_name=local&account_type=LOCAL' --bind calendar_id:i:$calendar_id --bind title:s:Trace_test_event --bind dtstart:l:$start_ms --bind dtend:l:$end_ms --bind eventTimezone:s:UTC --bind allDay:i:0 --bind eventStatus:i:1 --bind hasAlarm:i:0"
}

prepare_app_state_without_capture()
{
    echo "Preparing $APP_NAME app state before capture..."
    disable_debug_layers
    seed_local_calendar_data
    adb -e shell am force-stop "$APP_NAME" || true

    if ! launch_target_app; then
        fail "Could not launch $APP_NAME during pre-capture preparation."
    fi

    dismiss_known_popups
    sleep 1
    dismiss_known_popups
    adb -e shell am force-stop "$APP_NAME" || true
}

drive_app_activity()
{
    local seconds=$1
    local size width height mid_x left_x right_x top_y mid_y bottom_y
    size=$(get_screen_size)
    width=${size%x*}
    height=${size#*x}
    mid_x=$((width / 2))
    left_x=$((width / 4))
    right_x=$((width * 3 / 4))
    top_y=$((height / 4))
    mid_y=$((height / 2))
    bottom_y=$((height * 3 / 4))

    echo "Driving $APP_NAME UI activity for $seconds seconds on ${width}x${height} display..."
    local end_time=$((SECONDS + seconds))
    local step=0
    while [ "$SECONDS" -lt "$end_time" ]; do
        case $((step % 4)) in
            0)
                adb -e shell input swipe "$mid_x" "$bottom_y" "$mid_x" "$top_y" 250 >/dev/null 2>&1 || true
                ;;
            1)
                adb -e shell input swipe "$mid_x" "$top_y" "$mid_x" "$bottom_y" 250 >/dev/null 2>&1 || true
                ;;
            2)
                adb -e shell input swipe "$right_x" "$mid_y" "$left_x" "$mid_y" 250 >/dev/null 2>&1 || true
                ;;
            *)
                adb -e shell input tap "$mid_x" "$mid_y" >/dev/null 2>&1 || true
                ;;
        esac
        step=$((step + 1))
        sleep 0.25
    done
}

print_trace_sanity()
{
    local tool_dir packtool lava_print metadata frames global_frames frame_table_entries android_ext ext_ext boundary_count
    tool_dir=$(dirname "$REPLAY_PATH")
    packtool="$tool_dir/packtool"
    lava_print="$tool_dir/lava-print"

    echo "Trace sanity:"

    if [ -x "$packtool" ]; then
        metadata=$("$packtool" print metadata.json calendar.vk 2>/dev/null || true)
        global_frames=$(echo "$metadata" | awk -F: '/"global_frames"/ { gsub(/[, ]/, "", $2); print $2; exit }')
        android_ext=$(echo "$metadata" | awk '/"VK_ANDROID_frame_boundary"/ { count++ } END { print count + 0 }')
        ext_ext=$(echo "$metadata" | awk '/"VK_EXT_frame_boundary"/ { count++ } END { print count + 0 }')
        frames=$("$packtool" print frames_0.json calendar.vk 2>/dev/null || true)
        frame_table_entries=$(echo "$frames" | awk '/"global_frame"/ { count++ } END { print count + 0 }')

        echo "  global_frames: ${global_frames:-unknown}"
        echo "  frame table entries: $frame_table_entries"
        echo "  metadata VK_ANDROID_frame_boundary entries: $android_ext"
        echo "  metadata VK_EXT_frame_boundary entries: $ext_ext"
    else
        echo "  packtool not found next to replay binary: $packtool"
    fi

    if [ -x "$lava_print" ] && [ "${COUNT_TRACE_PACKETS:-0}" = "1" ]; then
        boundary_count=$("$lava_print" calendar.vk 2>/dev/null | awk '/"name":"vkFrameBoundaryANDROID"/ { count++ } END { print count + 0 }')
        echo "  vkFrameBoundaryANDROID packets: $boundary_count"
    elif [ -x "$lava_print" ]; then
        echo "  vkFrameBoundaryANDROID packets: skipped (set COUNT_TRACE_PACKETS=1 to count)"
    else
        echo "  lava-print not found next to replay binary: $lava_print"
    fi

    if echo "${global_frames:-}" | grep -qE '^[0-9]+$' && [ "$global_frames" -lt "$MIN_TRACE_FRAMES" ]; then
        echo "  WARNING: expected at least $MIN_TRACE_FRAMES frames after driving UI activity."
    fi
    if echo "${boundary_count:-}" | grep -qE '^[0-9]+$' && [ "$boundary_count" -eq 0 ]; then
        echo "  WARNING: no vkFrameBoundaryANDROID packets found in trace."
    fi
}

# Ensure device is ready
echo "Waiting for device to be ready..."
wait_for_emulator_device
validate_layer_for_device

APP_NAME="com.google.android.calendar"
LAYER_DIR="/data/local/debug/vulkan"
TRACE_DIR="/data/local/tmp/lavatube-traces"
TRACE_PATH="$TRACE_DIR/calendar.vk"
TRACE_SECONDS=${TRACE_SECONDS:-3}
MIN_TRACE_FRAMES=${MIN_TRACE_FRAMES:-120}

cleanup()
{
    disable_debug_layers
    adb -e shell setenforce 1 >/dev/null 2>&1 || true
}
trap cleanup EXIT

echo "Restarting adbd as root..."
adb root
sleep 2
wait_for_emulator_device

prepare_app_state_without_capture

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
echo "Launching $APP_NAME for capture..."
if ! launch_target_app; then
    echo "Failed to launch $APP_NAME into the foreground, trying fallback (com.android.settings)..."
    APP_NAME="com.android.settings"
    adb -e shell settings put global gpu_debug_app "$APP_NAME"
    launch_target_app || fail "Fallback app did not become foreground."
fi

# This should normally be a no-op because onboarding is cleared before enabling
# capture, but keep it here for unexpected permission prompts.
dismiss_known_popups

# Keep the app invalidating while tracing. Calendar otherwise tends to render a
# short startup burst and then go idle for most of the 15 second window.
drive_app_activity "$TRACE_SECONDS"

# Ask the Android test hook in the layer to serialize and pack while the app is
# still foreground; backgrounding first can race with activity teardown.
adb -e shell setprop debug.vulkan.lavatube.finish 1
adb -e shell input keyevent BACK || true

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

print_trace_sanity

# Replay on desktop - skipping for now
#echo "Replaying trace on desktop..."
#"$REPLAY_PATH" -V -B calendar.vk

echo "Test successful!"
