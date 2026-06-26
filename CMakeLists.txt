# Running with sanitizers:
# Create a _new_ build directory and run cmake with the -DSANITIZE=<option> command line option. The various options that should work are: address, thread, undefined
# You may need to run "export ASAN_OPTIONS=detect_leaks=0" before running any of the tests with the address sanitizer.

# This needs to go before the project() call below, since it can be set by toolchain files
set(NO_XCB FALSE CACHE BOOL "Do not compile support for XCB and Xlib")
set(NO_TRACETOOLTESTS FALSE CACHE BOOL "Do not compile tracetooltests")
set(GPU_MODEL CACHE STRING "environment variables for your GPU model here") # if you have multiple, separate them with semi-colon

find_program(CCACHE_PROGRAM ccache)
if(NOT CCACHE_PROGRAM)
    message(STATUS "No ccache found")
endif()
if(CCACHE_PROGRAM AND DEFINED ENV{CCACHE_DIR})
    set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE "${CCACHE_PROGRAM}")
    message(STATUS "We will build using ccache -- very good!")
endif()

cmake_minimum_required(VERSION 3.22)
project(lavatube VERSION 0.0.1 DESCRIPTION "Vulkan tracer designed for multi-threaded replay with minimum overhead" HOMEPAGE_URL "https://github.com/ARM-software/lavatube" LANGUAGES C CXX)
enable_testing()

if(CMAKE_SYSTEM_NAME STREQUAL "Android")
    set(CMAKE_CROSSCOMPILING_EMULATOR "${CMAKE_SOURCE_DIR}/scripts/adb-run.sh" CACHE STRING "Android test wrapper")

    if(ANDROID_ABI MATCHES "^x86")
        add_test(NAME emulator_setup COMMAND "${CMAKE_SOURCE_DIR}/tests/emulator_setup.sh")
        set_tests_properties(emulator_setup PROPERTIES FIXTURES_SETUP EmulatorFixture)

        add_test(NAME emulator_cleanup COMMAND "${CMAKE_SOURCE_DIR}/tests/emulator_cleanup.sh")
        set_tests_properties(emulator_cleanup PROPERTIES FIXTURES_CLEANUP EmulatorFixture)
    endif()

    # Automatically add EmulatorFixture requirement to all tests added so far and in the future
    define_property(DIRECTORY PROPERTY ANDROID_TESTS_LIST BRIEF_DOCS "List of tests" FULL_DOCS "List of tests")
    set_property(DIRECTORY APPEND PROPERTY ANDROID_TESTS_LIST "")
endif()

function(add_lavatube_test name)
    add_test(NAME ${name} ${ARGN})
    if(CMAKE_SYSTEM_NAME STREQUAL "Android" AND ANDROID_ABI MATCHES "^x86")
        set_property(TEST ${name} APPEND PROPERTY FIXTURES_REQUIRED EmulatorFixture)
    endif()
endfunction()
IF(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  SET(CMAKE_INSTALL_PREFIX "/opt/lavatube" CACHE PATH "install prefix" FORCE)
ENDIF(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)

if (NOT DEFINED CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 20)
endif()
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_CTEST_ARGUMENTS "--output-on-failure")

set(SRC_ROOT ${PROJECT_SOURCE_DIR})
set(THIRD_PARTY_DIR ${SRC_ROOT}/external)
set(PYTHON_EXECUTABLE python3)
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${PROJECT_SOURCE_DIR}/cmake")
set(LAYER_NAME VkLayer_lavatube)
find_package(PkgConfig REQUIRED)

if(NOT ${NO_XCB})
    find_package(XCB REQUIRED)
    find_package(X11 REQUIRED)
    find_package(WAYLAND REQUIRED)
    set(XCB_LIBRARIES ${XCB_LIBRARIES} ${X11_LIBRARIES} ${WAYLAND_LIBRARY})
    message(STATUS "Wayland, XCB and X11 enabled, using ${XCB_LIBRARIES} ${X11_LIBRARIES} ${WAYLAND_LIBRARY}")
    set(XCB_ENABLE -DVK_USE_PLATFORM_XCB_KHR -DVK_USE_PLATFORM_XLIB_KHR -DVK_USE_PLATFORM_WAYLAND_KHR)
elseif(ANDROID)
    set(XCB_ENABLE -DVK_USE_PLATFORM_ANDROID_KHR)
    set(ANDROID_LIBRARIES android log)
    message(STATUS "Android build enabled")
else()
    set(XCB_ENABLE "")
    message(STATUS "XCB, X11 and Wayland support not added")
endif()

if(NOT EXISTS "${PROJECT_SOURCE_DIR}/external/tracetooltests/external/Vulkan-Headers/.git")
    message(SEND_ERROR "git submodules not available. Please run: git submodule update --init --recursive")
endif()

if(SANITIZE)
    if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang")
        set(SANITIZER -fsanitize=${SANITIZE} -fstack-protector-all -fsanitize-undefined-trap-on-error)
    else()
        set(SANITIZER -fsanitize=${SANITIZE} -fstack-protector-all -fsanitize-undefined-trap-on-error -fuse-ld=gold)
    endif()
endif()

# --- spirv-simulator ---

message(STATUS "Configuring spirv-simulator")
add_subdirectory(external/spirv-simulator ${CMAKE_BINARY_DIR}/spirv-simulator)

# --- zipcontainer ---

message(STATUS "Configuring zipcontainer")
add_subdirectory(external/zipcontainer ${CMAKE_BINARY_DIR}/zipcontainer)

# --- ftxui ---

if(NOT ANDROID)
    message(STATUS "Configuring ftxui")
    add_subdirectory(external/ftxui ${CMAKE_BINARY_DIR}/ftxui)
    find_package(CURL REQUIRED)
endif()

# --- tracetooltests ---

if(NOT ${NO_TRACETOOLTESTS})
    message(STATUS "Configuring tracetooltests")
    set(NO_GLES 1)
    set(NO_CL 1)
    set(ENV{TOOLSTEST_SANITY} 1)
    add_subdirectory(external/tracetooltests ${CMAKE_BINARY_DIR}/tracetooltests)
else()
    message(STATUS "Asked to skip tracetooltests")
endif()

# --- compression ---

message(STATUS "Configuring density")
add_subdirectory(external/density ${CMAKE_BINARY_DIR}/density)
message(STATUS "Configuring lz4")
find_package(LZ4)
if (NOT LZ4_FOUND)
    message(STATUS "LZ4 not found, using FetchContent to build it from source")
    include(FetchContent)
    FetchContent_Declare(
        lz4
        GIT_REPOSITORY https://github.com/lz4/lz4.git
        GIT_TAG v1.9.4
        SOURCE_SUBDIR build/cmake
    )
    set(LZ4_BUILD_CLI OFF CACHE BOOL "Build lz4 CLI" FORCE)
    set(LZ4_BUILD_SHARED OFF CACHE BOOL "Build lz4 shared library" FORCE)
    FetchContent_MakeAvailable(lz4)
    if (NOT TARGET LZ4::LZ4)
        if (TARGET lz4_static)
            add_library(LZ4::LZ4 ALIAS lz4_static)
        elseif (TARGET lz4_shared)
            add_library(LZ4::LZ4 ALIAS lz4_shared)
        endif()
    endif()
endif()

# --- unordered_dense ---

add_subdirectory(external/unordered_dense)

# --- common ---

# Debug Release RelWithDebInfo MinSizeRel
set(COMMON_FLAGS ${SANITIZER} -Werror -Wall -fvisibility=hidden ${XCB_ENABLE} -Wshadow)
if(CMAKE_BUILD_TYPE STREQUAL "Debug" OR NOT CMAKE_BUILD_TYPE)
	set(COMMON_FLAGS ${COMMON_FLAGS} -ggdb -O0 -fno-omit-frame-pointer -DDEBUG -DRELTYPE="debug")
	message(STATUS "Debug build")
elseif(CMAKE_BUILD_TYPE STREQUAL "Release")
	set(COMMON_FLAGS ${COMMON_FLAGS} -ggdb -O3 -fno-strict-aliasing -DNDEBUG -DRELTYPE="release")
	message(STATUS "Release build")
else()
	set(COMMON_FLAGS ${COMMON_FLAGS} -DRELTYPE="custom")
	message(STATUS "Custom build -- set your own flags!")
endif()
if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang")
	set(COMMON_FLAGS ${COMMON_FLAGS} -Wthread-safety -Wno-error=thread-safety-analysis -Wno-error=unused-but-set-variable)
endif()
if(ANDROID)
    set(MOST_COMMON_LIBRARIES ${SANITIZER} dl ${ANDROID_LIBRARIES})
else()
    set(MOST_COMMON_LIBRARIES ${SANITIZER} stdc++ dl pthread ${ANDROID_LIBRARIES})
endif()
set(COMMON_LIBRARIES ${MOST_COMMON_LIBRARIES} unordered_dense::unordered_dense atomic density LZ4::LZ4 ${XCB_LIBRARIES} spirv_simulator_lib zipcontainer)

set(COMMON_INCLUDE
    PUBLIC ${SRC_ROOT}
    PUBLIC ${SRC_ROOT}/src
    PUBLIC ${SRC_ROOT}/include
    PUBLIC ${SRC_ROOT}/generated
    PUBLIC ${SRC_ROOT}/external
    PUBLIC ${SRC_ROOT}/external/tracetooltests/include
    PUBLIC ${SRC_ROOT}/external/tracetooltests/external/Vulkan-Headers/include
    PUBLIC ${SRC_ROOT}/external/tracetooltests/external/SPIRV-Headers/include
    PRIVATE ${SRC_ROOT}/external/jsoncpp
    PRIVATE ${SRC_ROOT}/external/zipcontainer
)

set(VULKAN_TESTS_SRC
   ${PROJECT_SOURCE_DIR}/tests/tests.h
   ${PROJECT_SOURCE_DIR}/tests/common.cpp
   ${PROJECT_SOURCE_DIR}/tests/common.h
)

set(COMMON_SRC
    ${PROJECT_SOURCE_DIR}/external/jsoncpp/src/lib_json/json_writer.cpp
    ${PROJECT_SOURCE_DIR}/external/jsoncpp/src/lib_json/json_reader.cpp
    ${PROJECT_SOURCE_DIR}/external/jsoncpp/src/lib_json/json_value.cpp
    ${PROJECT_SOURCE_DIR}/external/tracetooltests/src/usagetracker/vulkan_feature_detect.cpp
    ${PROJECT_SOURCE_DIR}/external/tracetooltests/src/usagetracker/vulkan_feature_detect.h
    ${PROJECT_SOURCE_DIR}/src/vk_wrapper.cpp
    ${PROJECT_SOURCE_DIR}/src/util.cpp
    ${PROJECT_SOURCE_DIR}/src/util.h
    ${PROJECT_SOURCE_DIR}/src/vkjson.cpp
    ${PROJECT_SOURCE_DIR}/src/vkjson.h
    ${PROJECT_SOURCE_DIR}/src/android_utils.cpp
    ${PROJECT_SOURCE_DIR}/src/packfile.cpp
    ${PROJECT_SOURCE_DIR}/src/packfile.h
    ${PROJECT_SOURCE_DIR}/src/lavamutex.h
    ${PROJECT_SOURCE_DIR}/src/containers.h
    ${PROJECT_SOURCE_DIR}/src/sandbox.cpp
    ${PROJECT_SOURCE_DIR}/src/sandbox.h
    ${PROJECT_SOURCE_DIR}/src/json_helpers.cpp
    ${PROJECT_SOURCE_DIR}/src/json_helpers.h
    ${PROJECT_SOURCE_DIR}/src/markings.cpp
    ${PROJECT_SOURCE_DIR}/src/markings.h

    ${PROJECT_SOURCE_DIR}/src/filewriter.cpp
    ${PROJECT_SOURCE_DIR}/src/filewriter.h
    ${PROJECT_SOURCE_DIR}/src/write.cpp
    ${PROJECT_SOURCE_DIR}/src/write.h
    ${PROJECT_SOURCE_DIR}/src/rangetracking.h
    ${PROJECT_SOURCE_DIR}/src/helpers_write.cpp
    ${PROJECT_SOURCE_DIR}/src/helpers_write.h

    ${PROJECT_SOURCE_DIR}/generated/vk_wrapper_auto.cpp
    ${PROJECT_SOURCE_DIR}/generated/vk_wrapper_auto.h
    ${PROJECT_SOURCE_DIR}/generated/util_auto.cpp
    ${PROJECT_SOURCE_DIR}/generated/util_auto.h
    ${PROJECT_SOURCE_DIR}/generated/write_auto.cpp
    ${PROJECT_SOURCE_DIR}/generated/write_auto.h
    ${PROJECT_SOURCE_DIR}/generated/write_resource_auto.cpp
    ${PROJECT_SOURCE_DIR}/generated/write_resource_auto.h
    ${PROJECT_SOURCE_DIR}/generated/tostring.cpp
    ${PROJECT_SOURCE_DIR}/generated/tostring.h
    ${PROJECT_SOURCE_DIR}/generated/struct_write_auto.cpp
    ${PROJECT_SOURCE_DIR}/generated/struct_write_auto.h
)

