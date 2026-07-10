Introduction
============

API tracer designed for multi-threaded replay with a minimum overhead and maximum portability
across different platforms. It is an experimental project that aims to explore options in API
tracing.

Requires Vulkan at least version 1.3.

Features
--------

* Fully multi-threaded design. See [Multithread design](doc/Multithreading.md) for more information.
* Focus on performance and generating stable, portable traces, sacrificing exact reproduction.
* Autogenerates nearly all its code with support for tracing nearly all functions and extensions.
  Replay support may however vary.
* Detects many unused features and removes erroneous enablement of them from the trace.
* Blackhole replay where no work is actually submitted to the GPU.
* Noscreen replay where we run any content without creating a window surface or displaying anything.
* Implements the experimental [Common Benchmark Standard](external/tracetooltests/doc/BenchmarkingStandard.md)
* Uses API usage analysis rather than a page guard to detect host-side changes (this was a mistake that
  needs to be undone).
* Aims to reproduce similar performance workload from capture to replay, not exactly identical behaviour.

Generally faster, uses less CPU resources and produces smaller trace files than gfxreconstruct.

Our goals and metrics can be found in [Goals.md](doc/Goals.md).

Performance
-----------

It has full multithreading support with a minimum of mutexes by using separate trace files for each
thread and lockless containers.

While tracing, each app thread will spawn two additional threads in the tracer. These are used to
asynchronously compress and save data to disk, so that the main thread never waits on these
operations.

While replaying, one additional thread will be spawned for each original thread in the app, for
asynchronously loading data while playing.

Portability
-----------

The goal is to be crossplatform 32/64 bit, linux/android, intel/arm and between all desktop and
mobile GPUs. How well portability works is however not well untested at the moment. Probably not at
all.

Tracing
=======

Make sure the "VkLayer_lavatube.json" is available in the loader search path. If it is not in a
default location you can set the `VK_LAYER_PATH` environment variable to point to its parent directory.

In addition, make sure the `libVkLayer_lavatube.so` file is the same folder as the `VkLayer_lavatube.json` manifest.

Then set the following environment variables at runtime:

```
export VK_LAYER_PATH=<path_to_json_and_.so>
export VK_INSTANCE_LAYERS=VK_LAYER_ARM_lavatube
export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:$VK_LAYER_PATH
```

You can also use the `lava-capture.py` script to set all these for you.

Building
========

For Ubuntu x86, install these packages:

```
sudo apt-get install git cmake pkg-config python3 libxcb1-dev libxrandr-dev libxcb-randr0-dev \
 libvulkan-dev spirv-headers ocl-icd-opencl-dev libgles-dev libegl-dev libglm-dev liblz4-dev libwayland-dev \
 libcurl4-gnutls-dev
```

Most of these are actually for compiling the tests, though.

To build for linux desktop:
--------------------------

```
git submodule update --init --recursive
mkdir build
cd build
cmake ..
make
```

Then in order to verify that everything is working correctly:

```
make test
```

To build for android:
--------------------

Fist install required dependencies for local Android building. These are the
recommended packages:

Ubuntu 24.04: `sudo apt-get install google-android-cmdline-tools-13.0-installer`
Ubuntu 25.10: `sudo apt-get install google-android-cmdline-tools-19.0-installer`

Then build by running

```
sudo apt-get install android-sdk-platform-tools
sudo sdkmanager "emulator" "ndk;28.2.13676358" "system-images;android-35;google_apis;x86_64"
export ANDROID_NDK_HOME="/usr/lib/android-sdk/ndk/28.2.13676358/"
scripts/build-android.sh
```

Linux cross-compile
-------------------

```
git submodule update --init
mkdir build_cross
cd build_cross
```

Then ONE of the following, for x86 32bit, ARMv7 or ARMv8, respectively:
```
cmake -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain/linux_x86_32.cmake ..
cmake -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain/linux_arm.cmake ..
cmake -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain/linux_arm64.cmake ..
```

Then complete as normal with:
```
make
```

If you are running Ubuntu, here are some tips on how to properly set up
a cross-compilation environment where you can install required packages:
https://askubuntu.com/questions/430705/how-to-use-apt-get-to-download-multi-arch-library

Debug
=====

To enable layer debugging, set `VK_LOADER_DEBUG=warning`.

To enable lavatube debug output, set `LAVATUBE_DEBUG` to one of 1, 2 or 3.

You can use `lava-cli` combined with `lava-replay --service` to step through a trace replay
while dumping out stored meta-information and replay state. See `lava-cli --help` for more
information.

You can also use the highly experimental `lava-tui` instead of `lava-cli` to have your own
natural language interface to traces. It can use any OpenAI compatible interface, such as
chatgpt with an API key, or Ollama. Example of how to run with Ollama and Gemma 4:
`LAVATUI_OPENAI_API_KEY=ollama LAVATUI_OPENAI_BASE_URL=http://localhost:11434/v1 LAVATUI_MODEL=gemma4:latest ./build/lava-tui`

Testing
=======

Lavatube has a comprehensive test suite that runs on both Linux Desktop and Android.

