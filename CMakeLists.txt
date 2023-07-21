# Running with sanitizers:
# Create a _new_ build directory and run cmake with the -DSANITIZE=<option> command line option. The various options that should work are: address, thread, undefined
# You may need to run "export ASAN_OPTIONS=detect_leaks=0" before running any of the tests with the address sanitizer.

# This needs to go before the project() call below, since it can be set by toolchain files
set(NO_XCB FALSE CACHE BOOL "Do not compile support for XCB and Xlib")
set(NO_TRACETOOLTESTS FALSE CACHE BOOL "Do not compile tracetooltests")

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM AND DEFINED ENV{CCACHE_DIR})
	set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE "${CCACHE_PROGRAM}")
	message(STATUS "We will build using ccache -- very good!")
endif()

cmake_minimum_required(VERSION 3.7)
project(lavatube VERSION 0.0.1)
enable_testing()

set(SRC_ROOT ${CMAKE_SOURCE_DIR})
set(THIRD_PARTY_DIR ${SRC_ROOT}/external)
set(PYTHON_EXECUTABLE python2)
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake")
set(LAYER_NAME VkLayer_lavatube)
find_package(PkgConfig REQUIRED)

if(NOT ${NO_XCB})
    find_package(XCB REQUIRED)
    find_package(X11 REQUIRED)
    set(XCB_LIBRARIES ${XCB_LIBRARIES} ${X11_LIBRARIES})
    message(STATUS "XCB and X11 enabled, using ${XCB_LIBRARIES} ${X11_LIBRARIES}")
    set(XCB_ENABLE -DVK_USE_PLATFORM_XCB_KHR -DVK_USE_PLATFORM_XLIB_KHR)
else()
    set(XCB_ENABLE "")
    message(STATUS "XCB and X11 support not added")
endif()

if(NOT EXISTS "${CMAKE_SOURCE_DIR}/external/Vulkan-Headers/.git")
    message(SEND_ERROR "git submodules not available. Please run: git submodule update --init --recursive")
endif()

if(SANITIZE)
    set(SANITIZER -fsanitize=${SANITIZE} -fsanitize-undefined-trap-on-error -fuse-ld=gold)
endif()

# --- tracetooltests ---

if(NOT ${NO_TRACETOOLTESTS})
    message(STATUS "Building tracetooltests")
    set(NO_GLES 1)
    add_subdirectory(external/tracetooltests ${CMAKE_BINARY_DIR}/tracetooltests)
else()
    message(STATUS "Asked to skip tracetooltests")
endif()

# --- density ---

set(DENSITY_SOURCES
    ${CMAKE_SOURCE_DIR}/external/density/src/globals.c
    ${CMAKE_SOURCE_DIR}/external/density/src/buffers/buffer.c
    ${CMAKE_SOURCE_DIR}/external/density/src/structure/header.c
    ${CMAKE_SOURCE_DIR}/external/density/src/algorithms/dictionaries.c
    ${CMAKE_SOURCE_DIR}/external/density/src/algorithms/algorithms.c
    ${CMAKE_SOURCE_DIR}/external/density/src/algorithms/cheetah/core/cheetah_decode.c
    ${CMAKE_SOURCE_DIR}/external/density/src/algorithms/cheetah/core/cheetah_encode.c
    ${CMAKE_SOURCE_DIR}/external/density/src/algorithms/chameleon/core/chameleon_decode.c
    ${CMAKE_SOURCE_DIR}/external/density/src/algorithms/chameleon/core/chameleon_encode.c
    ${CMAKE_SOURCE_DIR}/external/density/src/algorithms/lion/forms/lion_form_model.c
    ${CMAKE_SOURCE_DIR}/external/density/src/algorithms/lion/core/lion_decode.c
    ${CMAKE_SOURCE_DIR}/external/density/src/algorithms/lion/core/lion_encode.c
    ${CMAKE_SOURCE_DIR}/external/density/benchmark/src/benchmark.c)

add_library(density STATIC ${DENSITY_SOURCES})
target_compile_options(density PRIVATE -std=c99 -Wall -O2 -ggdb -Wno-unused-variable -Werror -fno-strict-aliasing -fvisibility=hidden -fPIC ${SANITIZER})
target_link_libraries(density ${SANITIZER})

# --- common ---

find_package(TBB REQUIRED HINTS /usr/lib/x86_64-linux-gnu/cmake/TBB /usr/aarch64-linux-gnu/lib/cmake/TBB /usr/arm-linux-gnueabihf/lib/cmake/TBB)