set(LAVATUBE_SRC
    ${PROJECT_SOURCE_DIR}/src/window.cpp
    ${PROJECT_SOURCE_DIR}/src/window.h
    ${PROJECT_SOURCE_DIR}/src/filereader.cpp
    ${PROJECT_SOURCE_DIR}/src/filereader.h
    ${PROJECT_SOURCE_DIR}/src/read.cpp
    ${PROJECT_SOURCE_DIR}/src/read.h
    ${PROJECT_SOURCE_DIR}/src/replay_screenshot.cpp
    ${PROJECT_SOURCE_DIR}/src/replay_screenshot.h
    ${PROJECT_SOURCE_DIR}/src/allocators.cpp
    ${PROJECT_SOURCE_DIR}/src/allocators.h
    ${PROJECT_SOURCE_DIR}/src/suballocator.cpp
    ${PROJECT_SOURCE_DIR}/src/suballocator.h
    ${PROJECT_SOURCE_DIR}/src/memory.cpp
    ${PROJECT_SOURCE_DIR}/src/memory.h
    ${PROJECT_SOURCE_DIR}/src/pipeline_executable_stats.cpp
    ${PROJECT_SOURCE_DIR}/src/pipeline_executable_stats.h

    ${PROJECT_SOURCE_DIR}/src/postprocess.cpp
    ${PROJECT_SOURCE_DIR}/src/postprocess.h
    ${PROJECT_SOURCE_DIR}/src/execute_commands.cpp
    ${PROJECT_SOURCE_DIR}/src/execute_commands.h
    ${PROJECT_SOURCE_DIR}/src/helpers_read.cpp
    ${PROJECT_SOURCE_DIR}/src/helpers_read.h

    ${PROJECT_SOURCE_DIR}/generated/read_auto.cpp
    ${PROJECT_SOURCE_DIR}/generated/read_auto.h
    ${PROJECT_SOURCE_DIR}/generated/struct_read_auto.cpp
    ${PROJECT_SOURCE_DIR}/generated/struct_read_auto.h
    ${PROJECT_SOURCE_DIR}/generated/struct_print_auto.cpp
    ${PROJECT_SOURCE_DIR}/generated/struct_print_auto.h
)

add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/.autogenerated
    BYPRODUCTS ${SRC_ROOT}/generated/struct_read_auto.cpp ${SRC_ROOT}/generated/struct_write_auto.cpp ${SRC_ROOT}/generated/struct_print_auto.cpp ${SRC_ROOT}/generated/struct_read_auto.h ${SRC_ROOT}/generated/struct_write_auto.h ${SRC_ROOT}/generated/struct_print_auto.h
           ${SRC_ROOT}/generated/read_auto.cpp ${SRC_ROOT}/generated/write_auto.cpp ${SRC_ROOT}/generated/read_auto.h ${SRC_ROOT}/generated/write_auto.h ${SRC_ROOT}/generated/util_auto.cpp
           ${SRC_ROOT}/generated/util_auto.h ${SRC_ROOT}/generated/vk_wrapper_auto.h ${SRC_ROOT}/generated/vk_wrapper_auto.cpp
           ${SRC_ROOT}/generated/write_resource_auto.cpp ${SRC_ROOT}/generated/write_resource_auto.h ${SRC_ROOT}/generated/tostring.cpp ${SRC_ROOT}/generated/tostring.h
    COMMAND rm -f scripts/spec.pyc # fix checkouts broken when this file's source moved
    COMMAND mkdir -p generated
    COMMAND ${PYTHON_EXECUTABLE} ${SRC_ROOT}/scripts/struct.py
    COMMAND ${PYTHON_EXECUTABLE} ${SRC_ROOT}/scripts/lava.py
    COMMAND ${PYTHON_EXECUTABLE} ${SRC_ROOT}/scripts/vk_wrapper.py
    COMMAND ${PYTHON_EXECUTABLE} ${SRC_ROOT}/scripts/tostring.py
    COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_BINARY_DIR}/.autogenerated
    WORKING_DIRECTORY ${SRC_ROOT}
    DEPENDS ${SRC_ROOT}/scripts/lava.py ${SRC_ROOT}/external/tracetooltests/external/Vulkan-Headers/registry/vk.xml ${SRC_ROOT}/external/tracetooltests/scripts/spec.py ${SRC_ROOT}/scripts/struct.py
	${SRC_ROOT}/src/hardcode_read.cpp ${SRC_ROOT}/src/hardcode_write.cpp ${SRC_ROOT}/scripts/util.py ${SRC_ROOT}/scripts/vk_wrapper.py ${SRC_ROOT}/scripts/vkconfig.py
)

# The below is necessary to inform cmake that we should not run the above once per target
add_custom_target(sync_generated DEPENDS ${CMAKE_BINARY_DIR}/.autogenerated)

add_library(lavatube STATIC ${COMMON_SRC} ${LAVATUBE_SRC})
target_include_directories(lavatube ${COMMON_INCLUDE})
target_link_libraries(lavatube ${COMMON_LIBRARIES})
target_compile_options(lavatube PRIVATE ${COMMON_FLAGS} -fPIC)
add_dependencies(lavatube sync_generated)

# --- tracer layer ---

add_library(VkLayer_lavatube SHARED ${PROJECT_SOURCE_DIR}/src/vulkan_layer.cpp ${COMMON_SRC})
target_include_directories(VkLayer_lavatube ${COMMON_INCLUDE})
target_link_libraries(VkLayer_lavatube ${COMMON_LIBRARIES})
target_compile_options(VkLayer_lavatube PRIVATE ${COMMON_FLAGS})
target_compile_definitions(VkLayer_lavatube PUBLIC COMPILE_LAYER)
add_custom_command(TARGET VkLayer_lavatube POST_BUILD COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/implicit_layer.d)
add_custom_command(TARGET VkLayer_lavatube POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:VkLayer_lavatube> ${CMAKE_BINARY_DIR}/implicit_layer.d/libVkLayer_lavatube.so)
add_custom_command(TARGET VkLayer_lavatube POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SRC_ROOT}/VkLayer_lavatube.json ${CMAKE_BINARY_DIR}/implicit_layer.d/VkLayer_lavatube.json)
add_dependencies(VkLayer_lavatube sync_generated)
install(FILES ${CMAKE_BINARY_DIR}/implicit_layer.d/VkLayer_lavatube.json ${CMAKE_BINARY_DIR}/implicit_layer.d/libVkLayer_lavatube.so DESTINATION implicit_layer.d)
install(PROGRAMS scripts/lava-capture.py TYPE BIN)

# --- unit tests ---

add_executable(mprotect tests/mprotect.cpp src/util.cpp src/util.h src/android_utils.cpp)
target_include_directories(mprotect ${COMMON_INCLUDE})
target_link_libraries(mprotect ${MOST_COMMON_LIBRARIES})
target_compile_options(mprotect PRIVATE ${COMMON_FLAGS})
add_dependencies(mprotect sync_generated)
add_lavatube_test(mprotect_test COMMAND mprotect)

#add_executable(userfaultfd tests/userfaultfd.cpp src/util.cpp src/util.h)
#target_include_directories(userfaultfd ${COMMON_INCLUDE})
#target_link_libraries(userfaultfd ${MOST_COMMON_LIBRARIES} pthread)
#target_compile_options(userfaultfd PRIVATE ${COMMON_FLAGS})

add_executable(containers tests/container_test.cpp)
target_include_directories(containers ${COMMON_INCLUDE})
target_link_libraries(containers ${MOST_COMMON_LIBRARIES} unordered_dense::unordered_dense)
target_compile_options(containers PRIVATE ${COMMON_FLAGS})
add_lavatube_test(container_test COMMAND containers)
add_dependencies(containers sync_generated)

add_executable(containers2 tests/container_test2.cpp)
target_include_directories(containers2 ${COMMON_INCLUDE})
target_link_libraries(containers2 ${MOST_COMMON_LIBRARIES} unordered_dense::unordered_dense)
target_compile_options(containers2 PRIVATE ${COMMON_FLAGS})
add_lavatube_test(container_test2 COMMAND containers2)
add_dependencies(containers sync_generated)

add_executable(host_write_regions_test tests/host_write_regions.cpp)
target_include_directories(host_write_regions_test ${COMMON_INCLUDE})
target_link_libraries(host_write_regions_test ${MOST_COMMON_LIBRARIES} unordered_dense::unordered_dense)
target_compile_options(host_write_regions_test PRIVATE ${COMMON_FLAGS})
add_lavatube_test(host_write_regions_test COMMAND host_write_regions_test)
add_dependencies(host_write_regions_test sync_generated)

add_executable(command_execution_test tests/command_execution_test.cpp)
target_include_directories(command_execution_test ${COMMON_INCLUDE})
target_link_libraries(command_execution_test ${COMMON_LIBRARIES} lavatube)
target_compile_options(command_execution_test PRIVATE ${COMMON_FLAGS})
add_lavatube_test(command_execution_test COMMAND command_execution_test)
add_dependencies(command_execution_test sync_generated)

add_executable(rangetrack tests/rangetrack.cpp)
target_include_directories(rangetrack ${COMMON_INCLUDE})
target_link_libraries(rangetrack ${MOST_COMMON_LIBRARIES})
target_compile_options(rangetrack PRIVATE ${COMMON_FLAGS})
add_lavatube_test(rangetrack_test COMMAND rangetrack)
add_dependencies(rangetrack sync_generated)

add_executable(rangetrack_perf tests/rangetrack_perf.cpp)
target_include_directories(rangetrack_perf ${COMMON_INCLUDE})
target_link_libraries(rangetrack_perf ${MOST_COMMON_LIBRARIES})
target_compile_options(rangetrack_perf PRIVATE ${COMMON_FLAGS})
add_dependencies(rangetrack_perf sync_generated)

add_executable(replay_screenshot_test tests/replay_screenshot.cpp)
target_include_directories(replay_screenshot_test ${COMMON_INCLUDE})
target_link_libraries(replay_screenshot_test ${COMMON_LIBRARIES} lavatube)
target_compile_options(replay_screenshot_test PRIVATE ${COMMON_FLAGS})
add_lavatube_test(replay_screenshot_test COMMAND replay_screenshot_test)
add_dependencies(replay_screenshot_test sync_generated)

