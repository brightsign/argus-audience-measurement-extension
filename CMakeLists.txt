cmake_minimum_required(VERSION 3.16)
project(attention_demo)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "-pthread")

# ---- Feature flags ----
add_definitions(-DDISABLE_RGA)   # keep your current behavior

# ---- Platform / sysroot & dependencies (kept from your original) ----
if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
    find_package(Boost REQUIRED COMPONENTS filesystem system)
    find_package(OpenCV REQUIRED)
    find_package(TurboJPEG REQUIRED)
    set(MY_LIBS ${Boost_LIBRARIES} ${OpenCV_LIBS})
else()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-poison-system-directories")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wpoison-system-directories")

    set(BOOST_ROOT ${CMAKE_SOURCE_DIR}/sdk/sysroots/aarch64-oe-linux/usr)

    include_directories(${OECORE_TARGET_SYSROOT}/usr/include)
    include_directories(${OECORE_TARGET_SYSROOT}/usr/include/opencv4)
    include_directories(${OECORE_TARGET_SYSROOT}/usr/include/opencv4/opencv2)
    include_directories(${OECORE_TARGET_SYSROOT}/usr/include/rga)
    include_directories(${OECORE_TARGET_SYSROOT}/usr/include/gstreamer-1.0)
    include_directories(${OECORE_TARGET_SYSROOT}/usr/include/glib-2.0)
    include_directories(${OECORE_TARGET_SYSROOT}/usr/lib/glib-2.0/include)

    link_directories(${OECORE_TARGET_SYSROOT}/usr/lib)
    set(MY_LIBS 
        opencv_core opencv_aruco opencv_bgsegm opencv_calib3d
        opencv_ccalib opencv_datasets opencv_features2d opencv_gapi
        opencv_hfs opencv_imgcodecs opencv_img_hash opencv_imgproc
        opencv_line_descriptor opencv_optflow opencv_photo opencv_quality
        opencv_rapid opencv_rgbd opencv_saliency opencv_shape
        opencv_stereo opencv_stitching opencv_superres opencv_surface_matching
        opencv_tracking opencv_ts opencv_videoio opencv_video
        opencv_videostab opencv_xfeatures2d opencv_ximgproc opencv_xphoto
        gstreamer-1.0 gstapp-1.0 gstbase-1.0 gstvideo-1.0 gobject-2.0 glib-2.0
    )
endif()

# ---- GStreamer ext-bundle (kept) ----
set(EXT_BUNDLE_TGZ "${OECORE_TARGET_SYSROOT}/usr/share/ext-bundle/ext-bundle-payload.tar.gz")
if(NOT EXISTS "${EXT_BUNDLE_TGZ}")
  set(EXT_BUNDLE_TGZ "${CMAKE_SOURCE_DIR}/sdk/sysroots/aarch64-oe-linux/usr/share/ext-bundle/ext-bundle-payload.tar.gz")
endif()

# ---- Build type & debug (kept) ----
set(CMAKE_BUILD_TYPE Debug)
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--allow-shlib-undefined")

# ---- Install prefix & rpaths (kept) ----
if(NOT DEFINED TARGET_SOC)
  set(TARGET_SOC rk3588)
endif()
if(NOT DEFINED LIB_ARCH)
  set(LIB_ARCH aarch64)