# Debug Release RelWithDebInfo MinSizeRel
set(COMMON_FLAGS ${SANITIZER} -std=c++17 -Werror -Wall -fvisibility=hidden ${XCB_ENABLE} -Wshadow)
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
	set(COMMON_FLAGS ${COMMON_FLAGS} -Wthread-safety -Wno-error=thread-safety-analysis)
endif()
set(MOST_COMMON_LIBRARIES ${SANITIZER} stdc++ dl pthread)
set(COMMON_LIBRARIES ${MOST_COMMON_LIBRARIES} TBB::tbb atomic density ${XCB_LIBRARIES})

set(COMMON_INCLUDE
    PUBLIC ${SRC_ROOT}
    PUBLIC ${SRC_ROOT}/src
    PUBLIC ${SRC_ROOT}/include
    PUBLIC ${SRC_ROOT}/generated
    PUBLIC ${SRC_ROOT}/external
    PUBLIC ${SRC_ROOT}/external/tracetooltests/include
    PUBLIC ${SRC_ROOT}/external/Vulkan-Headers/include
    PRIVATE ${SRC_ROOT}/external/jsoncpp
)

set(COMMON_SRC
    ${CMAKE_SOURCE_DIR}/external/jsoncpp/src/lib_json/json_writer.cpp
    ${CMAKE_SOURCE_DIR}/external/jsoncpp/src/lib_json/json_reader.cpp
    ${CMAKE_SOURCE_DIR}/external/jsoncpp/src/lib_json/json_value.cpp
    ${CMAKE_SOURCE_DIR}/include/feature_detect.h
    ${CMAKE_SOURCE_DIR}/src/vk_wrapper.cpp
    ${CMAKE_SOURCE_DIR}/src/util.cpp
    ${CMAKE_SOURCE_DIR}/src/util.h
    ${CMAKE_SOURCE_DIR}/src/vkjson.cpp
    ${CMAKE_SOURCE_DIR}/src/vkjson.h
    ${CMAKE_SOURCE_DIR}/src/packfile.cpp
    ${CMAKE_SOURCE_DIR}/src/packfile.h
    ${CMAKE_SOURCE_DIR}/src/lavamutex.h
    ${CMAKE_SOURCE_DIR}/src/containers.h
    ${CMAKE_SOURCE_DIR}/generated/util_auto.cpp
    ${CMAKE_SOURCE_DIR}/generated/util_auto.h
    ${CMAKE_SOURCE_DIR}/generated/tostring.h
    ${CMAKE_SOURCE_DIR}/generated/tostring.cpp
)

set(VULKAN_TESTS_SRC
   ${CMAKE_SOURCE_DIR}/tests/tests.h
   ${CMAKE_SOURCE_DIR}/tests/common.cpp
   ${CMAKE_SOURCE_DIR}/tests/common.h
)

set(READ_SRC
    ${CMAKE_SOURCE_DIR}/src/util.cpp
    ${CMAKE_SOURCE_DIR}/src/util.h
    ${CMAKE_SOURCE_DIR}/src/window.cpp
    ${CMAKE_SOURCE_DIR}/src/window.h
    ${CMAKE_SOURCE_DIR}/src/filereader.cpp
    ${CMAKE_SOURCE_DIR}/src/filereader.h
    ${CMAKE_SOURCE_DIR}/src/read.cpp
    ${CMAKE_SOURCE_DIR}/src/read.h
    ${CMAKE_SOURCE_DIR}/src/allocators.cpp
    ${CMAKE_SOURCE_DIR}/src/allocators.h
    ${CMAKE_SOURCE_DIR}/src/suballocator.cpp
    ${CMAKE_SOURCE_DIR}/src/suballocator.h
    ${CMAKE_SOURCE_DIR}/generated/vk_wrapper_auto.cpp
    ${CMAKE_SOURCE_DIR}/generated/vk_wrapper_auto.h
    ${CMAKE_SOURCE_DIR}/generated/read_auto.cpp
    ${CMAKE_SOURCE_DIR}/generated/read_auto.h
)

