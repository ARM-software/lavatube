Introduction
============

Vulkan tracer designed for multi-threaded replay with a minimum overhead and maximum portability
across different platforms. It is an experimental project that aims to explore Vulkan tracing
options.

Features
--------

* Fully multi-threaded design. See [Multithread design](doc/Multithreading.md) for more information.
* Focus on performance and generating stable, portable traces, sacrificing precise reproduction.
* Autogenerates nearly all its code with support for tracing nearly all functions and extensions.
  Replay support may however vary.
* Detects many unused features and removes erroneous enablement of them from the trace.
* Blackhole replay where no work is actually submitted to the GPU.
* Noscreen replay where we run any content without creating a window surface or displaying anything.
* Implements the experimental [Common Benchmark Standard](external/tracetooltests/doc/BenchmarkingStandard.md)

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
default location you can set the VK_LAYER_PATH environment variable to point to its parent directory.

In addition, make sure the libVkLayer_lavatube.so file is the same folder as the VkLayer_lavatube.json manifest.

Then set the following environment variables at runtime:

export VK_LAYER_PATH=<path_to_json_and_.so>
export VK_INSTANCE_LAYERS=VK_LAYER_ARM_lavatube
export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:$VK_LAYER_PATH

Building
========

For Ubuntu x86, install these packages:

	sudo apt-get install git cmake pkg-config python3 libxcb1-dev libxrandr-dev libxcb-randr0-dev libtbb-dev \
	     libvulkan-dev spirv-headers ocl-icd-opencl-dev libgles-dev libegl-dev libglm-dev

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

(To be done.)

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

If you don't have Intel TBB install for your platform, you can build and install it like
this (example for aarch64):

```
git clone https://github.com/oneapi-src/oneTBB.git tbb
cd tbb
mkdir build_arm
cd build_arm
cmake -DCMAKE_INSTALL_PREFIX=/usr/aarch64-linux-gnu -DCMAKE_TOOLCHAIN_FILE=<PATH TO LAVATUBE>/cmake/toolchain/linux_arm64.cmake ..
make
sudo make install
```

Debug
=====

To enable layer debugging, set VK_LOADER_DEBUG=warning

To enable lavatube debug output, set LAVATUBE_DEBUG to one of 1, 2 or 3.

Files
=====

When tracing, the following files will be created in a separate directory:

  dictionary.json -- mapping of API call names to index values
  limits.json -- number of each type of data structured created during tracing
  metadata.json -- metadata from the traced platform
  thread_X.vk -- one file for each thread containing API calls
  frames_X.json --- one JSON for each thread containing per-frame data

Tracing options
===============

LAVATUBE_DESTINATION can be used to set the name of the output trace file.

LAVATUBE_DEDICATED_BUFFER and LAVATUBE_DEDICATED_IMAGE can be used to override
or inject dedicate allocation hints to the application. If set to 1, all buffers
or images will have the preferred hint set. If set to 2, all buffers or images
will have the required hint set.

LAVATUBE_DELAY_FENCE_SUCCESS_FRAMES will delay the returned success of vkGetFenceStatus
and vkWaitForFences for the given number of frames to try to stagger the reuse of
content assets.

LAVATUBE_GPU lets you pick which GPU to use, by index. See vulkaninfo to see which
index value to use.

LAVATUBE_CHUNK_SIZE lets you choose the compression chunk size, by default it is 64mb.

LAVATUBE_EXTERNAL_MEMORY set it to 1 to experiment with replacing your GPU host memory
allocations with external memory allocations.

LAVATUBE_VIRTUAL_QUEUES if set to 1 will enable a virtualized memory system with only
one graphics queue family containing two queues. If the host system does not support
two queues, work for the second queue will be passed to the first queue. All other
queue families and queues will be hidden.

Lavatube uses separate threads for both compression and writeout to disk with their
own queues, which may cause you to run out of memory. To disable this, you can set
the environment variables LAVATUBE_DISABLE_MULTITHREADED_WRITEOUT and
LAVATUBE_DISABLE_MULTITHREADED_COMPRESS.

Further reading
===============

* [Vulkan memory management](doc/MemoryManagement.md)
* [Multithread design](doc/Multithreading.md)
