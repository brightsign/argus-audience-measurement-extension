cmake_minimum_required(VERSION 3.4)
project(ArgusTests)

# Unit tests should always build for native architecture
# Clear any cross-compilation flags inherited from parent
set(CMAKE_CXX_FLAGS_BACKUP "${CMAKE_CXX_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS_BACKUP "${CMAKE_EXE_LINKER_FLAGS}")

# Force native build for tests
set(CMAKE_SYSTEM_NAME "Linux")
set(CMAKE_SYSTEM_PROCESSOR "x86_64")

# Reset to clean flags for tests
set(CMAKE_CXX_FLAGS "-pthread -std=c++17")
set(CMAKE_EXE_LINKER_FLAGS "")

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../include)

# Find OpenCV for cv::Mat tests (use native system OpenCV, not cross-compiled)
set(OpenCV_DIR "/usr/lib/x86_64-linux-gnu/cmake/opencv4")
find_package(OpenCV REQUIRED COMPONENTS core imgproc)
include_directories(${OpenCV_INCLUDE_DIRS})

file(GLOB TEST_SOURCES "test_*.cpp")

# Exclude MQTT tests - requires HAVE_MOSQUITTO to be defined
list(REMOVE_ITEM TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/test_phase2_mqtt.cpp")
# Exclude old runtime tracker tests - superseded by test_demo_license_checker.cpp
list(REMOVE_ITEM TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/test_runtime_tracker.cpp")

# Add source files needed by tests
set(TEST_DEPENDENCIES
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/util/util.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/config/configuration.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/config/model_spec.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/config/processing_params.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/config/publisher_config.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/config/runtime_settings.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/metrics/log_global.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/metrics/file_logger.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/tracking/tracker.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/tracking/byte_tracker_lite.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/output/mqtt_publisher.cpp  # Phase 2A: MQTT tests
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/output/frame_writer.cpp    # Phase 2C: Async frame writer tests
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/output/face_blur.cpp       # Required by frame_writer
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/demo/demo_license_checker.cpp  # Demo mode license checker
    # Note: Input sources (input_rtsp.cpp, input_usb.cpp, input_file.cpp) are NOT included
    # because they require GStreamer, V4L2, and hardware dependencies not available in x86_64 tests.
    # Input factory tests validate the classification logic (looks_like_rtsp, etc.) only.
)

add_executable(run_all_tests ${TEST_SOURCES} ${TEST_DEPENDENCIES})

target_link_libraries(run_all_tests
    gtest
    gtest_main
    pthread
    opencv_core
    opencv_imgproc
    opencv_imgcodecs  # Phase 2C: Required for JPEG encoding tests
    mosquitto  # Phase 2A: System mosquitto library for tests
    stdc++fs  # For std::filesystem support
)

# Restore parent flags (in case other targets need them)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS_BACKUP}")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS_BACKUP}")

enable_testing()
add_test(NAME AllUnitTests COMMAND run_all_tests)