set(WRITE_SRC_COMMON
    ${CMAKE_SOURCE_DIR}/src/util.cpp
    ${CMAKE_SOURCE_DIR}/src/util.h
    ${CMAKE_SOURCE_DIR}/src/filewriter.cpp
    ${CMAKE_SOURCE_DIR}/src/filewriter.h
    ${CMAKE_SOURCE_DIR}/src/write.cpp
    ${CMAKE_SOURCE_DIR}/src/write.h
    ${CMAKE_SOURCE_DIR}/src/rangetracking.h
)
set(WRITE_SRC
    ${WRITE_SRC_COMMON}
    ${CMAKE_SOURCE_DIR}/generated/vk_wrapper_auto.cpp
    ${CMAKE_SOURCE_DIR}/generated/vk_wrapper_auto.h
    ${CMAKE_SOURCE_DIR}/generated/write_auto.cpp
    ${CMAKE_SOURCE_DIR}/generated/write_auto.h
    ${CMAKE_SOURCE_DIR}/generated/write_resource_auto.cpp
    ${CMAKE_SOURCE_DIR}/generated/write_resource_auto.h
)

file(MAKE_DIRECTORY generated)
add_custom_command(
    OUTPUT .autogenerated
    BYPRODUCTS ${SRC_ROOT}/generated/struct_read_auto.cpp ${SRC_ROOT}/generated/struct_write_auto.cpp ${SRC_ROOT}/generated/struct_read_auto.h ${SRC_ROOT}/generated/struct_write_auto.h
           ${SRC_ROOT}/generated/read_auto.cpp ${SRC_ROOT}/generated/write_auto.cpp ${SRC_ROOT}/generated/read_auto.h ${SRC_ROOT}/generated/write_auto.h ${SRC_ROOT}/generated/util_auto.cpp
           ${SRC_ROOT}/generated/util_auto.h ${SRC_ROOT}/generated/vk_wrapper_auto.h ${SRC_ROOT}/generated/vk_wrapper_auto.cpp
           ${SRC_ROOT}/generated/write_resource_auto.cpp ${SRC_ROOT}/generated/write_resource_auto.h ${SRC_ROOT}/generated/tostring.cpp ${SRC_ROOT}/generated/tostring.h
    COMMAND ${PYTHON_EXECUTABLE} ${SRC_ROOT}/scripts/struct.py
    COMMAND ${PYTHON_EXECUTABLE} ${SRC_ROOT}/scripts/lava.py
    COMMAND ${PYTHON_EXECUTABLE} ${SRC_ROOT}/scripts/vk_wrapper.py
    COMMAND ${PYTHON_EXECUTABLE} ${SRC_ROOT}/scripts/tostring.py
    COMMAND sync
    COMMAND touch .autogenerated
    WORKING_DIRECTORY ${SRC_ROOT}
    DEPENDS ${SRC_ROOT}/scripts/lava.py ${SRC_ROOT}/external/Vulkan-Headers/registry/vk.xml ${SRC_ROOT}/external/tracetooltests/scripts/spec.py ${SRC_ROOT}/src/hardcode_read.cpp
	${SRC_ROOT}/src/hardcode_write.cpp ${SRC_ROOT}/scripts/util.py ${SRC_ROOT}/external/Vulkan-Headers/include/vulkan/vk_layer.h ${SRC_ROOT}/scripts/vk_wrapper.py
)

# The below is necessary because cmake is too braindead to realize we should not run the above once per target
add_custom_target(sync_stuff DEPENDS .autogenerated)

add_library(lavatube STATIC ${COMMON_SRC})
target_include_directories(lavatube ${COMMON_INCLUDE})
target_link_libraries(lavatube ${COMMON_LIBRARIES})
target_compile_options(lavatube PRIVATE ${COMMON_FLAGS} -fPIC)
add_dependencies(lavatube sync_stuff)

# --- tracer layer ---

add_library(VkLayer_lavatube SHARED ${WRITE_SRC} ${CMAKE_SOURCE_DIR}/src/vulkan_layer.cpp)
target_include_directories(VkLayer_lavatube ${COMMON_INCLUDE})
target_link_libraries(VkLayer_lavatube ${COMMON_LIBRARIES} lavatube)
target_compile_options(VkLayer_lavatube PRIVATE ${COMMON_FLAGS})
target_compile_definitions(VkLayer_lavatube PUBLIC COMPILE_LAYER)
add_custom_command(TARGET VkLayer_lavatube POST_BUILD COMMAND mkdir -p implicit_layer.d)
add_custom_command(TARGET VkLayer_lavatube POST_BUILD COMMAND mv libVkLayer_lavatube.so implicit_layer.d/libVkLayer_lavatube.so)
add_custom_command(TARGET VkLayer_lavatube POST_BUILD COMMAND cp ${SRC_ROOT}/VkLayer_lavatube.json implicit_layer.d/VkLayer_lavatube.json)
add_dependencies(VkLayer_lavatube sync_stuff)
install(TARGETS VkLayer_lavatube LIBRARY)
install(PROGRAMS scripts/lava-capture.py TYPE BIN)