add_executable(check_png tests/check_png.cpp)
target_include_directories(check_png ${COMMON_INCLUDE})
target_link_libraries(check_png ${MOST_COMMON_LIBRARIES})
target_compile_options(check_png PRIVATE ${COMMON_FLAGS})
add_dependencies(check_png sync_generated)

function(internal_test test_name test_output)
	add_executable(${ARGV0} tests/${ARGV0}.cpp ${VULKAN_TESTS_SRC})
	target_include_directories(${ARGV0} ${COMMON_INCLUDE})
	target_link_libraries(${ARGV0} ${COMMON_LIBRARIES} lavatube)
	target_compile_options(${ARGV0} PRIVATE ${COMMON_FLAGS})
	add_dependencies(${ARGV0} sync_generated lava-replay lava-tool)
	add_lavatube_test(trace_test_${ARGV0} COMMAND ${ARGV0})
	set_tests_properties(trace_test_${ARGV0} PROPERTIES ENVIRONMENT "LAVATUBE_DELETE_EMPTY_TRACE=1")
	set_property(TEST trace_test_${ARGV0} APPEND PROPERTY FIXTURES_SETUP ${ARGV0})
	set_tests_properties(trace_test_${ARGV0} PROPERTIES SKIP_RETURN_CODE 77)
	add_lavatube_test(trace_test_${ARGV0}_replay COMMAND lava-replay --skip-missing-input -V ${ARGV1})
	set_property(TEST trace_test_${ARGV0}_replay APPEND PROPERTY FIXTURES_REQUIRED ${ARGV0})
	set_tests_properties(trace_test_${ARGV0}_replay PROPERTIES SKIP_RETURN_CODE 77)
endfunction()

if(NOT ANDROID)
internal_test(tracing1 tracing_1_2_0.vk)
add_lavatube_test(trace_test_1_2 COMMAND tracing1 1)
add_lavatube_test(trace_test_1_0_replay_2 COMMAND $<TARGET_FILE:lava-replay> -V -B tracing_1_2_0.vk)
add_lavatube_test(trace_test_1_0_replay_3 COMMAND $<TARGET_FILE:lava-replay> -V --no-multithreaded-io tracing_1_2_0.vk)
add_lavatube_test(trace_test_1_0_replay_4 COMMAND $<TARGET_FILE:lava-replay> -C -V tracing_1_2_0.vk)
add_lavatube_test(trace_test_1_1_replay_1 COMMAND $<TARGET_FILE:lava-replay> -V --no-dedicated tracing_1_2_1.vk)
add_lavatube_test(trace_test_1_1_replay_2 COMMAND $<TARGET_FILE:lava-replay> -V --heap 0 tracing_1_2_1.vk)
add_lavatube_test(trace_test_1_1_replay_3 COMMAND $<TARGET_FILE:lava-replay> -C -V tracing_1_2_1.vk)
endif()

internal_test(tracing_headless tracing_headless.vk)
internal_test(tracing_headless2 tracing_headless2.vk)

internal_test(tracing2 tracing_2.vk)
add_lavatube_test(trace_test_2_replay COMMAND $<TARGET_FILE:lava-replay> --heap 0 tracing_2.vk)
add_lavatube_test(trace_test_2_dedicbuf1 COMMAND tracing2)
set_tests_properties(trace_test_2_dedicbuf1 PROPERTIES ENVIRONMENT "LAVATUBE_DEDICATED_BUFFER=1")
add_lavatube_test(trace_test_2_chunksize COMMAND tracing2)
set_tests_properties(trace_test_2_chunksize PROPERTIES ENVIRONMENT "LAVATUBE_CHUNK_SIZE=32767")
add_lavatube_test(trace_test_2_virtqueue COMMAND tracing2)
set_tests_properties(trace_test_2_virtqueue PROPERTIES ENVIRONMENT "LAVATUBE_VIRTUAL_QUEUES=1")
add_lavatube_test(trace_test_2_nompwrite COMMAND tracing2)
set_tests_properties(trace_test_2_nompwrite PROPERTIES ENVIRONMENT "LAVATUBE_DISABLE_MULTITHREADED_WRITEOUT=1")
add_lavatube_test(trace_test_2_nompcompress COMMAND tracing2)
set_tests_properties(trace_test_2_nompcompress PROPERTIES ENVIRONMENT "LAVATUBE_DISABLE_MULTITHREADED_COMPRESS=1")
add_lavatube_test(trace_test_2_debug_log COMMAND tracing2)
set_tests_properties(trace_test_2_debug_log PROPERTIES ENVIRONMENT "LAVATUBE_DEBUG=2")

internal_test(tracing3 tracing_3.vk)
add_lavatube_test(trace_test_3_replay_mp_cpu COMMAND $<TARGET_FILE:lava-replay> -C -V tracing_3.vk)

internal_test(tracing4 tracing_4_q0_m0_F0.vk)
add_lavatube_test(trace_test_4_0_0 COMMAND tracing4 -q 0 -m 0)
add_lavatube_test(trace_test_4_1_0 COMMAND tracing4 -q 1 -m 0 -c 5)
add_lavatube_test(trace_test_4_2_0 COMMAND tracing4 -q 2 -m 0)
add_lavatube_test(trace_test_4_3_0 COMMAND tracing4 -q 3 -m 0)
add_lavatube_test(trace_test_4_4_0 COMMAND tracing4 -q 4 -m 0)
add_lavatube_test(trace_test_4_0_1 COMMAND tracing4 -q 0 -m 1 -c 7)
add_lavatube_test(trace_test_4_0_2 COMMAND tracing4 -q 0 -m 0 -f 1)
add_lavatube_test(trace_test_4_0_3 COMMAND tracing4 -q 0 -m 2 -H 0)
add_lavatube_test(trace_test_4_1_1 COMMAND tracing4 -q 1 -m 1)
add_lavatube_test(trace_test_4_2_1 COMMAND tracing4 -q 2 -m 1)
add_lavatube_test(trace_test_4_3_1 COMMAND tracing4 -q 3 -m 1)
add_lavatube_test(trace_test_4_4_1 COMMAND tracing4 -q 4 -m 1)
add_lavatube_test(trace_test_4_1_2 COMMAND tracing4 -q 1 -m 2)
add_lavatube_test(trace_test_4_2_2 COMMAND tracing4 -q 2 -m 2)
add_lavatube_test(trace_test_4_3_2 COMMAND tracing4 -q 3 -m 2)
add_lavatube_test(trace_test_4_4_2 COMMAND tracing4 -q 4 -m 2 -b 1000)
add_lavatube_test(trace_test_4_replay_1 COMMAND $<TARGET_FILE:lava-replay> tracing_4_q1_m1_F0.vk)
add_lavatube_test(trace_test_4_replay_2 COMMAND $<TARGET_FILE:lava-replay> tracing_4_q2_m2_F0.vk)
add_lavatube_test(trace_test_4_replay_3 COMMAND $<TARGET_FILE:lava-replay> tracing_4_q3_m0_F0.vk)
add_lavatube_test(trace_test_4_replay_4 COMMAND $<TARGET_FILE:lava-replay> --no-anisotropy tracing_4_q4_m1_F0.vk)
add_lavatube_test(trace_test_4_replay_5 COMMAND $<TARGET_FILE:lava-replay> -B tracing_4_q4_m1_F0.vk)
add_lavatube_test(trace_test_4_replay_6 COMMAND $<TARGET_FILE:lava-replay> -w none -B tracing_4_q4_m1_F0.vk)
add_lavatube_test(trace_test_4_virtqueue COMMAND tracing4)
set_tests_properties(trace_test_4_virtqueue PROPERTIES ENVIRONMENT "LAVATUBE_DESTINATION=tracing_4_virtqueue;LAVATUBE_VIRTUAL_QUEUES=1")
add_lavatube_test(trace_test_4_virtqueue_replay COMMAND $<TARGET_FILE:lava-replay> tracing_4_virtqueue.vk)

internal_test(tracing5 tracing_5.vk)
add_lavatube_test(trace_test_5_replay_cpu COMMAND $<TARGET_FILE:lava-replay> -C -V tracing_5.vk)

internal_test(tracing6 tracing_6.vk)
add_lavatube_test(trace_test_6_replay_cpu COMMAND $<TARGET_FILE:lava-replay> -C -V tracing_6.vk)

internal_test(tracing7 tracing_7.vk)
add_lavatube_test(trace_test_7_replay_cpu COMMAND $<TARGET_FILE:lava-replay> -C -V tracing_7.vk)

internal_test(tracing_dynamic_descriptors tracing_dynamic_descriptors.vk)
add_lavatube_test(trace_test_dynamic_descriptors_replay_cpu COMMAND $<TARGET_FILE:lava-replay> -C -V tracing_dynamic_descriptors.vk)

internal_test(tracing_remap tracing_remap.vk)
add_lavatube_test(trace_test_remap_replay_cpu COMMAND $<TARGET_FILE:lava-replay> -C -V tracing_remap.vk)
add_lavatube_test(trace_test_remap_validate COMMAND $<TARGET_FILE:lava-tool> tracing_remap.vk)

internal_test(tracing_remap_noncoherent tracing_remap_noncoherent.vk)
add_lavatube_test(trace_test_remap_noncoherent_replay_cpu COMMAND $<TARGET_FILE:lava-replay> -C -V tracing_remap_noncoherent.vk)
add_lavatube_test(trace_test_remap_noncoherent_validate COMMAND $<TARGET_FILE:lava-tool> tracing_remap_noncoherent.vk)

internal_test(tracing_buffers tracing_buffers.vk)
add_lavatube_test(trace_test_buffers_replay_cpu COMMAND $<TARGET_FILE:lava-replay> -C -V tracing_buffers.vk)
set_tests_properties(trace_test_buffers_replay_cpu PROPERTIES FIXTURES_REQUIRED tracing_buffers)

internal_test(tracing_memory tracing_memory.vk)
add_test(NAME trace_test_memory_replay_cpu COMMAND ${CMAKE_CURRENT_BINARY_DIR}/lava-replay -C -V tracing_memory.vk)
set_tests_properties(trace_test_memory_replay_cpu PROPERTIES FIXTURES_REQUIRED tracing_memory)

internal_test(tracing_alignment tracing_alignment.vk)
add_lavatube_test(trace_test_alignment_replay_cpu COMMAND $<TARGET_FILE:lava-replay> -C -V tracing_alignment.vk)
set_tests_properties(trace_test_alignment_replay_cpu PROPERTIES FIXTURES_REQUIRED tracing_alignment)

internal_test(tracing_unused tracing_unused.vk)

add_executable(write1 tests/write1.cpp)
target_include_directories(write1 ${COMMON_INCLUDE})
target_link_libraries(write1 ${COMMON_LIBRARIES} lavatube)
target_compile_options(write1 PRIVATE ${COMMON_FLAGS})
add_dependencies(write1 sync_generated)
add_lavatube_test(write_test_1 COMMAND write1)

add_executable(write2 tests/write2.cpp)
target_include_directories(write2 ${COMMON_INCLUDE})
target_link_libraries(write2 ${COMMON_LIBRARIES} lavatube)
target_compile_options(write2 PRIVATE ${COMMON_FLAGS})
add_dependencies(write2 sync_generated)
add_lavatube_test(write_test_2 COMMAND write2)

add_executable(write3 tests/write3.cpp)
target_include_directories(write3 ${COMMON_INCLUDE})
target_link_libraries(write3 ${COMMON_LIBRARIES} lavatube)
target_compile_options(write3 PRIVATE ${COMMON_FLAGS})
add_dependencies(write3 sync_generated)
add_lavatube_test(write_test_3 COMMAND write3)

add_executable(read3 tests/read3.cpp)
target_include_directories(read3 ${COMMON_INCLUDE})
target_link_libraries(read3 ${COMMON_LIBRARIES} lavatube)
target_compile_options(read3 PRIVATE ${COMMON_FLAGS})
add_dependencies(read3 sync_generated)
add_lavatube_test(read_test_3 COMMAND read3)
set_property(TEST read_test_3 APPEND PROPERTY DEPENDS write_test_3)

