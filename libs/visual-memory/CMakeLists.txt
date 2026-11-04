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
    project(visualmemory LANGUAGES C CXX)
    set(VISUALMEMORY_STANDALONE ON)
else()
    set(VISUALMEMORY_STANDALONE OFF)
endif()

option(VISUALMEMORY_BUILD_TESTS    "Build visualmemory unit tests"  ${VISUALMEMORY_STANDALONE})
option(VISUALMEMORY_BUILD_EXAMPLES "Build visualmemory examples"    ${VISUALMEMORY_STANDALONE})

# ---- Stage 2 feature switches ---------------------------------------------
# The Stage 2 core (embedding worker, vector store, search API) is small, depends only on SQLite +
# TurboJPEG and is always compiled; it stays inert unless a provider is injected AND
# Config::embedding.enabled is true. These switches control the OPTIONAL parts:
#   ENABLE_SQLITE_VEC       compile the pinned sqlite-vec C source (OFF => BLOB + C++ cosine fallback)
#   ENABLE_RKNN_EMBEDDINGS  build the RKNN backend (needs rknn_api.h + librknnrt)
# (ENABLE_VISUAL_EMBEDDINGS is read by the Argus top-level CMake: it wires the adapter to Stage 2.)
option(ENABLE_SQLITE_VEC "Compile and register the vendored sqlite-vec (vec0) backend" ON)
option(ENABLE_RKNN_EMBEDDINGS "Build the RKNN embedding backend (visualmemory_rknn)" OFF)
set(VM_SQLITE_AMALGAMATION_DIR "" CACHE PATH
    "DEV/TEST ONLY: directory with sqlite3.c/sqlite3.h(+sqlite3ext.h) to build SQLite from source (e.g. 3.26.0) instead of the system lib")

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
    if(NOT VM_SQLITE_AMALGAMATION_DIR)
        find_package(SQLite3 REQUIRED)
        set(VM_SQLITE_INCLUDE_DIRS "${SQLite3_INCLUDE_DIRS}")
        set(VM_SQLITE_LIBRARIES    "${SQLite3_LIBRARIES}")
    endif()

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

# Optional: build the exact SQLite we target (3.26.0) from an amalgamation, for host tests.
if(VM_SQLITE_AMALGAMATION_DIR)
    add_library(vm_sqlite3 STATIC ${VM_SQLITE_AMALGAMATION_DIR}/sqlite3.c)
    target_include_directories(vm_sqlite3 PUBLIC ${VM_SQLITE_AMALGAMATION_DIR})
    target_compile_definitions(vm_sqlite3 PRIVATE SQLITE_THREADSAFE=1)
    set_target_properties(vm_sqlite3 PROPERTIES POSITION_INDEPENDENT_CODE ON)
    find_library(VM_DL_LIB dl)
    set(VM_SQLITE_INCLUDE_DIRS "${VM_SQLITE_AMALGAMATION_DIR}")
    set(VM_SQLITE_LIBRARIES vm_sqlite3 ${VM_DL_LIB})
    message(STATUS "visualmemory: building SQLite from amalgamation ${VM_SQLITE_AMALGAMATION_DIR}")
endif()

# ---- sqlite-vec (static, explicitly registered; never loaded at runtime) --------
if(ENABLE_SQLITE_VEC)
    include(CheckCSourceCompiles)
    set(CMAKE_REQUIRED_INCLUDES ${VM_SQLITE_INCLUDE_DIRS})
    set(_save_tt ${CMAKE_TRY_COMPILE_TARGET_TYPE})
    set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
    check_c_source_compiles("#include <sqlite3.h>\nint f(void){return SQLITE_INNOCUOUS;}" VM_SQLITE_HAS_INNOCUOUS)
    set(CMAKE_TRY_COMPILE_TARGET_TYPE ${_save_tt})
    unset(CMAKE_REQUIRED_INCLUDES)
    add_library(vm_sqlite_vec STATIC third_party/sqlite-vec/sqlite-vec.c)
    target_include_directories(vm_sqlite_vec PUBLIC third_party/sqlite-vec PRIVATE ${VM_SQLITE_INCLUDE_DIRS})
    target_compile_definitions(vm_sqlite_vec PUBLIC SQLITE_CORE SQLITE_VEC_STATIC)
    if(NOT VM_SQLITE_HAS_INNOCUOUS)
        # SQLite < 3.31 (e.g. the BrightSign SDK's 3.26.0): SQLITE_INNOCUOUS only hints security to
        # sqlite3_create_function_v2 and is ignored by old versions -> define it away (source unmodified).
        target_compile_definitions(vm_sqlite_vec PRIVATE SQLITE_INNOCUOUS=0)
        message(STATUS "visualmemory: sqlite3.h lacks SQLITE_INNOCUOUS (SQLite < 3.31) -> -DSQLITE_INNOCUOUS=0 shim")
    endif()
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
        target_compile_definitions(vm_sqlite_vec PRIVATE SQLITE_VEC_ENABLE_NEON)
    endif()
    set_target_properties(vm_sqlite_vec PROPERTIES POSITION_INDEPENDENT_CODE ON C_STANDARD 99)
    # sqlite-vec is third-party: silence its warnings only
    target_compile_options(vm_sqlite_vec PRIVATE -w)
