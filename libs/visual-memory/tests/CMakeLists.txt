# Host unit tests for libvisualmemory (GoogleTest).
find_package(GTest)

if(NOT GTest_FOUND)
    message(WARNING "visualmemory: GoogleTest not found — unit tests will be skipped. "
                    "Install with: sudo apt install libgtest-dev")
    return()
endif()

add_executable(visualmemory_tests
    test_frame_encoder.cpp
    test_sqlite_store.cpp
    test_retention.cpp
    test_visual_memory.cpp
    test_preprocess.cpp
    test_vector_store.cpp
    test_embedding_pipeline.cpp
    test_reader.cpp
    test_object_index.cpp
    test_pop.cpp
)

target_include_directories(visualmemory_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..          # project root for test_util.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../src       # internal headers under test
    ${VM_SQLITE_INCLUDE_DIRS}
    ${VM_TURBOJPEG_INCLUDE_DIRS})

target_link_libraries(visualmemory_tests PRIVATE
    visualmemory
    ${VM_TURBOJPEG_LIBRARIES}
    ${VM_SQLITE_LIBRARIES}
    GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(visualmemory_tests)