add_executable(write3-2 tests/write3-2.cpp)
target_include_directories(write3-2 ${COMMON_INCLUDE})
target_link_libraries(write3-2 ${COMMON_LIBRARIES} lavatube)
target_compile_options(write3-2 PRIVATE ${COMMON_FLAGS})
add_dependencies(write3-2 sync_generated)
add_lavatube_test(write_test_3-2 COMMAND write3-2)

add_executable(write4 tests/write4.cpp src/filewriter.cpp src/util.cpp src/android_utils.cpp)
target_include_directories(write4 ${COMMON_INCLUDE})
target_link_libraries(write4 ${MOST_COMMON_LIBRARIES} density LZ4::LZ4)

target_compile_options(write4 PRIVATE ${COMMON_FLAGS})
add_dependencies(write4 sync_generated)
add_lavatube_test(write_test_4 COMMAND write4)

add_executable(read4 tests/read4.cpp src/filereader.cpp src/util.cpp src/android_utils.cpp)
target_include_directories(read4 ${COMMON_INCLUDE})
target_link_libraries(read4 ${MOST_COMMON_LIBRARIES} density LZ4::LZ4 zipcontainer)

target_compile_options(read4 PRIVATE ${COMMON_FLAGS})
add_dependencies(read4 sync_generated)
add_lavatube_test(read_test_4 COMMAND read4)
set_property(TEST read_test_4 APPEND PROPERTY DEPENDS write_test_4)

add_executable(read_preload tests/read_preload.cpp src/filewriter.cpp src/filereader.cpp src/util.cpp src/android_utils.cpp)
target_include_directories(read_preload ${COMMON_INCLUDE})
target_link_libraries(read_preload ${MOST_COMMON_LIBRARIES} density LZ4::LZ4 zipcontainer)
target_compile_options(read_preload PRIVATE ${COMMON_FLAGS})
add_dependencies(read_preload sync_generated)
add_lavatube_test(read_test_preload COMMAND read_preload)
set_tests_properties(read_test_preload PROPERTIES TIMEOUT 10)

add_executable(write5 tests/write5.cpp src/filewriter.cpp src/filereader.cpp src/util.cpp src/android_utils.cpp)
target_include_directories(write5 ${COMMON_INCLUDE})
target_link_libraries(write5 ${MOST_COMMON_LIBRARIES} density LZ4::LZ4 zipcontainer)

target_compile_options(write5 PRIVATE ${COMMON_FLAGS})
add_dependencies(write5 sync_generated)
add_lavatube_test(write_test_5 COMMAND write5)

# --- lava-replay ---

