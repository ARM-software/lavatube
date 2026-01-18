# Running with sanitizers:
# Create a _new_ build directory and run cmake with the -DSANITIZE=<option> command line option. The various options that should work are: address, thread, undefined
# You may need to run "export ASAN_OPTIONS=detect_leaks=0" before running any of the tests with the address sanitizer.

# This needs to go before the project() call below, since it can be set by toolchain files
set(NO_XCB FALSE CACHE BOOL "Do not compile support for XCB and Xlib")
set(NO_TRACETOOLTESTS FALSE CACHE BOOL "Do not compile tracetooltests")

find_program(CCACHE_PROGRAM ccache)
if(NOT CCACHE_PROGRAM)
    message(STATUS "No ccache found")
endif()
if(CCACHE_PROGRAM AND DEFINED ENV{CCACHE_DIR})
    set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE "${CCACHE_PROGRAM}")
    message(STATUS "We will build using ccache -- very good!")
endif()

cmake_minimum_required(VERSION 3.7)
project(lavatube VERSION 0.0.1 DESCRIPTION "Vulkan tracer designed for multi-threaded replay with minimum overhead" HOMEPAGE_URL "https://github.com/ARM-software/lavatube" LANGUAGES C CXX)
enable_testing()

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

# --- common ---

find_package(TBB REQUIRED HINTS /usr/lib/x86_64-linux-gnu/cmake/TBB /usr/aarch64-linux-gnu/lib/cmake/TBB /usr/arm-linux-gnueabihf/lib/cmake/TBB)

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
set(MOST_COMMON_LIBRARIES ${SANITIZER} stdc++ dl pthread)
set(COMMON_LIBRARIES ${MOST_COMMON_LIBRARIES} TBB::tbb atomic density LZ4::LZ4 ${XCB_LIBRARIES} spirv_simulator_lib)

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
    ${PROJECT_SOURCE_DIR}/external/tracetooltests/include/vulkan_feature_detect.cpp
    ${PROJECT_SOURCE_DIR}/external/tracetooltests/include/vulkan_feature_detect.h
    ${PROJECT_SOURCE_DIR}/src/vk_wrapper.cpp
    ${PROJECT_SOURCE_DIR}/src/util.cpp
    ${PROJECT_SOURCE_DIR}/src/util.h
    ${PROJECT_SOURCE_DIR}/src/vkjson.cpp
    ${PROJECT_SOURCE_DIR}/src/vkjson.h
    ${PROJECT_SOURCE_DIR}/src/packfile.cpp
    ${PROJECT_SOURCE_DIR}/src/packfile.h
    ${PROJECT_SOURCE_DIR}/src/lavamutex.h
    ${PROJECT_SOURCE_DIR}/src/containers.h
    ${PROJECT_SOURCE_DIR}/src/sandbox.cpp
    ${PROJECT_SOURCE_DIR}/src/sandbox.h
    ${PROJECT_SOURCE_DIR}/src/json_helpers.cpp
    ${PROJECT_SOURCE_DIR}/src/json_helpers.h

    ${PROJECT_SOURCE_DIR}/src/filewriter.cpp
    ${PROJECT_SOURCE_DIR}/src/filewriter.h
    ${PROJECT_SOURCE_DIR}/src/write.cpp
    ${PROJECT_SOURCE_DIR}/src/write.h
    ${PROJECT_SOURCE_DIR}/src/rangetracking.h

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
)

set(LAVATUBE_SRC
    ${PROJECT_SOURCE_DIR}/src/window.cpp
    ${PROJECT_SOURCE_DIR}/src/window.h
    ${PROJECT_SOURCE_DIR}/src/filereader.cpp
    ${PROJECT_SOURCE_DIR}/src/filereader.h
    ${PROJECT_SOURCE_DIR}/src/read.cpp
    ${PROJECT_SOURCE_DIR}/src/read.h
    ${PROJECT_SOURCE_DIR}/src/allocators.cpp
    ${PROJECT_SOURCE_DIR}/src/allocators.h
    ${PROJECT_SOURCE_DIR}/src/suballocator.cpp
    ${PROJECT_SOURCE_DIR}/src/suballocator.h
    ${PROJECT_SOURCE_DIR}/src/memory.cpp
    ${PROJECT_SOURCE_DIR}/src/memory.h

    ${PROJECT_SOURCE_DIR}/generated/read_auto.cpp
    ${PROJECT_SOURCE_DIR}/generated/read_auto.h
)

add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/.autogenerated
    BYPRODUCTS ${SRC_ROOT}/generated/struct_read_auto.cpp ${SRC_ROOT}/generated/struct_write_auto.cpp ${SRC_ROOT}/generated/struct_read_auto.h ${SRC_ROOT}/generated/struct_write_auto.h
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
    DEPENDS ${SRC_ROOT}/scripts/lava.py ${SRC_ROOT}/external/tracetooltests/external/Vulkan-Headers/registry/vk.xml ${SRC_ROOT}/external/tracetooltests/scripts/spec.py
	${SRC_ROOT}/src/hardcode_read.cpp ${SRC_ROOT}/src/hardcode_write.cpp ${SRC_ROOT}/scripts/util.py ${SRC_ROOT}/scripts/vk_wrapper.py
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

