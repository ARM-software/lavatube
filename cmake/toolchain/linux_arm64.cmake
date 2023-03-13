set(CMAKE_SYSTEM_NAME Linux CACHE STRING "System Name")
set(CMAKE_SYSTEM_PROCESSOR aarch64 CACHE STRING "System Processor")
set(CMAKE_C_COMPILER "aarch64-linux-gnu-gcc")
set(CMAKE_CXX_COMPILER "aarch64-linux-gnu-g++")
set(CMAKE_C_COMPILER_ID "GNU")
set(CMAKE_CXX_COMPILER_ID "GNU")
set(CMAKE_AR ar CACHE STRING "ar")
set(CMAKE_CXX_COMPILER_WORKS TRUE)
set(CMAKE_C_COMPILER_WORKS TRUE)
set(NO_XCB TRUE)
set(WINDOWSYSTEM "fbdev") # for tracetooltests...
set(NO_TRACETOOLTESTS TRUE) # but usually do not want this to make cross-compiles easier
set(TBB_TEST FALSE)
set(CMAKE_SYSROOT_COMPILE /usr/aarch64-linux-gnu)
