# High-speed data transfer over USB

This feature is meant for Android.

The usual TCP forwarding over adb data link we use to communicate with our
replayer is extremely slow and high overhead due to how it is designed in
Android itself.

One alternative is to use raw USB to transfer large amounts of data, while
keeping the main communications channel over TCP. This is easy to setup and
use on both Linux and Mac as hosts, but substantially more complex on
Windows. Since we do not really support Windows anyway, this is not a problem.

The biggest obstacle is that most USB cables people use are not actual USB 3,
but "high-speed" USB 2 which are really anything but high speed. We now have
a tool `usbconnection` to identify and debug setup and cable issues.

What can we do with a high-speed data transfer?

## Primary usage: Fetch replay artifacts

While debugging, we often want to fetch and inspect artifact from the replay
process by sending them to the controller. Such artifacts can be large (textures,
buffers, etc.), and it is not nice for users to wait a long time while they are
being transferred over.

## Possible usage idea: Transfer the trace file itself

Here the replayer starts in service mode, but with no trace file specified.
Instead, it will receive a trace file over a raw USB connection and then open it.

From controller side: `lava-cli init <trace file>`. Can be automated further from
a python launcher script to fire up the replayer in a waiting state, send the
trace file, and start the replay.

## Possible usage idea: Trace file streaming

We could make our `filereader` optionally request chunks over raw USB instead of
from the file system, allowing trace streaming directly from a controller. Useful
if the replay device has limited storage and we want to replay a big trace. And
could make Android replay snappier by not having to wait to transfer the whole
trace file first. The random access filereader would work the same way, but would
not be very efficient when run in such a streaming mode.

We would also have to virtualize the zip container entirely, so that requests for
smaller files within it are streamed over when requested instead of read from the
container file directly.

## Implementation notes

Implementation follows the "JNI Bootstrap → Raw File Descriptor" pattern in
Android Accessory Mode (AOA), where the phone acts as the USB peripheral connected
to the host PC.

The host must initiate the AOA switch by sending USB vendor control requests 51–53
via `libusb` (or `pyusb`).

### 1. JNI Bootstrap directly from ANativeActivity (No Java code required)

Because `lava-replay` on Android is structured around `ANativeActivity` (in
`src/replay_android.cpp`), we do not need to write any custom Java or Kotlin source
code. `app->activity->clazz` is an instance of `NativeActivity` (which inherits from
Android `Context`). We can execute the entire permission/accessory acquisition
sequence directly from C++ using JNI during replayer startup:

```cpp
static int open_usb_accessory_fd(android_app* app, JNIEnv* env)
{
	jclass activity_class = env->GetObjectClass(app->activity->clazz);
	jmethodID get_system_service = env->GetMethodID(activity_class, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");

	jstring service_name = env->NewStringUTF("usb");
	jobject usb_manager = env->CallObjectMethod(app->activity->clazz, get_system_service, service_name);
	env->DeleteLocalRef(service_name);
	env->DeleteLocalRef(activity_class);

	if (usb_manager == nullptr) return -1;

	jclass usb_manager_class = env->GetObjectClass(usb_manager);
	jmethodID get_accessory_list = env->GetMethodID(usb_manager_class, "getAccessoryList", "()[Landroid/hardware/usb/UsbAccessory;");
	jobjectArray accessory_list = (jobjectArray)env->CallObjectMethod(usb_manager, get_accessory_list);

	if (accessory_list == nullptr || env->GetArrayLength(accessory_list) == 0)
	{
		env->DeleteLocalRef(usb_manager_class);
		env->DeleteLocalRef(usb_manager);
		return -1;
	}

	jobject accessory = env->GetObjectArrayElement(accessory_list, 0);
	jmethodID open_accessory = env->GetMethodID(usb_manager_class, "openAccessory", "(Landroid/hardware/usb/UsbAccessory;)Landroid/os/ParcelFileDescriptor;");
	jobject pfd = env->CallObjectMethod(usb_manager, open_accessory, accessory);

	int raw_fd = -1;
	if (pfd != nullptr)
	{
		jclass pfd_class = env->GetObjectClass(pfd);
		jmethodID detach_fd = env->GetMethodID(pfd_class, "detachFd", "()I");
		raw_fd = env->CallIntMethod(pfd, detach_fd);
		env->DeleteLocalRef(pfd_class);
		env->DeleteLocalRef(pfd);
	}

	env->DeleteLocalRef(accessory);
	env->DeleteLocalRef(accessory_list);
	env->DeleteLocalRef(usb_manager_class);
	env->DeleteLocalRef(usb_manager);

	return raw_fd;
}
```