add_executable(mprotect tests/mprotect.cpp src/util.cpp src/util.h)
target_include_directories(mprotect ${COMMON_INCLUDE})
target_link_libraries(mprotect ${MOST_COMMON_LIBRARIES} pthread)
target_compile_options(mprotect PRIVATE ${COMMON_FLAGS})

#add_executable(userfaultfd tests/userfaultfd.cpp src/util.cpp src/util.h)
#target_include_directories(userfaultfd ${COMMON_INCLUDE})
#target_link_libraries(userfaultfd ${MOST_COMMON_LIBRARIES} pthread)
#target_compile_options(userfaultfd PRIVATE ${COMMON_FLAGS})

add_executable(containers tests/container_test.cpp)
target_include_directories(containers ${COMMON_INCLUDE})
target_link_libraries(containers ${MOST_COMMON_LIBRARIES} tbb pthread)
target_compile_options(containers PRIVATE ${COMMON_FLAGS})
add_test(NAME container_test COMMAND ${CMAKE_CURRENT_BINARY_DIR}/containers)

add_executable(containers2 tests/container_test2.cpp)
target_include_directories(containers2 ${COMMON_INCLUDE})
target_link_libraries(containers2 ${MOST_COMMON_LIBRARIES} tbb pthread)
target_compile_options(containers2 PRIVATE ${COMMON_FLAGS})
add_test(NAME container_test2 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/containers2)

add_executable(containers_perf tests/container_perf.cpp)
target_include_directories(containers_perf ${COMMON_INCLUDE})
target_link_libraries(containers_perf ${MOST_COMMON_LIBRARIES} tbb pthread)
target_compile_options(containers_perf PRIVATE ${COMMON_FLAGS})

add_executable(rangetrack tests/rangetrack.cpp)
target_include_directories(rangetrack ${COMMON_INCLUDE})
target_link_libraries(rangetrack ${MOST_COMMON_LIBRARIES})
target_compile_options(rangetrack PRIVATE ${COMMON_FLAGS})
add_test(NAME rangetrack_test COMMAND ${CMAKE_CURRENT_BINARY_DIR}/rangetrack)

function(internal_test test_name)
	add_executable(${ARGV0} tests/${ARGV0}.cpp ${VULKAN_TESTS_SRC})
	target_include_directories(${ARGV0} ${COMMON_INCLUDE})
	target_link_libraries(${ARGV0} ${COMMON_LIBRARIES} lavatube)
	target_compile_options(${ARGV0} PRIVATE ${COMMON_FLAGS})
	add_dependencies(${ARGV0} sync_generated lava-replay lava-tool)
	add_test(NAME trace_test_${ARGV0} COMMAND ${CMAKE_CURRENT_BINARY_DIR}/${ARGV0})
	set_tests_properties(trace_test_${ARGV0} PROPERTIES FIXTURES_SETUP ${ARGV0})
endfunction()

internal_test(tracing1)
add_test(NAME trace_test_1_2 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/tracing1 1)
add_test(NAME trace_test_1_0_replay COMMAND ${CMAKE_CURRENT_BINARY_DIR}/lava-replay -V tracing_1_2_0.vk)
add_test(NAME trace_test_1_1_replay COMMAND ${CMAKE_CURRENT_BINARY_DIR}/lava-replay -V --no-dedicated tracing_1_2_1.vk)
add_test(NAME trace_test_1_2_replay COMMAND ${CMAKE_CURRENT_BINARY_DIR}/lava-replay -V --heap 0 tracing_1_2_1.vk)
add_test(NAME trace_test_2_1_replay COMMAND ${CMAKE_CURRENT_BINARY_DIR}/lava-replay -V -B tracing_1_2_0.vk)
add_test(NAME trace_test_2_2_replay COMMAND ${CMAKE_CURRENT_BINARY_DIR}/lava-replay -V --no-multithreaded-io tracing_1_2_0.vk)