endif()
string(TOLOWER ${CMAKE_SYSTEM_NAME} cmake_system_name_lower)
string(TOLOWER ${LIB_ARCH} lib_arch_lower)
set(CMAKE_INSTALL_PREFIX ${CMAKE_SOURCE_DIR}/install/${TARGET_SOC}_${cmake_system_name_lower}_${lib_arch_lower})
set(CMAKE_SKIP_INSTALL_RPATH FALSE)
set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
set(CMAKE_INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib")
set(CMAKE_INSTALL_RPATH "lib")

# ---- Runtime libs (kept) ----
set(RKNN_RT_LIB ${CMAKE_SOURCE_DIR}/include/librknnrt.so)
set(RGA_LIB ${OECORE_TARGET_SYSROOT}/usr/lib/librga.so)

include_directories(${CMAKE_SOURCE_DIR}/include/3rdparty)
if(NOT CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
  get_filename_component(OECORE_TARGET_SYSROOT_ABS "${OECORE_TARGET_SYSROOT}" ABSOLUTE)
  include_directories(${OECORE_TARGET_SYSROOT_ABS}/usr/include)
  include_directories(${OECORE_TARGET_SYSROOT_ABS}/usr/include/opencv4)
  include_directories(${OECORE_TARGET_SYSROOT_ABS}/usr/include/rga)
  include_directories(${OECORE_TARGET_SYSROOT_ABS}/usr/include/turbojpeg)
  set(OpenCV_LIBS
    ${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libopencv_core.so
    ${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libopencv_aruco.so
    ${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libopencv_bgsegm.so
    ${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libopencv_calib3d.so
    ${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libopencv_ccalib.so
    ${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libopencv_datasets.so
    ${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libopencv_features2d.so
    ${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libopencv_gapi.so
    ${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libopencv_hfs.so
    ${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libopencv_highgui.so
    ${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libopencv_imgcodecs.so
    ${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libopencv_img_hash.so
    ${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libopencv_imgproc.so
    ${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libopencv_line_descriptor.so
    ${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libopencv_optflow.so
    ${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libopencv_photo.so
    ${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libopencv_quality.so
    ${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libopencv_rapid.so
    ${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libopencv_rgbd.so
    ${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libopencv_saliency.so
    ${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libopencv_shape.so
    ${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libopencv_stereo.so
    ${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libopencv_stitching.so
    ${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libopencv_superres.so
    ${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libopencv_surface_matching.so
    ${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libopencv_tracking.so
    ${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libopencv_ts.so
    ${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libopencv_videoio.so
    ${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libopencv_video.so
    ${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libopencv_videostab.so
    ${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libopencv_xfeatures2d.so
    ${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libopencv_ximgproc.so
    ${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libopencv_xphoto.so
  )
  set(TURBOJPEG_LIB ${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libturbojpeg.so)
endif()

# ---- Source collection: compile EVERYTHING from your module folders into the app ----
include_directories(${CMAKE_SOURCE_DIR}/include)  # so you can do #include "input/..." etc.

file(GLOB_RECURSE SRC_ALL CONFIGURE_DEPENDS
  src/*.cpp src/*.cc src/*.c
  src/config/*.cpp src/config/*.cc src/config/*.c
  src/health/*.cpp src/health/*.cc src/health/*.c
  src/input/*.cpp src/input/*.cc src/input/*.c
  src/metrics/*.cpp src/metrics/*.cc src/metrics/*.c
  src/models/*.cpp src/models/*.cc src/models/*.c
  src/orchestration/*.cpp src/orchestration/*.cc src/orchestration/*.c
  src/output/*.cpp src/output/*.cc src/output/*.c
  src/pipeline/*.cpp src/pipeline/*.cc src/pipeline/*.c
  src/resources/*.cpp src/resources/*.cc src/resources/*.c
  src/util/*.cpp
)

# Exclude template implementation files that shouldn't be compiled separately
list(REMOVE_ITEM SRC_ALL "${CMAKE_SOURCE_DIR}/src/queue.cpp")

# ---- App entrypoint ----
set(APP_SRC "")
if(EXISTS "${CMAKE_SOURCE_DIR}/apps/main.cpp")
  set(APP_SRC ${CMAKE_SOURCE_DIR}/apps/main.cpp)
elseif(EXISTS "${CMAKE_SOURCE_DIR}/src/main.cpp")
  set(APP_SRC ${CMAKE_SOURCE_DIR}/src/main.cpp)
else()
  message(FATAL_ERROR "No application entrypoint found (apps/main.cpp or src/main.cpp)")
endif()

add_executable(attention_demo
  ${APP_SRC}
  ${SRC_ALL}
)

target_compile_features(attention_demo PUBLIC cxx_std_20)
target_include_directories(attention_demo PUBLIC ${CMAKE_SOURCE_DIR})

# ---- Link all third-party/system libs at the app level (kept) ----
target_link_libraries(attention_demo
  ${RKNN_RT_LIB}
  ${OpenCV_LIBS}
  ${RGA_LIB}
  ${TURBOJPEG_LIB}
  ${MY_LIBS}
)

# ---- Go image stream server (kept) ----
string(TOUPPER ${TARGET_SOC} SOC_DIR)
set(IMAGE_STREAM_SERVER_DIR ${CMAKE_BINARY_DIR}/bs-image-stream-server)
set(IMAGE_STREAM_SERVER_BINARY ${IMAGE_STREAM_SERVER_DIR}/cmd/image-stream-server-arm64)
add_custom_target(image_stream_server ALL
    COMMAND ${CMAKE_COMMAND} -E echo "Cloning and building bs-image-stream-server..."
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}
    COMMAND bash -c "set -e && if [ ! -d '${IMAGE_STREAM_SERVER_DIR}' ]; then echo 'Cloning bs-image-stream-server...' && git clone git@github.com:brightsign/bs-image-stream-server.git '${IMAGE_STREAM_SERVER_DIR}'; else echo 'Repository already exists, updating...' && cd '${IMAGE_STREAM_SERVER_DIR}' && git pull origin main || true; fi && echo 'Building image stream server...' && cd '${IMAGE_STREAM_SERVER_DIR}' && echo 'Current directory:' && pwd && make build-arm64 && if [ -f '${IMAGE_STREAM_SERVER_BINARY}' ]; then echo 'Build completed successfully, copying binary...' && cp '${IMAGE_STREAM_SERVER_BINARY}' '${CMAKE_BINARY_DIR}/image-stream-server'; else echo 'Build failed - binary not found at ${IMAGE_STREAM_SERVER_BINARY}' && exit 1; fi"
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Cloning and building bs-image-stream-server"
    VERBATIM
)
add_dependencies(attention_demo image_stream_server)

# ---- Post-build copies (kept) ----
add_custom_command(TARGET attention_demo POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy ${RKNN_RT_LIB} $<TARGET_FILE_DIR:attention_demo>
  COMMAND ${CMAKE_COMMAND} -E copy ${RGA_LIB} $<TARGET_FILE_DIR:attention_demo>
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/install/${SOC_DIR}/model $<TARGET_FILE_DIR:attention_demo>/model
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_BINARY_DIR}/image-stream-server $<TARGET_FILE_DIR:attention_demo>/image-stream-server
)

# ---- Install (kept) ----
set(CMAKE_INSTALL_PREFIX ${CMAKE_SOURCE_DIR}/install/${SOC_DIR})
install(TARGETS attention_demo DESTINATION ./)
install(FILES ${CMAKE_BINARY_DIR}/image-stream-server
        DESTINATION ./
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                   GROUP_READ GROUP_EXECUTE
                   WORLD_READ WORLD_EXECUTE
        OPTIONAL)

# Post-build: copy configs next to the executable for local runs
add_custom_command(TARGET attention_demo POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_SOURCE_DIR}/configs
          $<TARGET_FILE_DIR:attention_demo>/configs
)

# Install: place configs under <install>/configs
install(DIRECTORY ${CMAKE_SOURCE_DIR}/configs/
        DESTINATION configs
        FILES_MATCHING PATTERN "*.json" PATTERN "*.toml" PATTERN "*.yaml")

# ---- Bundle shared libs + GStreamer runtime (kept) ----
install(CODE "
    file(MAKE_DIRECTORY \${CMAKE_INSTALL_PREFIX}/lib)

    execute_process(COMMAND cp -L ${RKNN_RT_LIB} \${CMAKE_INSTALL_PREFIX}/lib/)
    execute_process(
        COMMAND bash -c \"find $(dirname ${RGA_LIB}) -name 'librga.so*' -type f -o -type l | xargs -I{} cp -L {} \${CMAKE_INSTALL_PREFIX}/lib/\"
    )
    execute_process(
        COMMAND bash -c \"find $(dirname ${TURBOJPEG_LIB}) -name 'libturbojpeg.so*' -type f -o -type l | xargs -I{} cp -L {} \${CMAKE_INSTALL_PREFIX}/lib/\"
    )

    file(GLOB OPENCV_LIB_FILES \"${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libopencv_*.so*\")
    foreach(lib \${OPENCV_LIB_FILES})
        execute_process(COMMAND cp -L \${lib} \${CMAKE_INSTALL_PREFIX}/lib/)
    endforeach()

    file(GLOB TBB_LIB_FILES \"${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libtbb*.so*\")
    foreach(lib \${TBB_LIB_FILES})
        execute_process(COMMAND cp -L \${lib} \${CMAKE_INSTALL_PREFIX}/lib/)
    endforeach()

    message(STATUS \"Bundling GStreamer runtime from: ${EXT_BUNDLE_TGZ}\")
    if(EXISTS \"${EXT_BUNDLE_TGZ}\")
      file(MAKE_DIRECTORY \${CMAKE_INSTALL_PREFIX}/lib)
      file(MAKE_DIRECTORY \${CMAKE_INSTALL_PREFIX}/lib/gstreamer-1.0)

      set(_unpack_dir \${CMAKE_BINARY_DIR}/_extbundle_unpack)
      file(REMOVE_RECURSE \${_unpack_dir})
      file(MAKE_DIRECTORY \${_unpack_dir})

      execute_process(
        COMMAND \"${CMAKE_COMMAND}\" -E tar xzf \"${EXT_BUNDLE_TGZ}\"
        WORKING_DIRECTORY \${_unpack_dir}
        RESULT_VARIABLE _tar_rv
      )
      if(NOT _tar_rv EQUAL 0)
        message(FATAL_ERROR \"Failed to extract ${EXT_BUNDLE_TGZ} (rv=\${_tar_rv})\")
      endif()

      if(EXISTS \${_unpack_dir}/lib)
        execute_process(COMMAND \"${CMAKE_COMMAND}\" -E copy_directory
          \${_unpack_dir}/lib \${CMAKE_INSTALL_PREFIX}/lib)
      endif()
      if(EXISTS \${_unpack_dir}/gstreamer-1.0)
        execute_process(COMMAND \"${CMAKE_COMMAND}\" -E copy_directory
          \${_unpack_dir}/gstreamer-1.0 \${CMAKE_INSTALL_PREFIX}/lib/gstreamer-1.0)
      endif()

      file(REMOVE_RECURSE \${_unpack_dir})
      message(STATUS \"GStreamer runtime + plugins bundled under: \${CMAKE_INSTALL_PREFIX}/lib\")
    else()
      message(WARNING \"ext-bundle tarball missing; skipping GStreamer runtime bundling\")
    endif()
")