# --- unit tests ---

add_executable(featuretest tests/feature.cpp include/feature_detect.h src/util.cpp src/util.h)
target_include_directories(featuretest ${COMMON_INCLUDE})
target_link_libraries(featuretest ${MOST_COMMON_LIBRARIES} pthread)
target_compile_options(featuretest PRIVATE ${COMMON_FLAGS})
add_test(NAME feature_test COMMAND ${CMAKE_CURRENT_BINARY_DIR}/featuretest)

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

add_executable(tracing1 tests/tracing1.cpp ${WRITE_SRC} ${READ_SRC} ${VULKAN_TESTS_SRC})
target_include_directories(tracing1 ${COMMON_INCLUDE})
target_link_libraries(tracing1 ${COMMON_LIBRARIES} lavatube)
target_compile_options(tracing1 PRIVATE ${COMMON_FLAGS})
add_dependencies(tracing1 sync_stuff)
add_test(NAME trace_test_1_1 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/tracing1 0)
add_test(NAME trace_test_1_2 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/tracing1 1)
add_test(NAME trace_test_1_0_replay COMMAND ${CMAKE_CURRENT_BINARY_DIR}/replay tracing_1_2_0.vk)
add_test(NAME trace_test_1_1_replay COMMAND ${CMAKE_CURRENT_BINARY_DIR}/replay -D tracing_1_2_1.vk)
add_test(NAME trace_test_1_2_replay COMMAND ${CMAKE_CURRENT_BINARY_DIR}/replay -H 0 tracing_1_2_1.vk)
add_test(NAME trace_test_2_1_replay COMMAND ${CMAKE_CURRENT_BINARY_DIR}/replay -B tracing_1_2_0.vk)

add_executable(tracing2 tests/tracing2.cpp ${WRITE_SRC} ${READ_SRC} ${VULKAN_TESTS_SRC})
target_include_directories(tracing2 ${COMMON_INCLUDE})
target_link_libraries(tracing2 ${COMMON_LIBRARIES} lavatube)
target_compile_options(tracing2 PRIVATE ${COMMON_FLAGS})
add_dependencies(tracing2 sync_stuff replay)
add_test(NAME trace_test_2 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/tracing2)
set_tests_properties(trace_test_2 PROPERTIES FIXTURES_SETUP tracing_2)
add_test(NAME trace_test_2_replay COMMAND ${CMAKE_CURRENT_BINARY_DIR}/replay -H 0 tracing_2.vk)
add_test(NAME trace_test_2_dedicbuf1 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/tracing2)
set_tests_properties(trace_test_2_dedicbuf1 PROPERTIES ENVIRONMENT "LAVATUBE_DEDICATED_BUFFER=1")
add_test(NAME trace_test_2_chunksize COMMAND ${CMAKE_CURRENT_BINARY_DIR}/tracing2)
set_tests_properties(trace_test_2_chunksize PROPERTIES ENVIRONMENT "LAVATUBE_CHUNK_SIZE=32767")

add_executable(tracing3 tests/tracing3.cpp ${WRITE_SRC} ${READ_SRC} ${VULKAN_TESTS_SRC})
target_include_directories(tracing3 ${COMMON_INCLUDE})
target_link_libraries(tracing3 ${COMMON_LIBRARIES} lavatube)
target_compile_options(tracing3 PRIVATE ${COMMON_FLAGS})
add_dependencies(tracing3 sync_stuff replay)
add_test(NAME trace_test_3 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/tracing3)
add_test(NAME trace_test_3_replay_mp COMMAND ${CMAKE_CURRENT_BINARY_DIR}/replay tracing_3.vk)