internal_test(tracing2)
add_test(NAME trace_test_2_replay COMMAND ${CMAKE_CURRENT_BINARY_DIR}/lava-replay --heap 0 tracing_2.vk)
add_test(NAME trace_test_2_dedicbuf1 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/tracing2)
set_tests_properties(trace_test_2_dedicbuf1 PROPERTIES ENVIRONMENT "LAVATUBE_DEDICATED_BUFFER=1")
add_test(NAME trace_test_2_chunksize COMMAND ${CMAKE_CURRENT_BINARY_DIR}/tracing2)
set_tests_properties(trace_test_2_chunksize PROPERTIES ENVIRONMENT "LAVATUBE_CHUNK_SIZE=32767")
add_test(NAME trace_test_2_virtqueue COMMAND ${CMAKE_CURRENT_BINARY_DIR}/tracing2)
set_tests_properties(trace_test_2_virtqueue PROPERTIES ENVIRONMENT "LAVATUBE_VIRTUAL_QUEUES=1")
add_test(NAME trace_test_2_nompwrite COMMAND ${CMAKE_CURRENT_BINARY_DIR}/tracing2)
set_tests_properties(trace_test_2_nompwrite PROPERTIES ENVIRONMENT "LAVATUBE_DISABLE_MULTITHREADED_WRITEOUT=1")
add_test(NAME trace_test_2_nompcompress COMMAND ${CMAKE_CURRENT_BINARY_DIR}/tracing2)
set_tests_properties(trace_test_2_nompcompress PROPERTIES ENVIRONMENT "LAVATUBE_DISABLE_MULTITHREADED_COMPRESS=1")
add_test(NAME trace_test_2_debug_log COMMAND ${CMAKE_CURRENT_BINARY_DIR}/tracing2)
set_tests_properties(trace_test_2_debug_log PROPERTIES ENVIRONMENT "LAVATUBE_DEBUG=2")

internal_test(tracing3)
add_test(NAME trace_test_3_replay_mp COMMAND ${CMAKE_CURRENT_BINARY_DIR}/lava-replay -V tracing_3.vk)

internal_test(tracing4)
add_test(NAME trace_test_4_0_0 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/tracing4 -q 0 -m 0)
add_test(NAME trace_test_4_1_0 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/tracing4 -q 1 -m 0 -c 5)
add_test(NAME trace_test_4_2_0 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/tracing4 -q 2 -m 0)
add_test(NAME trace_test_4_3_0 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/tracing4 -q 3 -m 0)
add_test(NAME trace_test_4_4_0 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/tracing4 -q 4 -m 0)
add_test(NAME trace_test_4_0_1 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/tracing4 -q 0 -m 1 -c 7)
add_test(NAME trace_test_4_0_2 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/tracing4 -q 0 -m 0 -f 1)
add_test(NAME trace_test_4_0_3 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/tracing4 -q 0 -m 2 -H 0)
add_test(NAME trace_test_4_1_1 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/tracing4 -q 1 -m 1)
add_test(NAME trace_test_4_2_1 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/tracing4 -q 2 -m 1)
add_test(NAME trace_test_4_3_1 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/tracing4 -q 3 -m 1)
add_test(NAME trace_test_4_4_1 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/tracing4 -q 4 -m 1)
add_test(NAME trace_test_4_1_2 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/tracing4 -q 1 -m 2)
add_test(NAME trace_test_4_2_2 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/tracing4 -q 2 -m 2)
add_test(NAME trace_test_4_3_2 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/tracing4 -q 3 -m 2)
add_test(NAME trace_test_4_4_2 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/tracing4 -q 4 -m 2 -b 1000)
add_test(NAME trace_test_4_replay_0 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/lava-replay tracing_4_q0_m0_F0.vk)
add_test(NAME trace_test_4_replay_1 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/lava-replay tracing_4_q1_m1_F0.vk)
add_test(NAME trace_test_4_replay_2 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/lava-replay tracing_4_q2_m2_F0.vk)
add_test(NAME trace_test_4_replay_3 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/lava-replay tracing_4_q3_m0_F0.vk)
add_test(NAME trace_test_4_replay_4 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/lava-replay --no-anisotropy tracing_4_q4_m1_F0.vk)
add_test(NAME trace_test_4_replay_5 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/lava-replay -B tracing_4_q4_m1_F0.vk)
add_test(NAME trace_test_4_replay_6 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/lava-replay -w none -B tracing_4_q4_m1_F0.vk)
add_test(NAME trace_test_4_virtqueue COMMAND ${CMAKE_CURRENT_BINARY_DIR}/tracing4)
set_tests_properties(trace_test_4_virtqueue PROPERTIES ENVIRONMENT "LAVATUBE_DESTINATION=tracing_4_virtqueue;LAVATUBE_VIRTUAL_QUEUES=1")
add_test(NAME trace_test_4_virtqueue_replay COMMAND ${CMAKE_CURRENT_BINARY_DIR}/lava-replay tracing_4_virtqueue.vk)

internal_test(tracing5)
add_test(NAME trace_test_5_replay COMMAND ${CMAKE_CURRENT_BINARY_DIR}/lava-replay -V tracing_5.vk)

internal_test(tracing6)
add_test(NAME trace_test_6_replay COMMAND ${CMAKE_CURRENT_BINARY_DIR}/lava-replay -V tracing_6.vk)

internal_test(tracing7)
add_test(NAME trace_test_7_replay COMMAND ${CMAKE_CURRENT_BINARY_DIR}/lava-replay -V tracing_7.vk)