### 2. Native C++ handles all I/O directly

Once C++ receives the raw integer file descriptor `raw_fd`, the Android
runtime / Java VM is completely bypassed. You use standard POSIX system
calls (`read`, `write`, `poll`):

```cpp
#include <unistd.h>
#include <poll.h>

// Full-duplex I/O: Bulk IN (read) and Bulk OUT (write) run concurrently
void usb_read_worker(int fd)
{
	uint8_t buffer[64 * 1024]; // 64 KB chunks
	while (running)
	{
		ssize_t bytes_read = read(fd, buffer, sizeof(buffer));
		if (bytes_read > 0)
		{
			process_incoming_data(buffer, bytes_read);
		}
	}
}

void usb_send_artifact(int fd, const void* data, size_t size)
{
	const uint8_t* ptr = static_cast<const uint8_t*>(data);
	while (size > 0)
	{
		ssize_t written = write(fd, ptr, size);
		if (written <= 0) break;
		ptr += written;
		size -= written;
	}
}
```

Because AOA exposes concurrent Bulk IN and Bulk OUT endpoints, the link
is **full-duplex**: sending artifacts upstream to the host PC and receiving
commands/chunks downstream can occur simultaneously on separate threads without
any direction flipping.

### Controller setup

- Ensure your udev rule matches the vendor ID 18d1 so standard users have access to Interface 0.
- Running an adb command to grant USB permission: `adb cmd usb grant-accessory-permission <package> <accessory_name>`

### Manifest setup

```xml
    <uses-feature android:name="android.hardware.usb.accessory" />

    <activity ...>
        <intent-filter>
            <action android:name="android.hardware.usb.action.USB_ACCESSORY_ATTACHED" />
        </intent-filter>
        <meta-data android:name="android.hardware.usb.action.USB_ACCESSORY_ATTACHED"
                   android:resource="@xml/accessory_filter" />
    </activity>
```

### Limitations and gotchas

1. The Re-Enumeration Blip:
 - Calling Request 53 forcibly resets the USB interface on the phone.
 - If you request standard accessory mode (0x2D00), ADB will disconnect.
 - Solution: You must request Accessory + ADB composite mode (0x2D01), so the phone keeps the ADB endpoint
   active alongside your custom bulk accessory endpoints.
2. Loss of BSD Socket APIs:
 - It is not a network socket. There is no accept(), connect(), or port addressing.
 - You have exactly one bulk IN and one bulk OUT stream. If you need multiplexing (e.g., control commands vs.
   heavy data transfers), you must implement your own packet headers/framing.
3. Single App Lock:
 - Only one application on the device can hold the accessory file descriptor open at any given time.
4. Buffer Alignment Requirements:
 - To achieve maximum throughput, transfer sizes should be multiples of the USB packet size (512 bytes for USB
   2.0 High-Speed, 1024 bytes for USB 3.0 SuperSpeed), and large buffers (e.g., 64 KB – 1 MB) should be submitted
   as asynchronous URBs via libusb on the host.
5. Headless / Unattended Testing Is Tricky:
 - Because of the mandatory security dialog on stock Android, you cannot easily run an automated CI pipeline
   with freshly wiped devices without interacting with the screen once to click "Always allow".