add_executable(lava-replay src/replay.cpp src/trace_metadata.cpp src/trace_metadata.h)
target_include_directories(lava-replay ${COMMON_INCLUDE})
target_link_libraries(lava-replay ${COMMON_LIBRARIES} lavatube)
target_compile_options(lava-replay PRIVATE ${COMMON_FLAGS})
add_dependencies(lava-replay sync_generated)
add_lavatube_test(replayer_info COMMAND lava-replay -i tracing_2.vk)
set_tests_properties(replayer_info PROPERTIES FIXTURES_REQUIRED tracing_2)
add_lavatube_test(replayer_vulkan_demo COMMAND lava-replay -B -w none ${PROJECT_SOURCE_DIR}/traces/demo_computeraytracing.vk)
add_lavatube_test(replayer_vulkan_demo2 COMMAND lava-replay -B -w none -f 10 20 ${PROJECT_SOURCE_DIR}/traces/demo_computeraytracing.vk)
add_lavatube_test(replayer_samples COMMAND lava-replay -B -w none -f 5 9 ${PROJECT_SOURCE_DIR}/traces/sample_hello_triangle.vk)
add_lavatube_test(replayer_vkcube COMMAND lava-replay -B -w none -f 5 9 ${PROJECT_SOURCE_DIR}/traces/lunarg_vkcube.vk)
add_lavatube_test(replayer_vulkan_demo_cpu COMMAND lava-replay -C -B -w none ${PROJECT_SOURCE_DIR}/traces/demo_computeraytracing.vk)
add_lavatube_test(replayer_samples_cpu COMMAND lava-replay -C -B -w none ${PROJECT_SOURCE_DIR}/traces/sample_hello_triangle.vk)
add_lavatube_test(replayer_vkcube_cpu COMMAND lava-replay -C -B -w none ${PROJECT_SOURCE_DIR}/traces/lunarg_vkcube.vk)
# -p/--preload, --frames, and --allow-stalls coverage
add_lavatube_test(replayer_samples_preload0 COMMAND lava-replay -B -w none -p 0 ${PROJECT_SOURCE_DIR}/traces/sample_hello_triangle.vk)
add_lavatube_test(replayer_vkcube_preload0 COMMAND lava-replay -B -w none -p 0 ${PROJECT_SOURCE_DIR}/traces/lunarg_vkcube.vk)
add_lavatube_test(replayer_samples_frames_preload0 COMMAND lava-replay -B -w none -f 5 9 -p 0 ${PROJECT_SOURCE_DIR}/traces/sample_hello_triangle.vk)
add_lavatube_test(replayer_vkcube_frames_preload0 COMMAND lava-replay -B -w none -f 5 9 -p 0 ${PROJECT_SOURCE_DIR}/traces/lunarg_vkcube.vk)
add_lavatube_test(replayer_demo_frames_preload0 COMMAND lava-replay -B -w none -f 10 20 -p 0 ${PROJECT_SOURCE_DIR}/traces/demo_computeraytracing.vk)
add_lavatube_test(replayer_demo_frames_preload1 COMMAND lava-replay -B -w none -f 10 20 -p 1 ${PROJECT_SOURCE_DIR}/traces/demo_computeraytracing.vk)
add_lavatube_test(replayer_samples_frames_nostalls COMMAND lava-replay -B -w none -f 5 9 -p 1 ${PROJECT_SOURCE_DIR}/traces/sample_hello_triangle.vk)
set_tests_properties(replayer_samples_frames_nostalls PROPERTIES ENVIRONMENT "LAVATUBE_ALLOW_STALLS=0")
if(NOT ANDROID)
add_lavatube_test(trace_test_1_0_replay_preload0 COMMAND lava-replay --skip-missing-input -p 0 tracing_1_2_0.vk)
set_tests_properties(trace_test_1_0_replay_preload0 PROPERTIES FIXTURES_REQUIRED tracing1 SKIP_RETURN_CODE 77)
endif()
add_lavatube_test(replayer_samples_screenshot0 COMMAND lava-replay -C -w none --screenshots 0 ${PROJECT_SOURCE_DIR}/traces/sample_hello_triangle.vk)
set_tests_properties(replayer_samples_screenshot0 PROPERTIES FIXTURES_SETUP ReplayScreenshot0 WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
add_lavatube_test(replayer_samples_screenshot0_validate COMMAND check_png screenshot_frame_0.png)
set_tests_properties(replayer_samples_screenshot0_validate PROPERTIES FIXTURES_REQUIRED ReplayScreenshot0 WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
add_lavatube_test(replayer_samples_screenshot7 COMMAND lava-replay -C -w none -f 5 9 --screenshots 7 ${PROJECT_SOURCE_DIR}/traces/sample_hello_triangle.vk)
set_tests_properties(replayer_samples_screenshot7 PROPERTIES FIXTURES_SETUP ReplayScreenshot7 WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
add_lavatube_test(replayer_samples_screenshot7_validate COMMAND check_png screenshot_frame_7.png)
set_tests_properties(replayer_samples_screenshot7_validate PROPERTIES FIXTURES_REQUIRED ReplayScreenshot7 WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
add_lavatube_test(replayer_samples_screenshot_prefix COMMAND lava-replay -C -w none --screenshots 0 --screenshot-prefix capture_ ${PROJECT_SOURCE_DIR}/traces/sample_hello_triangle.vk)
set_tests_properties(replayer_samples_screenshot_prefix PROPERTIES FIXTURES_SETUP ReplayScreenshotPrefix WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
add_lavatube_test(replayer_samples_screenshot_prefix_validate COMMAND check_png capture_0.png)
set_tests_properties(replayer_samples_screenshot_prefix_validate PROPERTIES FIXTURES_REQUIRED ReplayScreenshotPrefix WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
add_lavatube_test(replayer_screenshot_invalid_spec COMMAND lava-replay --screenshots 4-2)
set_tests_properties(replayer_screenshot_invalid_spec PROPERTIES WILL_FAIL TRUE)
add_lavatube_test(replayer_screenshot_blackhole_rejected COMMAND lava-replay -C -B -w none --screenshots 0 ${PROJECT_SOURCE_DIR}/traces/sample_hello_triangle.vk)
set_tests_properties(replayer_screenshot_blackhole_rejected PROPERTIES WILL_FAIL TRUE)
add_lavatube_test(replayer_screenshot_prefix_requires_screenshots COMMAND lava-replay --screenshot-prefix capture_)
set_tests_properties(replayer_screenshot_prefix_requires_screenshots PROPERTIES WILL_FAIL TRUE)
install(TARGETS lava-replay RUNTIME)

# --- lava-tool ---

file(REMOVE "log.txt")
add_executable(lava-tool src/tool.cpp)
target_include_directories(lava-tool ${COMMON_INCLUDE})
target_link_libraries(lava-tool ${COMMON_LIBRARIES} lavatube)
target_compile_options(lava-tool PRIVATE ${COMMON_FLAGS})
add_dependencies(lava-tool sync_generated)
add_lavatube_test(trace_test_tracing1_tool_rewrite
	COMMAND ${CMAKE_COMMAND}
		-DLAVA_TOOL=$<TARGET_FILE:lava-tool>
		-DINPUT_TRACE=${CMAKE_CURRENT_BINARY_DIR}/tracing_1_1.vk
		-DOUTPUT_TRACE=/tmp/tracing_1_1_tool_rewrite.vk
		-P ${PROJECT_SOURCE_DIR}/tests/lava_tool_rewrite.cmake)
install(TARGETS lava-tool RUNTIME)

# --- lava-print ---

add_executable(lava-print src/print.cpp)
target_include_directories(lava-print ${COMMON_INCLUDE})
target_link_libraries(lava-print ${COMMON_LIBRARIES} lavatube)
target_compile_options(lava-print PRIVATE ${COMMON_FLAGS})
add_dependencies(lava-print sync_generated)
add_lavatube_test(lava_print_tracing2_ndjson
	COMMAND ${CMAKE_COMMAND}
		-DLAVA_PRINT=$<TARGET_FILE:lava-print>
		-DPYTHON_EXECUTABLE=${PYTHON_EXECUTABLE}
		-DINPUT_TRACE=${CMAKE_CURRENT_BINARY_DIR}/tracing_2.vk
		-DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/lava_print_tracing2.ndjson
		-P ${PROJECT_SOURCE_DIR}/tests/lava_print_ndjson.cmake)
set_tests_properties(lava_print_tracing2_ndjson PROPERTIES FIXTURES_REQUIRED tracing_2)
add_lavatube_test(lava_print_vkcube_frame2_ndjson
	COMMAND ${CMAKE_COMMAND}
		-DLAVA_PRINT=$<TARGET_FILE:lava-print>
		-DPYTHON_EXECUTABLE=${PYTHON_EXECUTABLE}
		-DINPUT_TRACE=${PROJECT_SOURCE_DIR}/traces/lunarg_vkcube.vk
		-DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/lava_print_vkcube_frame2.ndjson
		-DFRAME_START=2
		-DFRAME_END=2
		-DEXPECT_FRAME=2
		-P ${PROJECT_SOURCE_DIR}/tests/lava_print_ndjson.cmake)
add_lavatube_test(lava_print_skip_missing COMMAND lava-print --skip-missing-input does_not_exist.vk)
set_tests_properties(lava_print_skip_missing PROPERTIES SKIP_RETURN_CODE 77)
install(TARGETS lava-print RUNTIME)

# --- lava-cli ---

add_executable(lava-cli src/cli.cpp)
target_include_directories(lava-cli ${COMMON_INCLUDE})
target_link_libraries(lava-cli ${COMMON_LIBRARIES} lavatube)
target_compile_options(lava-cli PRIVATE ${COMMON_FLAGS})
add_dependencies(lava-cli sync_generated)
install(TARGETS lava-cli RUNTIME)

# --- lava-tui ---

if(NOT ANDROID)
add_executable(lava-tui
	src/tui.cpp
	src/tui_app.cpp
	src/tui_app.h
	src/tui_openai.cpp
	src/tui_openai.h
	src/tui_trace_tools.cpp
	src/tui_trace_tools.h
	src/trace_metadata.cpp
	src/trace_metadata.h
)
target_include_directories(lava-tui ${COMMON_INCLUDE})
target_link_libraries(lava-tui ${COMMON_LIBRARIES} lavatube ftxui::component CURL::libcurl)
target_compile_options(lava-tui PRIVATE ${COMMON_FLAGS})
add_dependencies(lava-tui sync_generated)
install(TARGETS lava-tui RUNTIME)

add_executable(tui_trace_tools_test tests/tui_trace_tools.cpp src/tui_trace_tools.cpp src/tui_trace_tools.h src/trace_metadata.cpp src/trace_metadata.h)
target_include_directories(tui_trace_tools_test ${COMMON_INCLUDE})
target_link_libraries(tui_trace_tools_test ${COMMON_LIBRARIES} lavatube)
target_compile_options(tui_trace_tools_test PRIVATE ${COMMON_FLAGS})
add_dependencies(tui_trace_tools_test sync_generated)
add_lavatube_test(tui_trace_tools_test COMMAND tui_trace_tools_test)
endif()

# --- traces ---

function(trace_test test_name trace_file)
	add_lavatube_test(trace_file_test_${ARGV0}_replay COMMAND $<TARGET_FILE:lava-replay> -w none --gpu ${PROJECT_SOURCE_DIR}/${ARGV1})
	set_tests_properties(trace_file_test_${ARGV0}_replay PROPERTIES SKIP_RETURN_CODE 77)
	add_lavatube_test(trace_file_test_${ARGV0}_replay_cpu COMMAND $<TARGET_FILE:lava-replay> -w none -B --cpu ${PROJECT_SOURCE_DIR}/${ARGV1})
	set_tests_properties(trace_file_test_${ARGV0}_replay_cpu PROPERTIES SKIP_RETURN_CODE 77)
	add_lavatube_test(trace_file_test_${ARGV0}_validate COMMAND $<TARGET_FILE:lava-tool> ${PROJECT_SOURCE_DIR}/${ARGV1})
	add_lavatube_test(trace_file_test_${ARGV0}_write_out COMMAND $<TARGET_FILE:lava-tool> ${PROJECT_SOURCE_DIR}/${ARGV1} ${CMAKE_CURRENT_BINARY_DIR}/tmp_${ARGV0}.vk)
	set_tests_properties(trace_file_test_${ARGV0}_write_out PROPERTIES SKIP_RETURN_CODE 77 FIXTURES_SETUP "tmp_${ARGV0}" FIXTURES_REQUIRES "cleanup_tmp")
	add_lavatube_test(trace_file_test_${ARGV0}_write_out_replay COMMAND $<TARGET_FILE:lava-replay> -w none --gpu ${CMAKE_CURRENT_BINARY_DIR}/tmp_${ARGV0}.vk)
	set_tests_properties(trace_file_test_${ARGV0}_write_out_replay PROPERTIES SKIP_RETURN_CODE 77 FIXTURES_REQUIRES "tmp_${ARGV0};cleanup_tmp")
endfunction()

trace_test(raytracingbasic traces/demo_raytracingbasic.vk)
trace_test(raytracingsbtdata traces/demo_raytracingsbtdata.vk)
trace_test(descriptorindexing traces/demo_descriptorindexing.vk)
trace_test(pushconstants traces/demo_pushconstants.vk)
trace_test(inlineuniformblocks traces/demo_inlineuniformblocks.vk)
trace_test(raytracingintersection traces/demo_raytracingintersection.vk)
trace_test(raytracingpositionfetch traces/demo_raytracingpositionfetch.vk)
trace_test(geometryshader traces/demo_geometryshader.vk)
trace_test(inputattachments traces/demo_inputattachments.vk)
trace_test(anki_simple traces/anki_simple.vk)

# --- packtool ---

add_executable(packtool src/packtool.cpp)
target_include_directories(packtool ${COMMON_INCLUDE})
target_link_libraries(packtool ${COMMON_LIBRARIES} lavatube)
target_compile_options(packtool PRIVATE ${COMMON_FLAGS} -fPIC)
add_dependencies(packtool sync_generated)
add_lavatube_test(pack_test_1 COMMAND packtool) # usage
set_tests_properties(pack_test_1 PROPERTIES FIXTURES_REQUIRED tracing_2)
add_lavatube_test(pack_test_2 COMMAND packtool extract tracking.json tracing_2.vk)
set_tests_properties(pack_test_2 PROPERTIES FIXTURES_REQUIRED tracing_2)
add_lavatube_test(pack_test_3_1 COMMAND packtool print tracking.json tracing_2.vk)
set_tests_properties(pack_test_3_1 PROPERTIES FIXTURES_REQUIRED tracing_2)
add_lavatube_test(pack_test_3_2 COMMAND packtool extract thread_0.bin tracing_2.vk)
set_tests_properties(pack_test_3_2 PROPERTIES FIXTURES_REQUIRED tracing_2)
add_lavatube_test(pack_test_4 COMMAND packtool unpack tmp_2 tracing_2.vk)
set_tests_properties(pack_test_4 PROPERTIES FIXTURES_REQUIRED tracing_2)
add_lavatube_test(pack_test_4_1 COMMAND cmp ${CMAKE_CURRENT_BINARY_DIR}/tmp_2/tracking.json ${CMAKE_CURRENT_BINARY_DIR}/tracking.json)
set_tests_properties(pack_test_4_1 PROPERTIES FIXTURES_REQUIRED tracing_2)
add_lavatube_test(pack_test_4_2 COMMAND cmp ${CMAKE_CURRENT_BINARY_DIR}/tmp_2/thread_0.bin ${CMAKE_CURRENT_BINARY_DIR}/thread_0.bin)
set_tests_properties(pack_test_4_2 PROPERTIES FIXTURES_REQUIRED tracing_2)
add_lavatube_test(pack_test_5 COMMAND packtool list tracing_2.vk)
set_tests_properties(pack_test_5 PROPERTIES FIXTURES_REQUIRED tracing_2)
add_lavatube_test(pack_test_6 COMMAND packtool check tracing_2.vk)
set_tests_properties(pack_test_6 PROPERTIES FIXTURES_REQUIRED tracing_2)
add_lavatube_test(pack_test_7_markings
	COMMAND ${CMAKE_COMMAND}
		-DLAVA_TOOL=$<TARGET_FILE:lava-tool>
		-DPACKTOOL=$<TARGET_FILE:packtool>
		-DINPUT_TRACE=${CMAKE_CURRENT_BINARY_DIR}/vulkan_raytracing_indirect_noop.vk
		-DOUTPUT_TRACE=${CMAKE_CURRENT_BINARY_DIR}/tmp_markings.vk
		-P ${PROJECT_SOURCE_DIR}/tests/packtool_markings_diff.cmake)
set_tests_properties(pack_test_7_markings PROPERTIES FIXTURES_REQUIRED raytracing_indirect_noop)
add_lavatube_test(pack_test_7_markings_simulate
	COMMAND ${CMAKE_COMMAND}
		-DSIMULATE=ON
		-DLAVA_TOOL=$<TARGET_FILE:lava-tool>
		-DPACKTOOL=$<TARGET_FILE:packtool>
		-DINPUT_TRACE=${CMAKE_CURRENT_BINARY_DIR}/vulkan_raytracing_indirect_noop.vk
		-DOUTPUT_TRACE=${CMAKE_CURRENT_BINARY_DIR}/tmp_markings_simulate.vk
		-P ${PROJECT_SOURCE_DIR}/tests/packtool_markings_diff.cmake)
set_tests_properties(pack_test_7_markings_simulate PROPERTIES FIXTURES_REQUIRED raytracing_indirect_noop)
add_lavatube_test(pack_test_7_markings_simulate_explicit
	COMMAND ${CMAKE_COMMAND}
		-DSIMULATE=ON
		-DLAVA_TOOL=$<TARGET_FILE:lava-tool>
		-DPACKTOOL=$<TARGET_FILE:packtool>
		-DINPUT_TRACE=${CMAKE_CURRENT_BINARY_DIR}/vulkan_raytracing_indirect_noop_explicit.vk
		-DCOMPARE_TRACE=${CMAKE_CURRENT_BINARY_DIR}/vulkan_raytracing_indirect_noop.vk
		-DOUTPUT_TRACE=${CMAKE_CURRENT_BINARY_DIR}/tmp_markings_simulate_explicit.vk
		-P ${PROJECT_SOURCE_DIR}/tests/packtool_markings_diff.cmake)
set_tests_properties(pack_test_7_markings_simulate_explicit PROPERTIES FIXTURES_REQUIRED raytracing_indirect_noop)
install(TARGETS packtool RUNTIME)

# --- layer tests ---

function(layer_test test_name test_exe)
	add_lavatube_test(layer_test_${ARGV0} COMMAND vulkan_${ARGV1} --gpu ${ARGV2} ${ARGV3} ${ARGV4} ${ARGV5} ${ARGV6} ${ARGV7})
	set_tests_properties(layer_test_${ARGV0} PROPERTIES
		FIXTURES_SETUP ${ARGV0}
		ENVIRONMENT "LAVATUBE_VIRTUAL_QUEUES=1;LAVATUBE_DELETE_EMPTY_TRACE=1;LAVATUBE_DESTINATION=vulkan_${ARGV0};VK_LAYER_PATH=${CMAKE_CURRENT_BINARY_DIR}/implicit_layer.d;VK_INSTANCE_LAYERS=VK_LAYER_ARM_lavatube;LD_LIBRARY_PATH=${CMAKE_CURRENT_BINARY_DIR}/implicit_layer.d"
		SKIP_RETURN_CODE 77
	)

	add_lavatube_test(layer_test_${ARGV0}_raw COMMAND vulkan_${ARGV1} --gpu --no-explicit --no-trace-helpers ${ARGV2} ${ARGV3} ${ARGV4} ${ARGV5} ${ARGV6} ${ARGV7})
	set_tests_properties(layer_test_${ARGV0}_raw PROPERTIES
		FIXTURES_SETUP ${ARGV0}
		ENVIRONMENT "LAVATUBE_VIRTUAL_QUEUES=1;LAVATUBE_DELETE_EMPTY_TRACE=1;LAVATUBE_DESTINATION=vulkan_${ARGV0}_raw;VK_LAYER_PATH=${CMAKE_CURRENT_BINARY_DIR}/implicit_layer.d;VK_INSTANCE_LAYERS=VK_LAYER_ARM_lavatube;LD_LIBRARY_PATH=${CMAKE_CURRENT_BINARY_DIR}/implicit_layer.d"
		SKIP_RETURN_CODE 77
	)

	add_lavatube_test(layer_test_${ARGV0}_explicit COMMAND vulkan_${ARGV1} --gpu --no-trace-helpers ${ARGV2} ${ARGV3} ${ARGV4} ${ARGV5} ${ARGV6} ${ARGV7})
	set_tests_properties(layer_test_${ARGV0}_explicit PROPERTIES
		FIXTURES_SETUP ${ARGV0}
		ENVIRONMENT "LAVATUBE_VIRTUAL_QUEUES=1;LAVATUBE_DELETE_EMPTY_TRACE=1;LAVATUBE_DESTINATION=vulkan_${ARGV0}_explicit;VK_LAYER_PATH=${CMAKE_CURRENT_BINARY_DIR}/implicit_layer.d;VK_INSTANCE_LAYERS=VK_LAYER_ARM_lavatube;LD_LIBRARY_PATH=${CMAKE_CURRENT_BINARY_DIR}/implicit_layer.d"
		SKIP_RETURN_CODE 77
	)

	add_lavatube_test(layer_test_${ARGV0}_simulation COMMAND vulkan_${ARGV1} --cpu ${ARGV2} ${ARGV3} ${ARGV4} ${ARGV5} ${ARGV6} ${ARGV7})
	set_tests_properties(layer_test_${ARGV0}_simulation PROPERTIES
		FIXTURES_SETUP ${ARGV0}_simulation
		ENVIRONMENT "LAVATUBE_VIRTUAL_QUEUES=1;LAVATUBE_DELETE_EMPTY_TRACE=1;LAVATUBE_DESTINATION=vulkan_${ARGV0}_simulation;VK_LAYER_PATH=${CMAKE_CURRENT_BINARY_DIR}/implicit_layer.d;VK_INSTANCE_LAYERS=VK_LAYER_ARM_lavatube;LD_LIBRARY_PATH=${CMAKE_CURRENT_BINARY_DIR}/implicit_layer.d"
		SKIP_RETURN_CODE 77
	)

	set(RUN_IF_EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/cmake/run_if_exists.sh)
	set(REMOVE_THEN_RUN ${CMAKE_CURRENT_SOURCE_DIR}/cmake/remove_then_run.sh)
	set(TRACE_FILE ${CMAKE_CURRENT_BINARY_DIR}/vulkan_${ARGV0}.vk)
	set(SIM_TRACE_FILE ${CMAKE_CURRENT_BINARY_DIR}/vulkan_${ARGV0}_simulation.vk)
	set(RAW_TRACE_FILE ${CMAKE_CURRENT_BINARY_DIR}/vulkan_${ARGV0}_raw.vk)
	set(MODEL_TRACE_FILE ${CMAKE_CURRENT_BINARY_DIR}/vulkan_${ARGV0}_model.vk)
	add_lavatube_test(layer_test_${ARGV0}_replay COMMAND ${RUN_IF_EXISTS} ${TRACE_FILE} $<TARGET_FILE:lava-replay> -V vulkan_${ARGV0}.vk)
	set_tests_properties(layer_test_${ARGV0}_replay PROPERTIES SKIP_RETURN_CODE 77 FIXTURES_REQUIRED ${ARGV0})
	add_lavatube_test(layer_test_${ARGV0}_replay_raw COMMAND ${RUN_IF_EXISTS} ${RAW_TRACE_FILE} $<TARGET_FILE:lava-replay> --blackhole --allow-stalls vulkan_${ARGV0}_raw.vk)
	set_tests_properties(layer_test_${ARGV0}_replay_raw PROPERTIES SKIP_RETURN_CODE 77 FIXTURES_REQUIRED ${ARGV0})
	add_lavatube_test(layer_test_${ARGV0}_replay_cpu COMMAND ${RUN_IF_EXISTS} ${TRACE_FILE} $<TARGET_FILE:lava-replay> -C -V vulkan_${ARGV0}.vk)
	set_tests_properties(layer_test_${ARGV0}_replay_cpu PROPERTIES FIXTURES_REQUIRED ${ARGV0} SKIP_RETURN_CODE 77)
	add_lavatube_test(layer_test_${ARGV0}_replay_blackhole COMMAND ${RUN_IF_EXISTS} ${TRACE_FILE} $<TARGET_FILE:lava-replay> -V -B vulkan_${ARGV0}.vk)
	set_tests_properties(layer_test_${ARGV0}_replay_blackhole PROPERTIES SKIP_RETURN_CODE 77 FIXTURES_REQUIRED ${ARGV0})
	add_lavatube_test(layer_test_${ARGV0}_replay_blackhole_nonewsi COMMAND ${RUN_IF_EXISTS} ${TRACE_FILE} $<TARGET_FILE:lava-replay> -V -B -w none vulkan_${ARGV0}.vk)
	set_tests_properties(layer_test_${ARGV0}_replay_blackhole_nonewsi PROPERTIES SKIP_RETURN_CODE 77 FIXTURES_REQUIRED ${ARGV0})
	add_lavatube_test(layer_test_${ARGV0}_replay_nonewsi COMMAND ${RUN_IF_EXISTS} ${TRACE_FILE} $<TARGET_FILE:lava-replay> -V -w none vulkan_${ARGV0}.vk)
	set_tests_properties(layer_test_${ARGV0}_replay_nonewsi PROPERTIES SKIP_RETURN_CODE 77 FIXTURES_REQUIRED ${ARGV0})
	add_lavatube_test(layer_test_${ARGV0}_replay_from_simulation COMMAND ${RUN_IF_EXISTS} ${SIM_TRACE_FILE} $<TARGET_FILE:lava-replay> -V vulkan_${ARGV0}_simulation.vk)
	set_tests_properties(layer_test_${ARGV0}_replay_from_simulation PROPERTIES FIXTURES_REQUIRED "${ARGV0}_simulation;${ARGV0}" SKIP_RETURN_CODE 77)
	add_test(NAME layer_test_${ARGV0}_write_out COMMAND ${RUN_IF_EXISTS} ${TRACE_FILE} ${CMAKE_CURRENT_BINARY_DIR}/lava-tool --skip-missing-input vulkan_${ARGV0}.vk tmp_${ARGV0}.vk)
	set_tests_properties(layer_test_${ARGV0}_write_out PROPERTIES FIXTURES_REQUIRED "${ARGV0}" SKIP_RETURN_CODE 77)
	add_test(NAME layer_test_${ARGV0}_write_out_replay COMMAND ${RUN_IF_EXISTS} tmp_${ARGV0}.vk ${CMAKE_CURRENT_BINARY_DIR}/lava-replay --skip-missing-input -V tmp_${ARGV0}.vk)
	set_tests_properties(layer_test_${ARGV0}_write_out_replay PROPERTIES FIXTURES_REQUIRED "${ARGV0};cleanup_tmp" SKIP_RETURN_CODE 77)

	if (NOT GPU_MODEL STREQUAL "")
		set(MODEL_CAPTURE_EXTRA_ARGS)
		if (ARGV1 STREQUAL "compute_wait")
			set(MODEL_CAPTURE_EXTRA_ARGS -W 1 -H 1 --payload-bytes 16 --dummy-copy-rounds 0)
		endif()
		add_lavatube_test(layer_test_${ARGV0}_model_capture COMMAND ${REMOVE_THEN_RUN} ${MODEL_TRACE_FILE} $<TARGET_FILE:vulkan_${ARGV1}> ${ARGV2} ${ARGV3} ${ARGV4} ${ARGV5} ${ARGV6} ${ARGV7} ${MODEL_CAPTURE_EXTRA_ARGS})
		set_tests_properties(layer_test_${ARGV0}_model_capture PROPERTIES
			FIXTURES_SETUP ${ARGV0}
			ENVIRONMENT "${GPU_MODEL};LAVATUBE_VIRTUAL_QUEUES=1;LAVATUBE_DELETE_EMPTY_TRACE=1;LAVATUBE_DESTINATION=vulkan_${ARGV0}_model;VK_LAYER_PATH=${CMAKE_CURRENT_BINARY_DIR}/implicit_layer.d;VK_INSTANCE_LAYERS=VK_LAYER_ARM_lavatube"
			ENVIRONMENT_MODIFICATION "LD_LIBRARY_PATH=path_list_prepend:${CMAKE_CURRENT_BINARY_DIR}/implicit_layer.d"
			SKIP_RETURN_CODE 77
		)
		add_lavatube_test(layer_test_${ARGV0}_model_replay COMMAND ${RUN_IF_EXISTS} ${MODEL_TRACE_FILE} $<TARGET_FILE:lava-replay> -V -w none vulkan_${ARGV0}_model.vk)
		set_tests_properties(layer_test_${ARGV0}_model_replay PROPERTIES ENVIRONMENT "${GPU_MODEL}" SKIP_RETURN_CODE 77 FIXTURES_REQUIRED ${ARGV0})
		add_lavatube_test(layer_test_${ARGV0}_model_replay_gpu COMMAND ${RUN_IF_EXISTS} ${TRACE_FILE} $<TARGET_FILE:lava-replay> -V -w none vulkan_${ARGV0}.vk)
		set_tests_properties(layer_test_${ARGV0}_model_replay_gpu PROPERTIES ENVIRONMENT "${GPU_MODEL}" SKIP_RETURN_CODE 77 FIXTURES_REQUIRED ${ARGV0})
		add_lavatube_test(layer_test_${ARGV0}_model_replay_cpu COMMAND ${RUN_IF_EXISTS} ${SIM_TRACE_FILE} $<TARGET_FILE:lava-replay> -V -w none vulkan_${ARGV0}_simulation.vk)
		set_tests_properties(layer_test_${ARGV0}_model_replay_cpu PROPERTIES ENVIRONMENT "${GPU_MODEL}" SKIP_RETURN_CODE 77 FIXTURES_REQUIRED ${ARGV0})
		add_lavatube_test(layer_test_${ARGV0}_gpu_replay_model COMMAND ${RUN_IF_EXISTS} ${MODEL_TRACE_FILE} $<TARGET_FILE:lava-replay> -V --gpu vulkan_${ARGV0}_model.vk)
		set_tests_properties(layer_test_${ARGV0}_gpu_replay_model PROPERTIES SKIP_RETURN_CODE 77 FIXTURES_REQUIRED ${ARGV0})
		add_lavatube_test(layer_test_${ARGV0}_cpu_replay_model COMMAND ${RUN_IF_EXISTS} ${MODEL_TRACE_FILE} $<TARGET_FILE:lava-replay> -V --cpu vulkan_${ARGV0}_model.vk)
		set_tests_properties(layer_test_${ARGV0}_cpu_replay_model PROPERTIES SKIP_RETURN_CODE 77 FIXTURES_REQUIRED ${ARGV0})
	endif()
endfunction()

layer_test(general general)
if(NOT NO_TRACETOOLTESTS AND NOT ANDROID)
	set(LAVATUBE_CHAMELEON_VULKANCORE_1_4_GPU_PATH "${PROJECT_SOURCE_DIR}/external/tracetooltests/share/chameleon/devices/VulkanCore-1.4")
	set(LAVATUBE_CHAMELEON_VULKANCORE_1_4_ENV "VK_DRIVER_FILES=${CMAKE_CURRENT_BINARY_DIR}/tracetooltests/chameleon_icd.json;CHAMELEON_GPU=${LAVATUBE_CHAMELEON_VULKANCORE_1_4_GPU_PATH}")
	set(LAVATUBE_CHAMELEON_LAYER_ENV "${LAVATUBE_CHAMELEON_VULKANCORE_1_4_ENV};LAVATUBE_VIRTUAL_QUEUES=1;LAVATUBE_DELETE_EMPTY_TRACE=1;LAVATUBE_DESTINATION=vulkan_general_chameleon_vulkancore_1_4;VK_LAYER_PATH=${CMAKE_CURRENT_BINARY_DIR}/implicit_layer.d;VK_INSTANCE_LAYERS=VK_LAYER_ARM_lavatube;LD_LIBRARY_PATH=${CMAKE_CURRENT_BINARY_DIR}/implicit_layer.d")
	set(LAVATUBE_RUN_IF_EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/cmake/run_if_exists.sh)
	set(LAVATUBE_CHAMELEON_TRACE ${CMAKE_CURRENT_BINARY_DIR}/vulkan_general_chameleon_vulkancore_1_4.vk)
	set(LAVATUBE_GENERAL_TRACE ${CMAKE_CURRENT_BINARY_DIR}/vulkan_general.vk)

	add_lavatube_test(layer_test_general_chameleon_vulkancore_1_4 COMMAND vulkan_general --gpu --no-trace-helpers)
	set_tests_properties(layer_test_general_chameleon_vulkancore_1_4 PROPERTIES
		FIXTURES_SETUP general_chameleon_vulkancore_1_4
		ENVIRONMENT "${LAVATUBE_CHAMELEON_LAYER_ENV}"
		SKIP_RETURN_CODE 77
		LABELS "chameleon;icd;lavatube"
	)

	add_lavatube_test(layer_test_general_chameleon_vulkancore_1_4_replay COMMAND ${LAVATUBE_RUN_IF_EXISTS} ${LAVATUBE_CHAMELEON_TRACE} $<TARGET_FILE:lava-replay> -V -w none --swapchain offscreen vulkan_general_chameleon_vulkancore_1_4.vk)
	set_tests_properties(layer_test_general_chameleon_vulkancore_1_4_replay PROPERTIES
		FIXTURES_REQUIRED general_chameleon_vulkancore_1_4
		ENVIRONMENT "${LAVATUBE_CHAMELEON_VULKANCORE_1_4_ENV}"
		SKIP_RETURN_CODE 77
		LABELS "chameleon;icd;lavatube"
	)

	add_lavatube_test(layer_test_general_replay_chameleon_vulkancore_1_4 COMMAND ${LAVATUBE_RUN_IF_EXISTS} ${LAVATUBE_GENERAL_TRACE} $<TARGET_FILE:lava-replay> -V -w none --swapchain offscreen vulkan_general.vk)
	set_tests_properties(layer_test_general_replay_chameleon_vulkancore_1_4 PROPERTIES
		FIXTURES_REQUIRED general
		ENVIRONMENT "${LAVATUBE_CHAMELEON_VULKANCORE_1_4_ENV}"
		SKIP_RETURN_CODE 77
		LABELS "chameleon;icd;lavatube"
	)
endif()
#layer_test(fence_delay fence_delay) # Disabling -- too many intermittent errors
add_test(NAME script_test_copying_1 COMMAND ${PROJECT_SOURCE_DIR}/scripts/lava-capture.py -o script_test_copying_1.vk --dedicated-buffer --gpu 0 --layer-path ${CMAKE_CURRENT_BINARY_DIR}/implicit_layer.d ${CMAKE_CURRENT_BINARY_DIR}/tracetooltests/vulkan_copying_1 -V 2)
add_test(NAME script_test_copying_nomp COMMAND ${PROJECT_SOURCE_DIR}/scripts/lava-capture.py -o script_test_copying_nomp.vk --no-multithread --automate --layer-path ${CMAKE_CURRENT_BINARY_DIR}/implicit_layer.d ${CMAKE_CURRENT_BINARY_DIR}/tracetooltests/vulkan_copying_1 -V 3)
add_test(NAME script_test_copying_nocomp COMMAND ${PROJECT_SOURCE_DIR}/scripts/lava-capture.py -o script_test_copying_nocomp.vk --compression-type NONE --capture-layer ${CMAKE_CURRENT_BINARY_DIR}/implicit_layer.d ${CMAKE_CURRENT_BINARY_DIR}/tracetooltests/vulkan_copying_1 -V 4)
layer_test(general_vulkan13 general -V 3)
layer_test(copying_1 copying_1)
layer_test(copying_1_q1 copying_1 -q 1)
layer_test(copying_1_q2 copying_1 -q 2)
layer_test(copying_1_q3 copying_1 -q 3)
layer_test(copying_1_q4 copying_1 -q 4)
layer_test(copying_1_q5 copying_1 -q 5)
layer_test(copying_1_m1 copying_1 -m 1)
layer_test(copying_1_m2 copying_1 -m 2)
layer_test(copying_1_bda copying_1 -B -DA -V 2)
layer_test(copying_2 copying_2 -q 1)
layer_test(copying_2_vulkan13 copying_2 -V 3 -q 1)
layer_test(copying_3 copying_3)
layer_test(memory_1 memory_1)
layer_test(memory_1_1 memory_1_1)
layer_test(memory_1_1_vulkan13 memory_1_1 -V 3)
layer_test(thread_1 thread_1)
layer_test(thread_2 thread_2)
layer_test(thread_3 thread_3)
layer_test(thread_3_loop10 thread_3 -l 10)
layer_test(thread_4 thread_4)
layer_test(thread_4_2 thread_4 -V 3 -S 100)
layer_test(thread_4_3 thread_4 -V 2 -S 0)
layer_test(compute_bda_sc compute_bda_sc)
layer_test(compute_bda_sc_multi_create_info compute_bda_sc_unhappy --multi-create-info)
layer_test(compute_bda_sc_stage_pnext compute_bda_sc_unhappy --stage-pnext)
layer_test(compute_bda_pushconstant compute_bda_pushconstant)
layer_test(compute_bda_pushconstant_4 compute_bda_pushconstant -V 4)
layer_test(compute_bda_ubo compute_bda_ubo)
layer_test(compute_bda_ubo_ssbo compute_bda_ubo --ssbo)
layer_test(compute_bda_ubo_fb compute_bda_ubo --frame-boundary)
add_lavatube_test(layer_test_thread_3_replay_blackhole_stress_1 COMMAND $<TARGET_FILE:lava-replay> -V -B vulkan_thread_3.vk)
set_tests_properties(layer_test_thread_3_replay_blackhole_stress_1 PROPERTIES FIXTURES_REQUIRED thread_3)
add_lavatube_test(layer_test_thread_3_replay_blackhole_stress_2 COMMAND $<TARGET_FILE:lava-replay> --no-dedicated -B vulkan_thread_3.vk)
set_tests_properties(layer_test_thread_3_replay_blackhole_stress_2 PROPERTIES FIXTURES_REQUIRED thread_3)
add_lavatube_test(layer_test_thread_3_replay_blackhole_stress_3 COMMAND $<TARGET_FILE:lava-replay> -B vulkan_thread_3.vk)
set_tests_properties(layer_test_thread_3_replay_blackhole_stress_3 PROPERTIES FIXTURES_REQUIRED thread_3)
add_lavatube_test(layer_test_thread_3_replay_blackhole_stress_4 COMMAND $<TARGET_FILE:lava-replay> -B vulkan_thread_3.vk)
set_tests_properties(layer_test_thread_3_replay_blackhole_stress_4 PROPERTIES FIXTURES_REQUIRED thread_3)
add_lavatube_test(layer_test_thread_3_replay_blackhole_stress_5 COMMAND $<TARGET_FILE:lava-replay> -B vulkan_thread_3.vk)
set_tests_properties(layer_test_thread_3_replay_blackhole_stress_5 PROPERTIES FIXTURES_REQUIRED thread_3)
add_lavatube_test(layer_test_thread_3_replay_blackhole_stress_6 COMMAND $<TARGET_FILE:lava-replay> -B vulkan_thread_3.vk)
set_tests_properties(layer_test_thread_3_replay_blackhole_stress_6 PROPERTIES FIXTURES_REQUIRED thread_3)
add_lavatube_test(layer_test_thread_3_replay_blackhole_stress_7 COMMAND $<TARGET_FILE:lava-replay> -B vulkan_thread_3.vk)
set_tests_properties(layer_test_thread_3_replay_blackhole_stress_7 PROPERTIES FIXTURES_REQUIRED thread_3)
add_lavatube_test(layer_test_thread_3_replay_blackhole_stress_8 COMMAND $<TARGET_FILE:lava-replay> -B vulkan_thread_3.vk)
set_tests_properties(layer_test_thread_3_replay_blackhole_stress_8 PROPERTIES FIXTURES_REQUIRED thread_3)
add_lavatube_test(layer_test_thread_3_replay_blackhole_stress_9 COMMAND $<TARGET_FILE:lava-replay> -B vulkan_thread_3.vk)
set_tests_properties(layer_test_thread_3_replay_blackhole_stress_9 PROPERTIES FIXTURES_REQUIRED thread_3)
layer_test(compute_1 compute_1)
layer_test(compute_1_indirect compute_1 --indirect)
layer_test(compute_1_indirect_offset compute_1 --indirect -ioff 9)
layer_test(compute_1_fb compute_1 --frame-boundary)
add_lavatube_test(layer_test_compute_1_1 COMMAND vulkan_compute_1 -pc)
set_tests_properties(layer_test_compute_1_1 PROPERTIES ENVIRONMENT "VK_LAYER_PATH=${CMAKE_CURRENT_BINARY_DIR}/implicit_layer.d;VK_INSTANCE_LAYERS=VK_LAYER_ARM_lavatube;LD_LIBRARY_PATH=${CMAKE_CURRENT_BINARY_DIR}/implicit_layer.d;")
add_lavatube_test(layer_test_compute_1_2 COMMAND vulkan_compute_1 -pc -pcf test.bin)
set_tests_properties(layer_test_compute_1_1 PROPERTIES ENVIRONMENT "VK_LAYER_PATH=${CMAKE_CURRENT_BINARY_DIR}/implicit_layer.d;VK_INSTANCE_LAYERS=VK_LAYER_ARM_lavatube;LD_LIBRARY_PATH=${CMAKE_CURRENT_BINARY_DIR}/implicit_layer.d;")
add_lavatube_test(layer_test_compute_1_3 COMMAND vulkan_compute_1 -pc -pcf test.bin)
set_tests_properties(layer_test_compute_1_1 PROPERTIES ENVIRONMENT "VK_LAYER_PATH=${CMAKE_CURRENT_BINARY_DIR}/implicit_layer.d;VK_INSTANCE_LAYERS=VK_LAYER_ARM_lavatube;LD_LIBRARY_PATH=${CMAKE_CURRENT_BINARY_DIR}/implicit_layer.d;")
add_custom_command(TARGET packtool PRE_BUILD COMMAND rm -f test.bin)
layer_test(compute_2 compute_2)
layer_test(compute_2_2 compute_2 -j 1 -s 2)
layer_test(compute_2_m5 compute_2 -m5)
layer_test(compute_3 compute_3)
add_lavatube_test(layer_test_compute_3_frame_boundary_trace COMMAND vulkan_compute_3 --gpu --frame-boundary)
set_tests_properties(layer_test_compute_3_frame_boundary_trace PROPERTIES
	FIXTURES_SETUP Compute3FrameBoundaryTrace
	ENVIRONMENT "LAVATUBE_VIRTUAL_QUEUES=1;LAVATUBE_DELETE_EMPTY_TRACE=1;LAVATUBE_DESTINATION=vulkan_compute_3;VK_LAYER_PATH=${CMAKE_CURRENT_BINARY_DIR}/implicit_layer.d;VK_INSTANCE_LAYERS=VK_LAYER_ARM_lavatube;LD_LIBRARY_PATH=${CMAKE_CURRENT_BINARY_DIR}/implicit_layer.d"
	SKIP_RETURN_CODE 77)
add_lavatube_test(layer_test_compute_3_frame_boundary_screenshot
	COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/cmake/run_if_exists.sh ${CMAKE_CURRENT_BINARY_DIR}/vulkan_compute_3.vk
		$<TARGET_FILE:lava-replay> -C -w none --screenshots 0 --screenshot-prefix frame_boundary_ vulkan_compute_3.vk)
set_tests_properties(layer_test_compute_3_frame_boundary_screenshot PROPERTIES
	FIXTURES_REQUIRED Compute3FrameBoundaryTrace
	FIXTURES_SETUP Compute3FrameBoundaryScreenshot
	SKIP_RETURN_CODE 77
	WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
add_lavatube_test(layer_test_compute_3_frame_boundary_screenshot_validate COMMAND check_png frame_boundary_0.png)
set_tests_properties(layer_test_compute_3_frame_boundary_screenshot_validate PROPERTIES
	FIXTURES_REQUIRED Compute3FrameBoundaryScreenshot
	WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
add_lavatube_test(layer_test_compute_3_frame_boundary_no_frame1
	COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/cmake/run_if_exists.sh ${CMAKE_CURRENT_BINARY_DIR}/vulkan_compute_3.vk
		${CMAKE_COMMAND}
			-DREPLAY=$<TARGET_FILE:lava-replay>
			-DINPUT_TRACE=${CMAKE_CURRENT_BINARY_DIR}/vulkan_compute_3.vk
			-DOUTPUT_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/frame_boundary_unselected_
			-P ${PROJECT_SOURCE_DIR}/tests/replay_frame_boundary_no_frame1.cmake)
set_tests_properties(layer_test_compute_3_frame_boundary_no_frame1 PROPERTIES
	FIXTURES_REQUIRED Compute3FrameBoundaryTrace
	SKIP_RETURN_CODE 77)
layer_test(feature_1 feature_1)
layer_test(feature_2 feature_2)
layer_test(tool_1 tool_1)
layer_test(debug_marker debug_marker)
layer_test(debug_report debug_report)
layer_test(deferred_1 deferred_1)
layer_test(pipelinecache_1 pipelinecache_1)
layer_test(pnext_chain pnext_chain)
layer_test(as_1 as_1)
layer_test(as_2 as_2)
layer_test(as_3 as_3)
layer_test(as_4 as_4)
layer_test(as_5 as_5)
layer_test(as_6 as_6)
layer_test(as_7 as_7)
layer_test(raytracing_1 raytracing_1)
layer_test(raytracing_2 raytracing_2)
layer_test(raytracing_3 raytracing_3)
layer_test(raytracing_4 raytracing_4)
layer_test(raytracing_5 raytracing_5)
layer_test(rayquery rayquery)
#layer_test(aliasing_1 aliasing_1) # TBD need to implement aliasing support
#layer_test(aliasing_2 aliasing_2)
#layer_test(aliasing_3 aliasing_3)
layer_test(privatedata privatedata)
layer_test(privatedataext privatedataext)
#layer_test(multidevice_1 multidevice_1) # assert failure on memory type
#layer_test(multiinstance multiinstance)
layer_test(mesh_1 mesh_1)
layer_test(graphics_1 graphics_1)
#layer_test(pipeline_binary compute_2 -b)
layer_test(pipeline_executable_properties pipeline_executable_properties)
layer_test(cooperative_matrix cooperative_matrix)
layer_test(extended_dynamic_state3 extended_dynamic_state3)
#layer_test(push_descriptor)
layer_test(host_image_copy host_image_copy)
layer_test(host_image_copy_ext host_image_copy_ext)
#layer_test(compute_aliasing compute_aliasing)
layer_test(compute_bda_copying_address compute_bda_copying_address -W 8 -H 8)
layer_test(compute_bda_copying_address_gpu_driven compute_bda_copying_address --gpu-driven -W 8 -H 8)
layer_test(compute_bda_copying_address_minimal compute_bda_copying_address_minimal)
layer_test(compute_descriptor_buffer compute_descriptor_buffer)
layer_test(descriptor_buffer_double_copy descriptor_buffer_double_copy)
layer_test(descriptor_buffer_mutable_type descriptor_buffer_mutable_type)
layer_test(maintenance7 maintenance7)
layer_test(memory_2 memory_2)
layer_test(memory_2_1 memory_2_1)
layer_test(pipeline_creation_cache_control pipeline_creation_cache_control)
layer_test(stress_1 stress_1 -c 2)
layer_test(tensors_1 tensors_1)
layer_test(tensors_2 tensors_2)
layer_test(tensors_3 tensors_3)
layer_test(tensors_4 tensors_4)
layer_test(address_remap_test_1 address_remap_test_1)
layer_test(timeline_semaphore_1 timeline_semaphore_1)
layer_test(trace_helpers trace_helpers)
layer_test(updatedescriptor_1 updatedescriptor_1)
layer_test(device_generated_commands_1 device_generated_commands_1)
layer_test(nested_commandbuffers nested_commandbuffers)
#layer_test(compute_shader_module_identifier compute_shader_module_identifier)
layer_test(compute_shader_object compute_shader_object)
layer_test(demo_bloom_minimal demo_bloom_minimal)
layer_test(demo_descriptor_buffer_minimal demo_descriptor_buffer_minimal)
layer_test(demo_descriptor_indexing demo_descriptor_indexing)
layer_test(vkquake2 vkquake2)
layer_test(anki_scene2 anki_scene2)
#layer_test(display display)
#layer_test(headless_surface headless_surface) -- driver issues on nvidia
layer_test(compute_device_generated compute_device_generated)
layer_test(graphics_multi_draw graphics_multi_draw --multi-draw-mode alternate)
layer_test(descriptor_heap_1 descriptor_heap_1)
layer_test(opacity_micromap opacity_micromap)
layer_test(raytracing_indirect_noop raytracing_indirect_noop)
#layer_test(swapchain_maintenance1 swapchain_maintenance1)
layer_test(surface_maintenance1 surface_maintenance1)
#layer_test(demo_graphics_pipeline_library demo_graphics_pipeline_library) -- crashes
#layer_test(external_fence_fd_capabilities external_fence_fd_capabilities)
#layer_test(external_fence_core_capabilities external_fence_core_capabilities)
#layer_test(external_memory_dma_buf external_memory_dma_buf)
layer_test(linear_optimal_mix linear_optimal_mix)
layer_test(demo_descriptor_heap_minimal demo_descriptor_heap_minimal)
layer_test(compute_descriptor_heap compute_descriptor_heap)
layer_test(compute_bda_sc_shader_object compute_bda_sc_shader_object)
layer_test(compute_bda_sc_shader_object_multi compute_bda_sc_shader_object --multi-create-info)
layer_test(aztec_simulator_slow_compute aztec_simulator_slow_compute)
layer_test(as_populate_instance_buffer as_populate_instance_buffer)
layer_test(compute_wait_completion_event compute_wait -fb --wait-type completion-event)
add_lavatube_test(layer_test_compute_wait_completion_event_raw_screenshots
	COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/cmake/run_if_exists.sh ${CMAKE_CURRENT_BINARY_DIR}/vulkan_compute_wait_completion_event_raw.vk
		$<TARGET_FILE:lava-replay> --screenshots 0,1 --screenshot-prefix compute_wait_raw_ vulkan_compute_wait_completion_event_raw.vk)
set_tests_properties(layer_test_compute_wait_completion_event_raw_screenshots PROPERTIES
	FIXTURES_REQUIRED compute_wait_completion_event
	FIXTURES_SETUP ComputeWaitCompletionEventRawScreenshots
	SKIP_RETURN_CODE 77
	WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
add_lavatube_test(layer_test_compute_wait_completion_event_raw_frame0_red COMMAND check_png compute_wait_raw_0.png --expect-solid 255,0,0,255)
set_tests_properties(layer_test_compute_wait_completion_event_raw_frame0_red PROPERTIES
	FIXTURES_REQUIRED ComputeWaitCompletionEventRawScreenshots
	WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
add_lavatube_test(layer_test_compute_wait_completion_event_raw_frame1_green COMMAND check_png compute_wait_raw_1.png --expect-solid 0,255,0,255)
set_tests_properties(layer_test_compute_wait_completion_event_raw_frame1_green PROPERTIES
	FIXTURES_REQUIRED ComputeWaitCompletionEventRawScreenshots
	WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
layer_test(compute_wait_fence_status compute_wait -fb --wait-type fence-status)
layer_test(compute_wait_wait_fences compute_wait -fb --wait-type wait-fences)
layer_test(compute_wait_wait_semaphores compute_wait -fb --wait-type wait-semaphores)
layer_test(compute_wait_query_pool compute_wait -fb --wait-type query-pool)

# The two below will fail as we lack support for interleaved data for VK_EXT_multi_draw for now
#layer_test(graphics_multi_draw_stride16 graphics_multi_draw --multi-draw-mode indexed --stride-mode aligned16)
#layer_test(graphics_multi_draw_stride32 graphics_multi_draw --multi-draw-mode draw --stride-mode aligned32)

# --- Simulator tests ---

function(markings_test test_name)
	add_lavatube_test(markings_compare_${ARGV0}_generate COMMAND $<TARGET_FILE:lava-tool> --skip-missing-input -S ${CMAKE_BINARY_DIR}/vulkan_${ARGV0}_raw.vk ${CMAKE_BINARY_DIR}/vulkan_${ARGV0}_generated.vk)
	set_tests_properties(markings_compare_${ARGV0}_generate PROPERTIES FIXTURES_REQUIRED "${ARGV0}" FIXTURES_SETUP "${ARGV0}_generate_markings" SKIP_RETURN_CODE 77)
	add_lavatube_test(markings_compare_${ARGV0}_compare COMMAND $<TARGET_FILE:packtool> diff --assert-markings ${CMAKE_BINARY_DIR}/vulkan_${ARGV0}.vk ${CMAKE_BINARY_DIR}/vulkan_${ARGV0}_generated.vk)
	set_tests_properties(markings_compare_${ARGV0}_compare PROPERTIES FIXTURES_REQUIRED "${ARGV0}_generate_markings" SKIP_RETURN_CODE 77)
endfunction()

markings_test(raytracing_1)
markings_test(raytracing_2)
markings_test(raytracing_3)
markings_test(raytracing_4)
markings_test(raytracing_5)
markings_test(as_2)
markings_test(as_3)
markings_test(as_4)
markings_test(as_5)
markings_test(as_6)
markings_test(as_7)
markings_test(anki_scene2)
markings_test(raytracing_indirect_noop)
markings_test(compute_bda_sc)
markings_test(compute_bda_sc_multi_create_info)
markings_test(compute_bda_sc_stage_pnext)
markings_test(compute_bda_ubo)
markings_test(compute_bda_pushconstant)
markings_test(compute_bda_ubo_ssbo)
markings_test(compute_bda_copying_address_minimal)
# Known isolation case for copied buffer device address provenance: the generated
# trace currently adds one extra marking to the pre-submit buffer update.
set_tests_properties(markings_compare_compute_bda_copying_address_minimal_compare PROPERTIES WILL_FAIL TRUE)
markings_test(address_remap_test_1)
markings_test(demo_descriptor_buffer_minimal)
markings_test(descriptor_buffer_double_copy)

# --- Android emulator tracing test ---

if (ANDROID)
	add_test(NAME android_emulator_trace COMMAND ${CMAKE_SOURCE_DIR}/tests/android_emulator_trace.sh ${CMAKE_BINARY_DIR}/libVkLayer_lavatube.so $<TARGET_FILE:lava-replay> ${CMAKE_SOURCE_DIR}/VkLayer_lavatube.json)
	set_tests_properties(android_emulator_trace PROPERTIES SKIP_RETURN_CODE 77 FIXTURES_REQUIRED EmulatorFixture)
endif()

# -- cleanup --

add_test(NAME cleanup_tmp COMMAND ${CMAKE_COMMAND} -E rm -f ${CMAKE_CURRENT_BINARY_DIR}/tmp_*.vk)
set_tests_properties(cleanup_tmp PROPERTIES FIXTURES_CLEANUP cleanup_tmp)