internal_test(tracing_remap)
add_test(NAME trace_test_remap_replay COMMAND ${CMAKE_CURRENT_BINARY_DIR}/lava-replay -V tracing_remap.vk)
add_test(NAME trace_test_remap_validate COMMAND ${CMAKE_CURRENT_BINARY_DIR}/lava-tool -V tracing_remap.vk)
add_test(NAME trace_test_remap_validate_remap COMMAND ${CMAKE_CURRENT_BINARY_DIR}/lava-tool -V -r tracing_remap.vk)

internal_test(tracing_buffers)
add_test(NAME trace_test_buffers_replay COMMAND ${CMAKE_CURRENT_BINARY_DIR}/lava-replay -V tracing_buffers.vk)
set_tests_properties(trace_test_buffers_replay PROPERTIES FIXTURES_REQUIRED tracing_buffers)
add_test(NAME trace_test_buffers_validate COMMAND ${CMAKE_CURRENT_BINARY_DIR}/lava-tool -V tracing_buffers.vk)
set_tests_properties(trace_test_buffers_validate PROPERTIES FIXTURES_REQUIRED tracing_buffers)

internal_test(tracing_alignment)
add_test(NAME trace_test_alignment_replay COMMAND ${CMAKE_CURRENT_BINARY_DIR}/lava-replay -V tracing_alignment.vk)
set_tests_properties(trace_test_alignment_replay PROPERTIES FIXTURES_REQUIRED tracing_alignment)
add_test(NAME trace_test_alignment_validate COMMAND ${CMAKE_CURRENT_BINARY_DIR}/lava-tool -V tracing_alignment.vk)
set_tests_properties(trace_test_alignment_validate PROPERTIES FIXTURES_REQUIRED tracing_alignment)

add_executable(write1 tests/write1.cpp)
target_include_directories(write1 ${COMMON_INCLUDE})
target_link_libraries(write1 ${COMMON_LIBRARIES} lavatube)
target_compile_options(write1 PRIVATE ${COMMON_FLAGS})
add_dependencies(write1 sync_generated)
add_test(NAME write_test_1 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/write1)

add_executable(write2 tests/write2.cpp)
target_include_directories(write2 ${COMMON_INCLUDE})
target_link_libraries(write2 ${COMMON_LIBRARIES} lavatube)
target_compile_options(write2 PRIVATE ${COMMON_FLAGS})
add_dependencies(write2 sync_generated)
add_test(NAME write_test_2 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/write2)

add_executable(write3 tests/write3.cpp)
target_include_directories(write3 ${COMMON_INCLUDE})
target_link_libraries(write3 ${COMMON_LIBRARIES} lavatube)
target_compile_options(write3 PRIVATE ${COMMON_FLAGS})
add_dependencies(write3 sync_generated)
add_test(NAME write_test_3 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/write3)

add_executable(read3 tests/read3.cpp)
target_include_directories(read3 ${COMMON_INCLUDE})
target_link_libraries(read3 ${COMMON_LIBRARIES} lavatube)
target_compile_options(read3 PRIVATE ${COMMON_FLAGS})
add_dependencies(read3 sync_generated)
add_test(NAME read_test_3 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/read3)
set_property(TEST read_test_3 APPEND PROPERTY DEPENDS write_test_3)

add_executable(write3-2 tests/write3-2.cpp)
target_include_directories(write3-2 ${COMMON_INCLUDE})
target_link_libraries(write3-2 ${COMMON_LIBRARIES} lavatube)
target_compile_options(write3-2 PRIVATE ${COMMON_FLAGS})
add_dependencies(write3-2 sync_generated)
add_test(NAME write_test_3-2 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/write3-2)

add_executable(write4 tests/write4.cpp src/filewriter.cpp src/util.cpp)
target_include_directories(write4 ${COMMON_INCLUDE})
target_link_libraries(write4 ${MOST_COMMON_LIBRARIES} density LZ4::LZ4 pthread)
target_compile_options(write4 PRIVATE ${COMMON_FLAGS})
add_dependencies(write4 sync_generated)
add_test(NAME write_test_4 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/write4)

add_executable(read4 tests/read4.cpp src/filereader.cpp src/util.cpp)
target_include_directories(read4 ${COMMON_INCLUDE})
target_link_libraries(read4 ${MOST_COMMON_LIBRARIES} density LZ4::LZ4 pthread)
target_compile_options(read4 PRIVATE ${COMMON_FLAGS})
add_dependencies(read4 sync_generated)
add_test(NAME read_test_4 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/read4)
set_property(TEST read_test_4 APPEND PROPERTY DEPENDS write_test_4)

add_executable(write5 tests/write5.cpp src/filewriter.cpp src/filereader.cpp src/util.cpp)
target_include_directories(write5 ${COMMON_INCLUDE})
target_link_libraries(write5 ${MOST_COMMON_LIBRARIES} density LZ4::LZ4 pthread)
target_compile_options(write5 PRIVATE ${COMMON_FLAGS})
add_dependencies(write5 sync_generated)
add_test(NAME write_test_5 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/write5)

