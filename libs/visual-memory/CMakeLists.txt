# libvisualmemory — reusable frame-persistence library (Stage 1).
#
# Builds BOTH ways:
#   * Standalone on a Linux host (for independent tests/examples):
#       cmake -S libs/visual-memory -B build-vm && cmake --build build-vm
#   * As a subdirectory of the Argus build when -DENABLE_VISUAL_MEMORY=ON.
#
# Dependencies: SQLite3 and TurboJPEG. On a native host they are discovered with
# find_package/find_library; when cross-compiling for BrightSign they are taken
# from the OpenEmbedded sysroot (OECORE_TARGET_SYSROOT), matching Argus.
cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED PROJECT_NAME)
    project(visualmemory LANGUAGES CXX)
    set(VISUALMEMORY_STANDALONE ON)
else()
    set(VISUALMEMORY_STANDALONE OFF)
endif()

option(VISUALMEMORY_BUILD_TESTS    "Build visualmemory unit tests"  ${VISUALMEMORY_STANDALONE})
option(VISUALMEMORY_BUILD_EXAMPLES "Build visualmemory examples"    ${VISUALMEMORY_STANDALONE})

# ---- Resolve SQLite3 + TurboJPEG ------------------------------------------
# The Argus build passes -DOECORE_TARGET_SYSROOT=...; a standalone cross build
# may only have it in the environment. Honour either.
if(NOT DEFINED OECORE_TARGET_SYSROOT AND DEFINED ENV{OECORE_TARGET_SYSROOT})
    set(OECORE_TARGET_SYSROOT "$ENV{OECORE_TARGET_SYSROOT}")
endif()

# Allow the parent build to inject explicit paths; otherwise resolve here.
if(OECORE_TARGET_SYSROOT AND NOT CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
    # Cross-compile for BrightSign: use the sysroot copies (verified present:
    # libsqlite3.so 3.26.0 and libturbojpeg.so).
    get_filename_component(_VM_SYSROOT "${OECORE_TARGET_SYSROOT}" ABSOLUTE)
    set(VM_SQLITE_INCLUDE_DIRS "${_VM_SYSROOT}/usr/include")
    set(VM_SQLITE_LIBRARIES    "${_VM_SYSROOT}/usr/lib/libsqlite3.so")
    set(VM_TURBOJPEG_INCLUDE_DIRS "${_VM_SYSROOT}/usr/include")
    set(VM_TURBOJPEG_LIBRARIES  "${_VM_SYSROOT}/usr/lib/libturbojpeg.so")
    message(STATUS "visualmemory: using sysroot SQLite3 + TurboJPEG at ${_VM_SYSROOT}")
else()
    # Native host build.
    find_package(SQLite3 REQUIRED)
    set(VM_SQLITE_INCLUDE_DIRS "${SQLite3_INCLUDE_DIRS}")
    set(VM_SQLITE_LIBRARIES    "${SQLite3_LIBRARIES}")

    find_path(VM_TURBOJPEG_INCLUDE_DIR turbojpeg.h)
    find_library(VM_TURBOJPEG_LIB turbojpeg)
    if(NOT VM_TURBOJPEG_INCLUDE_DIR OR NOT VM_TURBOJPEG_LIB)
        message(FATAL_ERROR
            "visualmemory: TurboJPEG not found. Install with: sudo apt install libturbojpeg0-dev")
    endif()
    set(VM_TURBOJPEG_INCLUDE_DIRS "${VM_TURBOJPEG_INCLUDE_DIR}")
    set(VM_TURBOJPEG_LIBRARIES    "${VM_TURBOJPEG_LIB}")
    message(STATUS "visualmemory: SQLite3=${VM_SQLITE_LIBRARIES} TurboJPEG=${VM_TURBOJPEG_LIBRARIES}")
endif()

# ---- Library target --------------------------------------------------------
add_library(visualmemory STATIC
    src/visual_memory.cpp
    src/frame_encoder.cpp
    src/file_store.cpp
    src/sqlite_store.cpp
    src/retention.cpp
    src/sha256.cpp
)
add_library(visualmemory::visualmemory ALIAS visualmemory)

target_compile_features(visualmemory PUBLIC cxx_std_20)
set_target_properties(visualmemory PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Public headers: only the standard library and visualmemory/ types are exposed.
target_include_directories(visualmemory
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${VM_SQLITE_INCLUDE_DIRS}
            ${VM_TURBOJPEG_INCLUDE_DIRS})

target_link_libraries(visualmemory
    PRIVATE ${VM_SQLITE_LIBRARIES} ${VM_TURBOJPEG_LIBRARIES})

find_package(Threads REQUIRED)
target_link_libraries(visualmemory PUBLIC Threads::Threads)

# ---- Tests -----------------------------------------------------------------
if(VISUALMEMORY_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---- Examples --------------------------------------------------------------
if(VISUALMEMORY_BUILD_EXAMPLES)
    add_executable(vm_ingest_image examples/ingest_image.cpp)
    target_link_libraries(vm_ingest_image PRIVATE visualmemory)
endif()