add_executable(tracing4 tests/tracing4.cpp ${WRITE_SRC} ${READ_SRC} ${VULKAN_TESTS_SRC})
target_include_directories(tracing4 ${COMMON_INCLUDE})
target_link_libraries(tracing4 ${COMMON_LIBRARIES} lavatube)
target_compile_options(tracing4 PRIVATE ${COMMON_FLAGS})
add_dependencies(tracing4 sync_stuff replay)
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
add_test(NAME trace_test_4_replay_0 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/replay tracing_4_q0_m0_F0.vk)
add_test(NAME trace_test_4_replay_1 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/replay tracing_4_q1_m1_F0.vk)
add_test(NAME trace_test_4_replay_2 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/replay tracing_4_q2_m2_F0.vk)
add_test(NAME trace_test_4_replay_3 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/replay tracing_4_q3_m0_F0.vk)
add_test(NAME trace_test_4_replay_4 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/replay -N tracing_4_q4_m1_F0.vk)
add_test(NAME trace_test_4_replay_5 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/replay -B tracing_4_q4_m1_F0.vk)
add_test(NAME trace_test_4_replay_6 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/replay -v -w none -B tracing_4_q4_m1_F0.vk)

add_executable(tracing5 tests/tracing5.cpp ${WRITE_SRC} ${READ_SRC} ${VULKAN_TESTS_SRC})
target_include_directories(tracing5 ${COMMON_INCLUDE})
target_link_libraries(tracing5 ${COMMON_LIBRARIES} lavatube)
target_compile_options(tracing5 PRIVATE ${COMMON_FLAGS})
add_dependencies(tracing5 sync_stuff replay)
add_test(NAME trace_test_5 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/tracing5)
add_test(NAME trace_test_5_replay COMMAND ${CMAKE_CURRENT_BINARY_DIR}/replay tracing_5.vk)

add_executable(tracing6 tests/tracing6.cpp ${WRITE_SRC} ${READ_SRC} ${VULKAN_TESTS_SRC})
target_include_directories(tracing6 ${COMMON_INCLUDE})
target_link_libraries(tracing6 ${COMMON_LIBRARIES} lavatube)
target_compile_options(tracing6 PRIVATE ${COMMON_FLAGS})
add_dependencies(tracing6 sync_stuff replay)
add_test(NAME trace_test_6 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/tracing6)
add_test(NAME trace_test_6_replay COMMAND ${CMAKE_CURRENT_BINARY_DIR}/replay tracing_6.vk)

add_executable(write1 tests/write1.cpp ${WRITE_SRC} ${READ_SRC})
target_include_directories(write1 ${COMMON_INCLUDE})
target_link_libraries(write1 ${COMMON_LIBRARIES} lavatube)
target_compile_options(write1 PRIVATE ${COMMON_FLAGS})
add_dependencies(write1 sync_stuff)
add_test(NAME write_test_1 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/write1)

add_executable(write2 tests/write2.cpp ${WRITE_SRC} ${READ_SRC})
target_include_directories(write2 ${COMMON_INCLUDE})
target_link_libraries(write2 ${COMMON_LIBRARIES} lavatube)
target_compile_options(write2 PRIVATE ${COMMON_FLAGS})
add_dependencies(write2 sync_stuff)
add_test(NAME write_test_2 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/write2)

add_executable(write3 tests/write3.cpp ${WRITE_SRC})
target_include_directories(write3 ${COMMON_INCLUDE})
target_link_libraries(write3 ${COMMON_LIBRARIES} lavatube)
target_compile_options(write3 PRIVATE ${COMMON_FLAGS})
add_dependencies(write3 sync_stuff)
add_test(NAME write_test_3 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/write3)

add_executable(read3 tests/read3.cpp ${READ_SRC})
target_include_directories(read3 ${COMMON_INCLUDE})
target_link_libraries(read3 ${COMMON_LIBRARIES} lavatube)
target_compile_options(read3 PRIVATE ${COMMON_FLAGS})
add_dependencies(read3 sync_stuff)
add_test(NAME read_test_3 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/read3)
set_property(TEST read_test_3 APPEND PROPERTY DEPENDS write_test_3)

add_executable(write3-2 tests/write3-2.cpp ${WRITE_SRC})
target_include_directories(write3-2 ${COMMON_INCLUDE})
target_link_libraries(write3-2 ${COMMON_LIBRARIES} lavatube)
target_compile_options(write3-2 PRIVATE ${COMMON_FLAGS})
add_dependencies(write3-2 sync_stuff)
add_test(NAME write_test_3-2 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/write3-2)

add_executable(write4 tests/write4.cpp src/filewriter.cpp src/util.cpp)
target_include_directories(write4 ${COMMON_INCLUDE})
target_link_libraries(write4 ${MOST_COMMON_LIBRARIES} density pthread)
target_compile_options(write4 PRIVATE ${COMMON_FLAGS})
add_dependencies(write4 sync_stuff)
add_test(NAME write_test_4 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/write4)