# --- lava-replay ---

add_executable(lava-replay src/replay.cpp)
target_include_directories(lava-replay ${COMMON_INCLUDE})
target_link_libraries(lava-replay ${COMMON_LIBRARIES} lavatube)
target_compile_options(lava-replay PRIVATE ${COMMON_FLAGS})
add_dependencies(lava-replay sync_generated)
add_test(NAME replayer_info COMMAND ${CMAKE_CURRENT_BINARY_DIR}/lava-replay -i tracing_2.vk)
set_tests_properties(replayer_info PROPERTIES FIXTURES_REQUIRED tracing_2)
add_test(NAME replayer_vulkan_demo COMMAND ${CMAKE_CURRENT_BINARY_DIR}/lava-replay -B -w none ${PROJECT_SOURCE_DIR}/traces/demo_computeraytracing.vk)
add_test(NAME replayer_vulkan_demo2 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/lava-replay -B -w none -f 10 20 ${PROJECT_SOURCE_DIR}/traces/demo_computeraytracing.vk)
add_test(NAME replayer_vkmark COMMAND ${CMAKE_CURRENT_BINARY_DIR}/lava-replay -B -w none -f 5 9 ${PROJECT_SOURCE_DIR}/traces/vkmark_cube.vk)
add_test(NAME replayer_samples COMMAND ${CMAKE_CURRENT_BINARY_DIR}/lava-replay -B -w none -f 5 9 ${PROJECT_SOURCE_DIR}/traces/sample_hello_triangle.vk)
add_test(NAME replayer_vkcube COMMAND ${CMAKE_CURRENT_BINARY_DIR}/lava-replay -B -w none -f 5 9 ${PROJECT_SOURCE_DIR}/traces/lunarg_vkcube.vk)
install(TARGETS lava-replay RUNTIME)

# --- lava-tool ---

add_executable(lava-tool src/tool.cpp)
target_include_directories(lava-tool ${COMMON_INCLUDE})
target_link_libraries(lava-tool ${COMMON_LIBRARIES} lavatube)
target_compile_options(lava-tool PRIVATE ${COMMON_FLAGS})
add_dependencies(lava-tool sync_generated)
add_test(NAME validate_vulkan_demo COMMAND ${CMAKE_CURRENT_BINARY_DIR}/lava-tool -V ${PROJECT_SOURCE_DIR}/traces/demo_computeraytracing.vk)
add_test(NAME validate_vulkan_demo2 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/lava-tool -V -f 10 20 ${PROJECT_SOURCE_DIR}/traces/demo_computeraytracing.vk)
add_test(NAME validate_vkmark COMMAND ${CMAKE_CURRENT_BINARY_DIR}/lava-tool -V -f 5 9 ${PROJECT_SOURCE_DIR}/traces/vkmark_cube.vk)
add_test(NAME validate_samples COMMAND ${CMAKE_CURRENT_BINARY_DIR}/lava-tool -V -f 5 9 ${PROJECT_SOURCE_DIR}/traces/sample_hello_triangle.vk)
add_test(NAME validate_vkcube COMMAND ${CMAKE_CURRENT_BINARY_DIR}/lava-tool -V -f 5 9 ${PROJECT_SOURCE_DIR}/traces/lunarg_vkcube.vk)
install(TARGETS lava-tool RUNTIME)

# --- packtool ---

add_executable(packtool src/packtool.cpp
	${PROJECT_SOURCE_DIR}/external/jsoncpp/src/lib_json/json_writer.cpp
	${PROJECT_SOURCE_DIR}/external/jsoncpp/src/lib_json/json_reader.cpp
	${PROJECT_SOURCE_DIR}/external/jsoncpp/src/lib_json/json_value.cpp
	${PROJECT_SOURCE_DIR}/src/packfile.cpp
	${PROJECT_SOURCE_DIR}/src/packfile.h
	${PROJECT_SOURCE_DIR}/src/util.cpp
	${PROJECT_SOURCE_DIR}/src/util.h
)
target_include_directories(packtool ${COMMON_INCLUDE})
target_link_libraries(packtool ${MOST_COMMON_LIBRARIES})
target_compile_options(packtool PRIVATE ${COMMON_FLAGS} -fPIC)
add_test(NAME pack_test_1 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/packtool) # usage
set_tests_properties(pack_test_1 PROPERTIES FIXTURES_REQUIRED tracing_2)
add_test(NAME pack_test_2 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/packtool extract debug.json tracing_2.vk)
set_tests_properties(pack_test_2 PROPERTIES FIXTURES_REQUIRED tracing_2)
add_test(NAME pack_test_3_1 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/packtool print tracking.json tracing_2.vk)
set_tests_properties(pack_test_3_1 PROPERTIES FIXTURES_REQUIRED tracing_2)
add_test(NAME pack_test_3_2 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/packtool extract thread_0.bin tracing_2.vk)
set_tests_properties(pack_test_3_2 PROPERTIES FIXTURES_REQUIRED tracing_2)
add_test(NAME pack_test_4 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/packtool unpack tmp_2 tracing_2.vk)
set_tests_properties(pack_test_4 PROPERTIES FIXTURES_REQUIRED tracing_2)
add_test(NAME pack_test_4_1 COMMAND cmp ${CMAKE_CURRENT_BINARY_DIR}/tmp_2/debug.json ${CMAKE_CURRENT_BINARY_DIR}/debug.json)
set_tests_properties(pack_test_4_1 PROPERTIES FIXTURES_REQUIRED tracing_2)
add_test(NAME pack_test_4_2 COMMAND cmp ${CMAKE_CURRENT_BINARY_DIR}/tmp_2/thread_0.bin ${CMAKE_CURRENT_BINARY_DIR}/thread_0.bin)
set_tests_properties(pack_test_4_2 PROPERTIES FIXTURES_REQUIRED tracing_2)
add_test(NAME pack_test_5 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/packtool list tracing_2.vk)
set_tests_properties(pack_test_5 PROPERTIES FIXTURES_REQUIRED tracing_2)
add_test(NAME pack_test_6 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/packtool check tracing_2.vk)
set_tests_properties(pack_test_6 PROPERTIES FIXTURES_REQUIRED tracing_2)
install(TARGETS packtool RUNTIME)