endif()

# ---- Library target --------------------------------------------------------
add_library(visualmemory STATIC
    src/visual_memory.cpp
    src/frame_encoder.cpp
    src/file_store.cpp
    src/sqlite_store.cpp
    src/retention.cpp
    src/sha256.cpp
    # Stage 2
    src/preprocess.cpp
    src/mock_embedding_provider.cpp
    src/jpeg_decoder.cpp
    src/vector_store.cpp
    src/embedding_worker.cpp
    src/search_core.cpp
    src/reader.cpp
    src/object_index.cpp
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

if(ENABLE_SQLITE_VEC)
    target_compile_definitions(visualmemory PRIVATE VM_HAVE_SQLITE_VEC=1)
    target_link_libraries(visualmemory PRIVATE vm_sqlite_vec)
endif()
if(TARGET vm_sqlite3)
    target_link_libraries(visualmemory PRIVATE vm_sqlite3)
endif()

find_package(Threads REQUIRED)
target_link_libraries(visualmemory PUBLIC Threads::Threads)

# ---- Optional RKNN backend --------------------------------------------------
if(ENABLE_RKNN_EMBEDDINGS)
    # Inside Argus the header/runtime live in ${CMAKE_SOURCE_DIR}/include; standalone builds pass
    # -DRKNN_API_INCLUDE_DIR=... -DRKNN_RT_LIB=...
    if(NOT RKNN_API_INCLUDE_DIR)
        set(RKNN_API_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/include")
    endif()
    if(NOT RKNN_RT_LIB)
        set(RKNN_RT_LIB "${CMAKE_SOURCE_DIR}/include/librknnrt.so")
    endif()
    add_library(visualmemory_rknn STATIC backends/rknn/rknn_embedding_provider.cpp)
    target_include_directories(visualmemory_rknn
        PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/backends/rknn
        PRIVATE ${RKNN_API_INCLUDE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(visualmemory_rknn PUBLIC visualmemory PRIVATE ${RKNN_RT_LIB})
    set_target_properties(visualmemory_rknn PROPERTIES POSITION_INDEPENDENT_CODE ON)
    message(STATUS "visualmemory: RKNN backend enabled (api=${RKNN_API_INCLUDE_DIR} rt=${RKNN_RT_LIB})")
endif()

# ---- Tests -----------------------------------------------------------------
if(VISUALMEMORY_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---- Examples --------------------------------------------------------------
if(VISUALMEMORY_BUILD_EXAMPLES)
    add_executable(vm_ingest_image examples/ingest_image.cpp)
    target_link_libraries(vm_ingest_image PRIVATE visualmemory)

    # Stage 2 command-line demo / utility (ingest, search, backfill, embed, stats, bench-db)
    add_executable(vm_embed_demo examples/embed_demo.cpp)
    target_include_directories(vm_embed_demo PRIVATE ${VM_TURBOJPEG_INCLUDE_DIRS})
    target_link_libraries(vm_embed_demo PRIVATE visualmemory ${VM_TURBOJPEG_LIBRARIES})
    if(ENABLE_RKNN_EMBEDDINGS)
        target_link_libraries(vm_embed_demo PRIVATE visualmemory_rknn)
        target_compile_definitions(vm_embed_demo PRIVATE VM_WITH_RKNN=1)
    endif()
    if(TARGET vm_sqlite3)
        target_link_libraries(vm_embed_demo PRIVATE vm_sqlite3)
    endif()
endif()