add_executable(read4 tests/read4.cpp src/filereader.cpp src/util.cpp)
target_include_directories(read4 ${COMMON_INCLUDE})
target_link_libraries(read4 ${MOST_COMMON_LIBRARIES} density pthread)
target_compile_options(read4 PRIVATE ${COMMON_FLAGS})
add_dependencies(read4 sync_stuff)
add_test(NAME read_test_4 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/read4)
set_property(TEST read_test_4 APPEND PROPERTY DEPENDS write_test_4)

add_executable(write5 tests/write5.cpp src/filewriter.cpp src/filereader.cpp src/util.cpp)
target_include_directories(write5 ${COMMON_INCLUDE})
target_link_libraries(write5 ${MOST_COMMON_LIBRARIES} density pthread)
target_compile_options(write5 PRIVATE ${COMMON_FLAGS})
add_dependencies(write5 sync_stuff)
add_test(NAME write_test_5 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/write5)

# --- replayer ---

add_executable(replay src/replay.cpp ${READ_SRC})
target_include_directories(replay ${COMMON_INCLUDE})
target_link_libraries(replay ${COMMON_LIBRARIES} lavatube)
target_compile_options(replay PRIVATE ${COMMON_FLAGS})
add_dependencies(replay sync_stuff)
add_test(NAME replayer_info COMMAND ${CMAKE_CURRENT_BINARY_DIR}/replay -i tracing_2.vk)
set_tests_properties(replayer_info PROPERTIES FIXTURES_REQUIRED tracing_2)
add_test(NAME replayer_vulkan_demo COMMAND ${CMAKE_CURRENT_BINARY_DIR}/replay -B -w none -v -f 10 20 ${CMAKE_SOURCE_DIR}/traces/demo_computeraytracing.vk)
add_test(NAME replayer_vkmark COMMAND ${CMAKE_CURRENT_BINARY_DIR}/replay -B -w none -v -f 5 9 ${CMAKE_SOURCE_DIR}/traces/vkmark_cube.vk)
add_test(NAME replayer_samples COMMAND ${CMAKE_CURRENT_BINARY_DIR}/replay -B -w none -v -f 5 9 ${CMAKE_SOURCE_DIR}/traces/sample_hello_triangle.vk)
add_test(NAME replayer_vkcube COMMAND ${CMAKE_CURRENT_BINARY_DIR}/replay -B -w none -v -f 5 9 ${CMAKE_SOURCE_DIR}/traces/lunarg_vkcube.vk)

# --- lavatool ---

add_executable(lavatool src/lavatool.cpp
	${CMAKE_SOURCE_DIR}/external/jsoncpp/src/lib_json/json_writer.cpp
	${CMAKE_SOURCE_DIR}/external/jsoncpp/src/lib_json/json_reader.cpp
	${CMAKE_SOURCE_DIR}/external/jsoncpp/src/lib_json/json_value.cpp
	${CMAKE_SOURCE_DIR}/src/packfile.cpp
	${CMAKE_SOURCE_DIR}/src/packfile.h
	${CMAKE_SOURCE_DIR}/src/util.cpp
	${CMAKE_SOURCE_DIR}/src/util.h
)
target_include_directories(lavatool ${COMMON_INCLUDE})
target_link_libraries(lavatool ${MOST_COMMON_LIBRARIES})
target_compile_options(lavatool PRIVATE ${COMMON_FLAGS} -fPIC)
add_test(NAME pack_test_1 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/lavatool) # usage
set_tests_properties(pack_test_1 PROPERTIES FIXTURES_REQUIRED tracing_2)
add_test(NAME pack_test_2 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/lavatool extract debug.json tracing_2.vk)
set_tests_properties(pack_test_2 PROPERTIES FIXTURES_REQUIRED tracing_2)
add_test(NAME pack_test_3_1 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/lavatool print tracking.json tracing_2.vk)
set_tests_properties(pack_test_3_1 PROPERTIES FIXTURES_REQUIRED tracing_2)
add_test(NAME pack_test_3_2 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/lavatool extract thread_0.bin tracing_2.vk)
set_tests_properties(pack_test_3_2 PROPERTIES FIXTURES_REQUIRED tracing_2)
add_test(NAME pack_test_4 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/lavatool unpack tmp_2 tracing_2.vk)
set_tests_properties(pack_test_4 PROPERTIES FIXTURES_REQUIRED tracing_2)
add_test(NAME pack_test_4_1 COMMAND cmp ${CMAKE_CURRENT_BINARY_DIR}/tmp_2/debug.json ${CMAKE_CURRENT_BINARY_DIR}/debug.json)
set_tests_properties(pack_test_4_1 PROPERTIES FIXTURES_REQUIRED tracing_2)
add_test(NAME pack_test_4_2 COMMAND cmp ${CMAKE_CURRENT_BINARY_DIR}/tmp_2/thread_0.bin ${CMAKE_CURRENT_BINARY_DIR}/thread_0.bin)
set_tests_properties(pack_test_4_2 PROPERTIES FIXTURES_REQUIRED tracing_2)
add_test(NAME pack_test_5 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/lavatool list tracing_2.vk)
set_tests_properties(pack_test_5 PROPERTIES FIXTURES_REQUIRED tracing_2)
add_test(NAME pack_test_6 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/lavatool check tracing_2.vk)
set_tests_properties(pack_test_6 PROPERTIES FIXTURES_REQUIRED tracing_2)

