# Build of the display-frame feature. Host (default): core + bridge + test double + unit tests. Cross build of just the bridge for the player:
#   source <resilience sdk>/environment-setup-aarch64-oe-linux
#   cmake -S tests/display -B build-bridge-xt5 -DDISPLAY_BUILD_TESTS=OFF -DCMAKE_TOOLCHAIN_FILE=$OECORE_NATIVE_SYSROOT/usr/share/cmake/OEToolchainConfig.cmake
# Host build of the display-frame feature: core + bridge + test double + unit tests. Needs grpc++/protobuf/protoc/grpc_cpp_plugin.
#   cmake -S tests/display -B build-display && cmake --build build-display -j && ctest --test-dir build-display
cmake_minimum_required(VERSION 3.16)
project(display_frames_host CXX)
option(DISPLAY_BUILD_TESTS "Build the OS-service test double and the unit tests (host only)" ON)
set(CMAKE_CXX_STANDARD 17)
set(R ${CMAKE_CURRENT_SOURCE_DIR}/../..)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GRPCPP REQUIRED grpc++ protobuf)
find_program(PROTOC protoc REQUIRED)
find_program(GRPC_PLUGIN grpc_cpp_plugin REQUIRED)

set(GEN ${CMAKE_CURRENT_BINARY_DIR}/gen)
file(MAKE_DIRECTORY ${GEN})
set(PROTO_DIR ${R}/third_party/resilience-proto)
set(PROTOS brightsign/resilience/v1/common.proto brightsign/resilience/v1/rendered_output.proto)
set(GEN_SRCS)
foreach(p ${PROTOS})
  get_filename_component(n ${p} NAME_WE)
  set(base ${GEN}/brightsign/resilience/v1/${n})
  add_custom_command(OUTPUT ${base}.pb.cc ${base}.pb.h ${base}.grpc.pb.cc ${base}.grpc.pb.h
    COMMAND ${PROTOC} -I ${PROTO_DIR} --cpp_out=${GEN} --grpc_out=${GEN} --plugin=protoc-gen-grpc=${GRPC_PLUGIN} ${PROTO_DIR}/${p}
    DEPENDS ${PROTO_DIR}/${p})
  list(APPEND GEN_SRCS ${base}.pb.cc ${base}.grpc.pb.cc)
endforeach()
add_library(resilience_proto STATIC ${GEN_SRCS})
target_include_directories(resilience_proto PUBLIC ${GEN} ${GRPCPP_INCLUDE_DIRS})
target_link_libraries(resilience_proto PUBLIC ${GRPCPP_LIBRARIES})

add_library(display_core STATIC ${R}/src/display/display_frame_source.cpp ${R}/src/display/bridge_client.cpp)
target_include_directories(display_core PUBLIC ${R}/src ${R}/include/3rdparty)
target_link_libraries(display_core PUBLIC Threads::Threads)

add_executable(argus_display_bridge ${R}/src/tools/display_grpc_bridge.cpp)
target_include_directories(argus_display_bridge PRIVATE ${R}/src ${R}/include/3rdparty)
target_link_libraries(argus_display_bridge PRIVATE display_core resilience_proto Threads::Threads)

if(DISPLAY_BUILD_TESTS)
add_executable(fake_rendered_output_server fake_rendered_output_server.cpp)
target_include_directories(fake_rendered_output_server PRIVATE ${R}/include/3rdparty)
target_link_libraries(fake_rendered_output_server PRIVATE resilience_proto Threads::Threads)
find_package(GTest)
endif()
if(DISPLAY_BUILD_TESTS AND GTest_FOUND)
  enable_testing()
  add_executable(display_source_tests test_display_source.cpp)
  target_include_directories(display_source_tests PRIVATE ${R}/src ${R}/include/3rdparty)
  target_link_libraries(display_source_tests PRIVATE display_core GTest::gtest_main Threads::Threads)
  add_test(NAME display_source_tests COMMAND display_source_tests)
endif()