# --- layer tests ---

function(layer_test test_name test_exe)
	add_test(NAME layer_test_${ARGV0} COMMAND ${CMAKE_CURRENT_BINARY_DIR}/tracetooltests/vulkan_${ARGV1} --gpu ${ARGV2} ${ARGV3} ${ARGV4} ${ARGV5} ${ARGV6} ${ARGV7})
	set_tests_properties(layer_test_${ARGV0} PROPERTIES
		FIXTURES_SETUP ${ARGV0}
		ENVIRONMENT "LAVATUBE_VIRTUAL_QUEUES=1;LAVATUBE_DESTINATION=vulkan_${ARGV0};VK_LAYER_PATH=${CMAKE_CURRENT_BINARY_DIR}/implicit_layer.d;VK_INSTANCE_LAYERS=VK_LAYER_ARM_lavatube;LD_LIBRARY_PATH=${CMAKE_CURRENT_BINARY_DIR}/implicit_layer.d"
		SKIP_RETURN_CODE 77
	)
	add_test(NAME layer_test_${ARGV0}_simulation COMMAND ${CMAKE_CURRENT_BINARY_DIR}/tracetooltests/vulkan_${ARGV1} --cpu ${ARGV2} ${ARGV3} ${ARGV4} ${ARGV5} ${ARGV6} ${ARGV7})
	set_tests_properties(layer_test_${ARGV0}_simulation PROPERTIES
		FIXTURES_SETUP ${ARGV0}_simulation
		ENVIRONMENT "LAVATUBE_VIRTUAL_QUEUES=1;LAVATUBE_DESTINATION=vulkan_${ARGV0}_simulation;VK_LAYER_PATH=${CMAKE_CURRENT_BINARY_DIR}/implicit_layer.d;VK_INSTANCE_LAYERS=VK_LAYER_ARM_lavatube;LD_LIBRARY_PATH=${CMAKE_CURRENT_BINARY_DIR}/implicit_layer.d"
		SKIP_RETURN_CODE 77
	)
	add_test(NAME layer_test_${ARGV0}_validate COMMAND ${CMAKE_CURRENT_BINARY_DIR}/lava-tool -V vulkan_${ARGV0}.vk)
	set_tests_properties(layer_test_${ARGV0}_validate PROPERTIES FIXTURES_REQUIRED ${ARGV0})
	add_test(NAME layer_test_${ARGV0}_replay COMMAND ${CMAKE_CURRENT_BINARY_DIR}/lava-replay -V vulkan_${ARGV0}.vk)
	set_tests_properties(layer_test_${ARGV0}_replay PROPERTIES FIXTURES_REQUIRED ${ARGV0})
	add_test(NAME layer_test_${ARGV0}_replay_blackhole COMMAND ${CMAKE_CURRENT_BINARY_DIR}/lava-replay -V -B vulkan_${ARGV0}.vk)
	set_tests_properties(layer_test_${ARGV0}_replay_blackhole PROPERTIES FIXTURES_REQUIRED ${ARGV0})
	add_test(NAME layer_test_${ARGV0}_replay_blackhole_nonewsi COMMAND ${CMAKE_CURRENT_BINARY_DIR}/lava-replay -V -B -w none vulkan_${ARGV0}.vk)
	set_tests_properties(layer_test_${ARGV0}_replay_blackhole_nonewsi PROPERTIES FIXTURES_REQUIRED ${ARGV0})
	add_test(NAME layer_test_${ARGV0}_replay_nonewsi COMMAND ${CMAKE_CURRENT_BINARY_DIR}/lava-replay -V -w none vulkan_${ARGV0}.vk)
	set_tests_properties(layer_test_${ARGV0}_replay_nonewsi PROPERTIES FIXTURES_REQUIRED ${ARGV0})
	add_test(NAME layer_test_${ARGV0}_replay_from_simulation COMMAND ${CMAKE_CURRENT_BINARY_DIR}/lava-replay -V vulkan_${ARGV0}_simulation.vk)
	set_tests_properties(layer_test_${ARGV0}_replay_from_simulation PROPERTIES FIXTURES_REQUIRED "${ARGV0}_simulation;${ARGV0}")
