# Android cross-compilation toolchain wrapper
# Usage: cmake -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain/android.cmake ..

if(NOT DEFINED ENV{ANDROID_NDK_HOME})
    message(FATAL_ERROR "Please set ANDROID_NDK_HOME environment variable to your NDK installation path.")
endif()

# Project-specific defaults for Android
set(NO_XCB TRUE CACHE BOOL "Disable XCB for Android" FORCE)
set(NO_TRACETOOLTESTS TRUE CACHE BOOL "Skip tracetooltests for Android cross-compile" FORCE)

# Default Android ABI and Platform (can be overridden on command line)
if(NOT DEFINED ANDROID_ABI)
    set(ANDROID_ABI "arm64-v8a" CACHE STRING "Android ABI")
endif()

if(NOT DEFINED ANDROID_PLATFORM)
    set(ANDROID_PLATFORM "android-26" CACHE STRING "Android Platform")
endif()

# Include the official NDK toolchain
include($ENV{ANDROID_NDK_HOME}/build/cmake/android.toolchain.cmake)