# --- layer tests ---

function(layer_test test_name test_exe)
	add_test(NAME layer_test_${ARGV0} COMMAND ${CMAKE_CURRENT_BINARY_DIR}/tracetooltests/vulkan_${ARGV1} ${ARGV2} ${ARGV3} ${ARGV4} ${ARGV5} ${ARGV6} ${ARGV7})
	set_tests_properties(layer_test_${ARGV0} PROPERTIES
		FIXTURES_SETUP ${ARGV0}
		ENVIRONMENT "LAVATUBE_DESTINATION=vulkan_${ARGV0};VK_LAYER_PATH=${CMAKE_CURRENT_BINARY_DIR}/implicit_layer.d;VK_INSTANCE_LAYERS=VK_LAYER_ARM_lavatube;LD_LIBRARY_PATH=${CMAKE_CURRENT_BINARY_DIR}/implicit_layer.d"
		SKIP_RETURN_CODE 77
	)
	add_test(NAME layer_test_${ARGV0}_replay COMMAND ${CMAKE_CURRENT_BINARY_DIR}/replay -V vulkan_${ARGV0}.vk)
	set_tests_properties(layer_test_${ARGV0}_replay PROPERTIES FIXTURES_REQUIRED ${ARGV0})
	add_test(NAME layer_test_${ARGV0}_replay_blackhole COMMAND ${CMAKE_CURRENT_BINARY_DIR}/replay -B vulkan_${ARGV0}.vk)
	set_tests_properties(layer_test_${ARGV0}_replay_blackhole PROPERTIES FIXTURES_REQUIRED ${ARGV0})
	add_test(NAME layer_test_${ARGV0}_replay_blackhole_nonewsi COMMAND ${CMAKE_CURRENT_BINARY_DIR}/replay -B -v -w none vulkan_${ARGV0}.vk)
	set_tests_properties(layer_test_${ARGV0}_replay_blackhole_nonewsi PROPERTIES FIXTURES_REQUIRED ${ARGV0})
	add_test(NAME layer_test_${ARGV0}_replay_nonewsi COMMAND ${CMAKE_CURRENT_BINARY_DIR}/replay -v -w none vulkan_${ARGV0}.vk)
	set_tests_properties(layer_test_${ARGV0}_replay_nonewsi PROPERTIES FIXTURES_REQUIRED ${ARGV0})
endfunction()