Linux Desktop
-------------

Run the standard CTest command in your build directory:

```bash
cd build
ctest -V
```

Android
-------

Lavatube supports running the entire test suite on an Android device or emulator. The testing behavior depends on the architecture (ABI) you are building for.

### Automated Emulator Testing (x86_64 / x86)

If you build for `x86_64` (default for emulators), CTest will automatically manage the emulator lifecycle for you using CTest Fixtures:

1.  **Boot**: It starts a headless emulator instance (`TestDevice`). *Note: The first test execution may take a few minutes as the emulator boots.*
2.  **Execution**: It pushes each test binary to `/data/local/tmp/` and executes it via ADB.
3.  **Teardown**: It shuts down the emulator when finished.

To run:
```bash
scripts/build-android.sh x86_64
cd build_android_x86_64
ctest -R container_test -V
```

### Physical Device Testing (arm64-v8a / armeabi-v7a)

For ARM builds, CTest assumes you have a physical device connected via USB with ADB enabled. It will **not** attempt to start an emulator.

1.  Connect your device via USB.
2.  Verify it's visible with `adb devices`.
3.  Run tests:

```bash
scripts/build-android.sh arm64-v8a
cd build_android_arm64-v8a
ctest -V
```

*Note: If no device is connected, ARM tests will fail quickly with a 10-second timeout.*

### Custom Android App Tracing Test

There is a specialized integration test `android_emulator_trace` that performs a full end-to-end capture and replay:
1.  Boots the emulator.
2.  Injects the Lavatube layer into a UI application (e.g., Google Calendar).
3.  Captures a trace for 15 seconds.
4.  Pulls the trace file back to your desktop.
5.  Runs `lava-replay` on the desktop to verify the trace is valid.

Files
=====

When tracing, the following files will be created in a separate directory:

  dictionary.json -- mapping of API call names to index values
  limits.json -- number of each type of data structured created during tracing
  metadata.json -- metadata from the traced platform
  thread_X.bin -- one file for each thread containing packets
  frames_X.json --- one JSON for each thread containing per-frame data

Replay security
===============

You can set the environment variable `LAVATUBE_SANDBOX_LEVEL` to set your desired
level of security paranoia. Levels are zero to three, from lowest to highest
security, where zero means the security sandbox is completely turned off. High
levels may interfere with other tools or layers. The security level may also be
modified with a command line option, but this cannot set the level to zero.

Also see our [SECURITY.md](SECURITY.md).

Tracing options
===============

`LAVATUBE_DESTINATION` can be used to set the name of the output trace file.

`LAVATUBE_DEDICATED_BUFFER` and `LAVATUBE_DEDICATED_IMAGE` can be used to override
or inject dedicate allocation hints to the application. If set to 1, all buffers
or images will have the preferred hint set. If set to 2, all buffers or images
will have the required hint set.

`LAVATUBE_DELAY_FENCE_SUCCESS_FRAMES` will delay the returned success of vkGetFenceStatus
and vkWaitForFences for the given number of frames to try to stagger the reuse of
content assets.

`LAVATUBE_GPU` lets you pick which GPU to use, by index. See vulkaninfo to see which
index value to use.

`LAVATUBE_CHUNK_SIZE` lets you choose the compression chunk size, by default it is 64mb.

`LAVATUBE_EXTERNAL_MEMORY` set it to 1 to experiment with replacing your GPU host memory
allocations with external memory allocations.

`LAVATUBE_VIRTUAL_QUEUES` if set to 1 will enable a virtualized memory system with only
one graphics queue family containing two queues. If the host system does not support
two queues, work for the second queue will be passed to the first queue. All other
queue families and queues will be hidden.

`LAVATUBE_TRUST_HOST_FLUSHING` can be set to 1 to disable active tracking of bindings,
and instead trust the application to flush all host memory before using it on the GPU
device.

Lavatube uses separate threads for both compression and writeout to disk with their
own queues, which may cause you to run out of memory. To disable this, you can set
the environment variables `LAVATUBE_DISABLE_MULTITHREADED_WRITEOUT` and
`LAVATUBE_DISABLE_MULTITHREADED_COMPRESS`.

Compression
===========

You can modify the compression algorithm and compression level used during tracing
with the environment variables `LAVATUBE_COMPRESSION_TYPE` and `LAVATUBE_COMPRESSION_LEVEL`.
The `scripts/lava-capture.py` helper also supports `--compression-type {LZ4,DENSITY,NONE}`.

The types are
0. Uncompressed
1. Density (default)
2. LZ4

For Density, the possible levels are (zero means use default, which is Cheetah)

1. Chameleon
2. Cheetah
3. Lion

For LZ4, higher levels means less compression and faster execution. Zero means
use the default, which is level one (best compression, worst performance). See LZ4
documentation for the exact meaning of this value.

For uncompressed traces, set `LAVATUBE_COMPRESSION_TYPE` to 0.

Further reading
===============

* [Vulkan memory management](doc/MemoryManagement.md)
* [Multithread design](doc/Multithreading.md)