endfunction()

layer_test(general general)
layer_test(fence_delay fence_delay)
# TBD : Fence delay tests need to be fixed since they were rewritten in tracetooltests for gfxr (and now is in its own test file)
#add_test(NAME layer_test_fence_delay_ COMMAND ${CMAKE_CURRENT_BINARY_DIR}/tracetooltests/vulkan_general -f 1)
#set_tests_properties(layer_test_general_fencedelay PROPERTIES ENVIRONMENT "VK_LAYER_PATH=${CMAKE_CURRENT_BINARY_DIR}/implicit_layer.d;VK_INSTANCE_LAYERS=VK_LAYER_ARM_lavatube;LD_LIBRARY_PATH=${CMAKE_CURRENT_BINARY_DIR}/implicit_layer.d;LAVATUBE_DELAY_FENCE_SUCCESS_FRAMES=2;")
#add_test(NAME script_test_general COMMAND ${PROJECT_SOURCE_DIR}/scripts/lava-capture.py -o script_test_general.vk --layer-path ${CMAKE_CURRENT_BINARY_DIR}/implicit_layer.d ${CMAKE_CURRENT_BINARY_DIR}/tracetooltests/vulkan_general -f 0 -V 2)
#add_test(NAME script_test_general_fencedelay COMMAND ${PROJECT_SOURCE_DIR}/scripts/lava-capture.py -o script_test_general_fencedelay.vk --layer-path ${CMAKE_CURRENT_BINARY_DIR}/implicit_layer.d --delayfence 2 ${CMAKE_CURRENT_BINARY_DIR}/tracetooltests/vulkan_general -f 1)
add_test(NAME script_test_copying_1 COMMAND ${PROJECT_SOURCE_DIR}/scripts/lava-capture.py -o script_test_copying_1.vk --dedicated-buffer --gpu 0 --layer-path ${CMAKE_CURRENT_BINARY_DIR}/implicit_layer.d ${CMAKE_CURRENT_BINARY_DIR}/tracetooltests/vulkan_copying_1 -V 2)
add_test(NAME script_test_copying_nomp COMMAND ${PROJECT_SOURCE_DIR}/scripts/lava-capture.py -o script_test_copying_nomp.vk --no-multithread --automate --layer-path ${CMAKE_CURRENT_BINARY_DIR}/implicit_layer.d ${CMAKE_CURRENT_BINARY_DIR}/tracetooltests/vulkan_copying_1 -V 3)
layer_test(general_vulkan13 general -V 3)
layer_test(copying_1 copying_1)
layer_test(copying_1_q1 copying_1 -q 1)
layer_test(copying_1_q2 copying_1 -q 2)
layer_test(copying_1_q3 copying_1 -q 3)
layer_test(copying_1_q4 copying_1 -q 4)
layer_test(copying_1_q5 copying_1 -q 5)
layer_test(copying_1_m1 copying_1 -m 1)
layer_test(copying_1_m2 copying_1 -m 2)
layer_test(copying_1_bda copying_1 -B -D -V 2)
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
layer_test(compute_bda_pushconstant compute_bda_pushconstant)
layer_test(compute_bda_ubo compute_bda_ubo)
layer_test(compute_bda_ubo_ssbo compute_bda_ubo --ssbo)
layer_test(compute_bda_ubo_fb compute_bda_ubo --frame-boundary)
add_test(NAME layer_test_thread_3_replay_blackhole_stress_1 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/lava-replay -V -B vulkan_thread_3.vk)
set_tests_properties(layer_test_thread_3_replay_blackhole_stress_1 PROPERTIES FIXTURES_REQUIRED thread_3)
add_test(NAME layer_test_thread_3_replay_blackhole_stress_2 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/lava-replay --no-dedicated -B vulkan_thread_3.vk)
set_tests_properties(layer_test_thread_3_replay_blackhole_stress_2 PROPERTIES FIXTURES_REQUIRED thread_3)
add_test(NAME layer_test_thread_3_replay_blackhole_stress_3 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/lava-replay -B vulkan_thread_3.vk)
set_tests_properties(layer_test_thread_3_replay_blackhole_stress_3 PROPERTIES FIXTURES_REQUIRED thread_3)
add_test(NAME layer_test_thread_3_replay_blackhole_stress_4 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/lava-replay -B vulkan_thread_3.vk)
set_tests_properties(layer_test_thread_3_replay_blackhole_stress_4 PROPERTIES FIXTURES_REQUIRED thread_3)
add_test(NAME layer_test_thread_3_replay_blackhole_stress_5 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/lava-replay -B vulkan_thread_3.vk)
set_tests_properties(layer_test_thread_3_replay_blackhole_stress_5 PROPERTIES FIXTURES_REQUIRED thread_3)
add_test(NAME layer_test_thread_3_replay_blackhole_stress_6 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/lava-replay -B vulkan_thread_3.vk)
set_tests_properties(layer_test_thread_3_replay_blackhole_stress_6 PROPERTIES FIXTURES_REQUIRED thread_3)
add_test(NAME layer_test_thread_3_replay_blackhole_stress_7 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/lava-replay -B vulkan_thread_3.vk)
set_tests_properties(layer_test_thread_3_replay_blackhole_stress_7 PROPERTIES FIXTURES_REQUIRED thread_3)
add_test(NAME layer_test_thread_3_replay_blackhole_stress_8 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/lava-replay -B vulkan_thread_3.vk)
set_tests_properties(layer_test_thread_3_replay_blackhole_stress_8 PROPERTIES FIXTURES_REQUIRED thread_3)
add_test(NAME layer_test_thread_3_replay_blackhole_stress_9 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/lava-replay -B vulkan_thread_3.vk)
set_tests_properties(layer_test_thread_3_replay_blackhole_stress_9 PROPERTIES FIXTURES_REQUIRED thread_3)
layer_test(compute_1 compute_1)
layer_test(compute_1_indirect compute_1 --indirect)
layer_test(compute_1_indirect_offset compute_1 --indirect -ioff 9)
layer_test(compute_1_fb compute_1 --frame-boundary)
add_test(NAME layer_test_compute_1_1 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/tracetooltests/vulkan_compute_1 -pc)
set_tests_properties(layer_test_compute_1_1 PROPERTIES ENVIRONMENT "VK_LAYER_PATH=${CMAKE_CURRENT_BINARY_DIR}/implicit_layer.d;VK_INSTANCE_LAYERS=VK_LAYER_ARM_lavatube;LD_LIBRARY_PATH=${CMAKE_CURRENT_BINARY_DIR}/implicit_layer.d;")
add_test(NAME layer_test_compute_1_2 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/tracetooltests/vulkan_compute_1 -pc -pcf test.bin)
set_tests_properties(layer_test_compute_1_1 PROPERTIES ENVIRONMENT "VK_LAYER_PATH=${CMAKE_CURRENT_BINARY_DIR}/implicit_layer.d;VK_INSTANCE_LAYERS=VK_LAYER_ARM_lavatube;LD_LIBRARY_PATH=${CMAKE_CURRENT_BINARY_DIR}/implicit_layer.d;")
add_test(NAME layer_test_compute_1_3 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/tracetooltests/vulkan_compute_1 -pc -pcf test.bin)
set_tests_properties(layer_test_compute_1_1 PROPERTIES ENVIRONMENT "VK_LAYER_PATH=${CMAKE_CURRENT_BINARY_DIR}/implicit_layer.d;VK_INSTANCE_LAYERS=VK_LAYER_ARM_lavatube;LD_LIBRARY_PATH=${CMAKE_CURRENT_BINARY_DIR}/implicit_layer.d;")
add_custom_command(TARGET packtool PRE_BUILD COMMAND rm -f test.bin)
layer_test(compute_2 compute_2)
layer_test(compute_2_2 compute_2 -j 1 -s 2)
layer_test(compute_3 compute_3)
layer_test(feature_1 feature_1)
layer_test(feature_2 feature_2)
layer_test(tool_1 tool_1)
layer_test(debug_marker debug_marker)
layer_test(debug_report debug_report)
layer_test(deferred_1 deferred_1)
layer_test(pipelinecache_1 pipelinecache_1)
layer_test(pnext_chain pnext_chain)
#layer_test(as_1 as_1)
#layer_test(as_2 as_2)
#layer_test(as_3 as_3)
#layer_test(as_4 as_4) # crashes
#layer_test(as_5 as_5) # crashes
#layer_test(aliasing_1 aliasing_1) # TBD need to implement aliasing support
layer_test(privatedata privatedata)
layer_test(privatedataext privatedataext)
#layer_test(multidevice_1 multidevice_1) # assert failure on memory type
#layer_test(multiinstance multiinstance)
layer_test(mesh_1 mesh_1)
layer_test(graphics_1 graphics_1)
layer_test(pipeline_binary compute_2 -b)
layer_test(pipeline_executable_properties pipeline_executable_properties)
#layer_test(cooperative_matrix cooperative_matrix)
layer_test(extended_dynamic_state3 extended_dynamic_state3)
#layer_test(push_descriptor)
layer_test(host_image_copy host_image_copy)