layer_test(general general)
add_test(NAME layer_test_general_fencedelay COMMAND ${CMAKE_CURRENT_BINARY_DIR}/tracetooltests/vulkan_general -f 1)
set_tests_properties(layer_test_general_fencedelay PROPERTIES ENVIRONMENT "VK_LAYER_PATH=${CMAKE_CURRENT_BINARY_DIR}/implicit_layer.d;VK_INSTANCE_LAYERS=VK_LAYER_ARM_lavatube;LD_LIBRARY_PATH=${CMAKE_CURRENT_BINARY_DIR}/implicit_layer.d;LAVATUBE_DELAY_FENCE_SUCCESS_FRAMES=1;")
#layer_test(general_vulkan13 general -V 3) # crashes on replay on privatedata
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
layer_test(thread_1 thread_1)
layer_test(thread_2 thread_2)
layer_test(thread_3 thread_3)
add_test(NAME layer_test_thread_3_replay_blackhole_stress_1 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/replay -V -B vulkan_thread_3.vk)
set_tests_properties(layer_test_thread_3_replay_blackhole_stress_1 PROPERTIES FIXTURES_REQUIRED thread_3)
add_test(NAME layer_test_thread_3_replay_blackhole_stress_2 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/replay -D -B vulkan_thread_3.vk)
set_tests_properties(layer_test_thread_3_replay_blackhole_stress_2 PROPERTIES FIXTURES_REQUIRED thread_3)
add_test(NAME layer_test_thread_3_replay_blackhole_stress_3 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/replay -B vulkan_thread_3.vk)
set_tests_properties(layer_test_thread_3_replay_blackhole_stress_3 PROPERTIES FIXTURES_REQUIRED thread_3)
add_test(NAME layer_test_thread_3_replay_blackhole_stress_4 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/replay -B vulkan_thread_3.vk)
set_tests_properties(layer_test_thread_3_replay_blackhole_stress_4 PROPERTIES FIXTURES_REQUIRED thread_3)
add_test(NAME layer_test_thread_3_replay_blackhole_stress_5 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/replay -B vulkan_thread_3.vk)
set_tests_properties(layer_test_thread_3_replay_blackhole_stress_5 PROPERTIES FIXTURES_REQUIRED thread_3)
add_test(NAME layer_test_thread_3_replay_blackhole_stress_6 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/replay -B vulkan_thread_3.vk)
set_tests_properties(layer_test_thread_3_replay_blackhole_stress_6 PROPERTIES FIXTURES_REQUIRED thread_3)
add_test(NAME layer_test_thread_3_replay_blackhole_stress_7 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/replay -B vulkan_thread_3.vk)
set_tests_properties(layer_test_thread_3_replay_blackhole_stress_7 PROPERTIES FIXTURES_REQUIRED thread_3)
add_test(NAME layer_test_thread_3_replay_blackhole_stress_8 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/replay -B vulkan_thread_3.vk)
set_tests_properties(layer_test_thread_3_replay_blackhole_stress_8 PROPERTIES FIXTURES_REQUIRED thread_3)
add_test(NAME layer_test_thread_3_replay_blackhole_stress_9 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/replay -B vulkan_thread_3.vk)
set_tests_properties(layer_test_thread_3_replay_blackhole_stress_9 PROPERTIES FIXTURES_REQUIRED thread_3)
layer_test(compute_1 compute_1)
add_test(NAME layer_test_compute_1_1 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/tracetooltests/vulkan_compute_1 -pc)
set_tests_properties(layer_test_compute_1_1 PROPERTIES ENVIRONMENT "VK_LAYER_PATH=${CMAKE_CURRENT_BINARY_DIR}/implicit_layer.d;VK_INSTANCE_LAYERS=VK_LAYER_ARM_lavatube;LD_LIBRARY_PATH=${CMAKE_CURRENT_BINARY_DIR}/implicit_layer.d;")
add_test(NAME layer_test_compute_1_2 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/tracetooltests/vulkan_compute_1 -pc -pcf test.bin)
set_tests_properties(layer_test_compute_1_1 PROPERTIES ENVIRONMENT "VK_LAYER_PATH=${CMAKE_CURRENT_BINARY_DIR}/implicit_layer.d;VK_INSTANCE_LAYERS=VK_LAYER_ARM_lavatube;LD_LIBRARY_PATH=${CMAKE_CURRENT_BINARY_DIR}/implicit_layer.d;")
add_test(NAME layer_test_compute_1_3 COMMAND ${CMAKE_CURRENT_BINARY_DIR}/tracetooltests/vulkan_compute_1 -pc -pcf test.bin)
set_tests_properties(layer_test_compute_1_1 PROPERTIES ENVIRONMENT "VK_LAYER_PATH=${CMAKE_CURRENT_BINARY_DIR}/implicit_layer.d;VK_INSTANCE_LAYERS=VK_LAYER_ARM_lavatube;LD_LIBRARY_PATH=${CMAKE_CURRENT_BINARY_DIR}/implicit_layer.d;")
add_custom_command(TARGET lavatool PRE_BUILD COMMAND rm -f test.bin)
layer_test(compute_2 compute_2)
layer_test(feature_1 feature_1)
layer_test(tool_1 tool_1)
#layer_test(multidevice_1 multidevice_1) # assert failure on memory type
#layer_test(as_1)
#layer_test(mesh_1)
