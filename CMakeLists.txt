cmake_minimum_required(VERSION 3.4.1)

project(attention_demo)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "-pthread")

if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
# OrangePi or other for development
    find_package(Boost REQUIRED COMPONENTS filesystem system)
    find_package(OpenCV REQUIRED)

    set(MY_LIBS ${Boost_LIBRARIES} ${OpenCV_LIBS})
else()
# cross-compile from x86 for rk3588/XT5, rk3568/LS5
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

# ---- Find the SDK's ext-bundle tarball ----
# Primary (correct) location when the SDK is sourced:
set(EXT_BUNDLE_TGZ "${OECORE_TARGET_SYSROOT}/usr/share/ext-bundle/ext-bundle-payload.tar.gz")

# Fallback (in case you're pointing at a local SDK folder in-tree)
if(NOT EXISTS "${EXT_BUNDLE_TGZ}")
  set(EXT_BUNDLE_TGZ "${CMAKE_SOURCE_DIR}/sdk/sysroots/aarch64-oe-linux/usr/share/ext-bundle/ext-bundle-payload.tar.gz")
endif()

if(NOT EXISTS "${EXT_BUNDLE_TGZ}")
  message(WARNING "ext-bundle tarball not found: ${EXT_BUNDLE_TGZ}\n"
                  "GStreamer runtime/plugins will NOT be bundled with the extension.")
endif()

# Set the build type to Debug to include debug symbols
set(CMAKE_BUILD_TYPE Debug)

# Add the -g flag to the compiler options
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g")

# skip 3rd-party lib dependencies
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--allow-shlib-undefined")

# install target and libraries
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

# rknn api
set(LIB_ARCH aarch64)
set(RKNN_RT_LIB ${CMAKE_SOURCE_DIR}/include/librknnrt.so)

#rga -- the 3588 rga lib is okay 3576
# set(RGA_PATH ${CMAKE_SOURCE_DIR}/include/3rdparty/rga/RK3588)
# set(RGA_LIB ${RGA_PATH}/lib/Linux//${LIB_ARCH}/librga.so)
set(RGA_LIB ${OECORE_TARGET_SYSROOT}/usr/lib/librga.so)

# turbojpeg
# set(TURBOJPEG_PATH ${CMAKE_SOURCE_DIR}/include/3rdparty/turbojpeg/lib/Linux/aarch64)
# set(TURBOJPEG_LIB ${TURBOJPEG_PATH}/libturbojpeg.a)


# include_directories(${RKNN_API_PATH}/include)
include_directories(${CMAKE_SOURCE_DIR}/include/3rdparty)
include_directories(${RGA_PATH}/include)

# opencv
if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
  find_package(OpenCV REQUIRED)
  find_package(TurboJPEG REQUIRED)
else()
  message(STATUS "OECORE_TARGET_SYSROOT: ${OECORE_TARGET_SYSROOT}")
  include_directories(${OECORE_TARGET_SYSROOT}/usr/include)
  include_directories(${OECORE_TARGET_SYSROOT}/usr/include/opencv4)
  include_directories(${OECORE_TARGET_SYSROOT}/usr/include/rga)
  include_directories(${OECORE_TARGET_SYSROOT}/usr/include/turbojpeg)
endif()

set(CMAKE_INSTALL_RPATH "lib")

if(NOT CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
  get_filename_component(OECORE_TARGET_SYSROOT_ABS "${OECORE_TARGET_SYSROOT}" ABSOLUTE)
  message(STATUS "OECORE_TARGET_SYSROOT_ABS: ${OECORE_TARGET_SYSROOT_ABS}")
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

set(CMAKE_INSTALL_RPATH "lib")

# rknn_yolov5_demo
include_directories( ${CMAKE_SOURCE_DIR}/include)

# Enable MQTT publisher support
add_definitions(-DHAVE_MOSQUITTO=1)

add_executable(attention_demo
        src/main.cpp
        src/attention.cpp
        src/file_utils.c
        src/image_utils.c
        src/image_drawing.c
        src/inference.cpp
        src/publisher.cpp
        src/retinaface.cc
        src/utils.cc
        src/postprocess.cc
        src/queue.cpp
        # Config module
        src/config/configuration.cpp
        src/config/model_spec.cpp
        src/config/processing_params.cpp
        src/config/publisher_config.cpp
        src/config/runtime_settings.cpp
        # Input module
        src/input/input_factory.cpp
        src/input/input_file.cpp
        src/input/input_from_registry.cpp
        src/input/input_rtsp.cpp
        src/input/input_usb.cpp
        src/input/registry_helper.cpp
        # Metrics module
        src/metrics/file_logger.cpp
        src/metrics/log_global.cpp
        # Models module
        src/models/model_factory.cpp
        src/models/model_runner_retinaface.cpp
        src/models/model_runner_yolox.cpp
        # Orchestration module
        src/orchestration/inference_worker.cpp
        src/orchestration/orchestrator.cpp
        src/orchestration/orchestrator_fixed.cpp
        src/orchestration/visualization.cpp
        # Output module
        src/output/annotator.cpp
        src/output/async_publisher.cpp
        src/output/brightsign_v3_publisher.cpp
        src/output/file_sinks.cpp
        src/output/frame_writer.cpp
        src/output/mqtt_broker.cpp
        src/output/mqtt_publisher.cpp
        src/output/udp_json_publisher.cpp
        # Pipeline module
        src/pipeline/capture_worker.cpp
        src/pipeline/inference_stage.cpp
        src/pipeline/postprocess_stage.cpp
        src/pipeline/preprocess_stage.cpp
        src/pipeline/processing_pipeline.cpp
        # Resources module
        src/resources/mem_pool.cpp
        src/resources/resource_manager.cpp
        src/resources/rga_context.cpp
        src/resources/tensor_manager.cpp
        # Tracking module
        src/tracking/tracker.cpp
        # Utility module
        src/util/util.cpp
        src/yolox.cc
)

target_link_libraries(attention_demo
  ${RKNN_RT_LIB}
  ${OpenCV_LIBS}
  ${RGA_LIB}
  ${TURBOJPEG_LIB}
  ${MY_LIBS}
  mosquitto
)

# Convert TARGET_SOC to uppercase for SOC_DIR
string(TOUPPER ${TARGET_SOC} SOC_DIR)

# Build mosquitto broker (aarch64) from source for embedded deployment
set(MOSQUITTO_SOURCE_DIR ${CMAKE_BINARY_DIR}/mosquitto-build)
set(MOSQUITTO_BINARY ${MOSQUITTO_SOURCE_DIR}/bin/mosquitto)

# Allow environment variable override, otherwise search common locations
if(DEFINED ENV{MOSQUITTO_SOURCE_TARBALL})
    set(MOSQUITTO_SOURCE_TARBALL "$ENV{MOSQUITTO_SOURCE_TARBALL}")
else()
    # Try to find mosquitto tarball in common locations
    set(MOSQUITTO_SEARCH_PATHS
        "${CMAKE_SOURCE_DIR}/brightsign-oe/build/downloads/mosquitto-2.0.18.tar.gz"
        "${CMAKE_SOURCE_DIR}/../brightsign-oe/build/downloads/mosquitto-2.0.18.tar.gz"
        "${CMAKE_SOURCE_DIR}/downloads/mosquitto-2.0.18.tar.gz"
    )
    
    foreach(SEARCH_PATH ${MOSQUITTO_SEARCH_PATHS})
        if(EXISTS "${SEARCH_PATH}")
            set(MOSQUITTO_SOURCE_TARBALL "${SEARCH_PATH}")
            break()
        endif()
    endforeach()
    
    if(NOT MOSQUITTO_SOURCE_TARBALL)
        message(WARNING "Mosquitto source tarball not found. Searched: ${MOSQUITTO_SEARCH_PATHS}")
        message(WARNING "Set MOSQUITTO_SOURCE_TARBALL environment variable to specify location")
        set(MOSQUITTO_SOURCE_TARBALL "mosquitto-2.0.18.tar.gz")
    endif()
endif()

message(STATUS "Mosquitto source tarball: ${MOSQUITTO_SOURCE_TARBALL}")

add_custom_target(mosquitto_broker ALL
    COMMAND ${CMAKE_COMMAND} -E echo "Building mosquitto broker (aarch64)..."
    COMMAND bash -c "set -e && \
        mkdir -p '${MOSQUITTO_SOURCE_DIR}' && \
        cd '${MOSQUITTO_SOURCE_DIR}' && \
        if [ ! -f CMakeLists.txt ]; then \
            echo 'Extracting mosquitto source...' && \
            tar -xzf '${MOSQUITTO_SOURCE_TARBALL}' --strip-components=1 && \
            echo 'Disabling man page generation...' && \
            sed -i 's/add_subdirectory(man)/#add_subdirectory(man)/' CMakeLists.txt || true && \
            echo 'Configuring CMake for aarch64 cross-compilation...' && \
            mkdir -p build && \
            cd build && \
            cmake .. \
                -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER} \
                -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER} \
                -DCMAKE_SYSROOT=${OECORE_TARGET_SYSROOT} \
                -DCMAKE_BUILD_TYPE=Release \
                -DWITH_TLS=ON \
                -DWITH_PERSISTENCE=ON \
                -DWITH_DOCS=OFF \
                -DWITH_TESTING=OFF \
                -DWITH_WEBSOCKETS=OFF \
                -DWITH_STRIP=ON && \
            echo 'Building mosquitto...' && \
            make -j\$(nproc) && \
            echo 'Mosquitto build completed successfully'; \
        else \
            echo 'Mosquitto already built'; \
        fi"
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Building aarch64 mosquitto broker from source"
    VERBATIM
)

# Clone and build the Go image stream server using external script
# --- Config (adjust if your paths differ) ---
set(IMAGE_STREAM_SERVER_DIR "${CMAKE_BINARY_DIR}/bs-image-stream-server")
# Match the repo's build output path/name (from Makefile: cmd/image-stream-server-arm64)
set(IMAGE_STREAM_SERVER_BINARY "${IMAGE_STREAM_SERVER_DIR}/cmd/image-stream-server-arm64")

add_custom_target(image_stream_server ALL
  COMMAND ${CMAKE_COMMAND} -E echo "Cloning and building bs-image-stream-server (auto toolchain if needed)..."
  COMMAND bash ${CMAKE_SOURCE_DIR}/scripts/build_image_server.sh ${IMAGE_STREAM_SERVER_DIR} ${IMAGE_STREAM_SERVER_BINARY} ${CMAKE_BINARY_DIR}
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
  COMMENT "Cloning and building bs-image-stream-server"
  VERBATIM
)

# Copy shared libs and models. Copy the Go binary only if it exists.
add_custom_command(TARGET attention_demo POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy ${RKNN_RT_LIB} $<TARGET_FILE_DIR:attention_demo>
  COMMAND ${CMAKE_COMMAND} -E copy ${RGA_LIB} $<TARGET_FILE_DIR:attention_demo>
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/install/${SOC_DIR}/model $<TARGET_FILE_DIR:attention_demo>/model
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_BINARY_DIR}/image-stream-server $<TARGET_FILE_DIR:attention_demo>/image-stream-server || ${CMAKE_COMMAND} -E true
  COMMENT "Copying libraries and binaries to build directory"
)

# Ensure order only if those targets exist
if(TARGET mosquitto_broker)
  add_dependencies(attention_demo mosquitto_broker)
endif()
add_dependencies(attention_demo image_stream_server)

# install target and libraries
set(CMAKE_INSTALL_PREFIX ${CMAKE_SOURCE_DIR}/install/${SOC_DIR})
install(TARGETS attention_demo DESTINATION ./)

# Install the image stream server binary from the copied location
install(FILES ${CMAKE_BINARY_DIR}/image-stream-server
        DESTINATION ./
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                   GROUP_READ GROUP_EXECUTE
                   WORLD_READ WORLD_EXECUTE
        OPTIONAL)

# Install config.json to the install directory
install(FILES ${CMAKE_SOURCE_DIR}/configs/config.json
        DESTINATION ./configs
        PERMISSIONS OWNER_READ OWNER_WRITE
                   GROUP_READ
                   WORLD_READ)

# Use FILE(COPY) which preserves the actual files rather than symlinks
install(CODE "
    # Create lib and bin directories if they don't exist
    file(MAKE_DIRECTORY \${CMAKE_INSTALL_PREFIX}/lib)
    file(MAKE_DIRECTORY \${CMAKE_INSTALL_PREFIX}/bin)
    
    # Copy MQTT broker and utilities (aarch64 binaries from SDK sysroot)
    set(MOSQUITTO_BINARIES
        \"${OECORE_TARGET_SYSROOT_ABS}/usr/bin/mosquitto_pub\"
        \"${OECORE_TARGET_SYSROOT_ABS}/usr/bin/mosquitto_sub\"
        \"${OECORE_TARGET_SYSROOT_ABS}/usr/bin/mosquitto_rr\"
        \"${OECORE_TARGET_SYSROOT_ABS}/usr/bin/mosquitto_passwd\"
    )
    foreach(bin \${MOSQUITTO_BINARIES})
        if(EXISTS \"\${bin}\")
            execute_process(COMMAND cp -L \"\${bin}\" \${CMAKE_INSTALL_PREFIX}/bin/)
        endif()
    endforeach()
    
    # Copy MQTT client library (aarch64) to both lib/ and bin/ directories
    # Binaries in bin/ need direct access to .so.1 files
    set(MOSQUITTO_LIBS
        \"${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libmosquitto.so\"
        \"${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libmosquitto.so.1\"
        \"${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libmosquittopp.so\"
        \"${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libmosquittopp.so.1\"
    )
    foreach(lib \${MOSQUITTO_LIBS})
        if(EXISTS \"\${lib}\")
            execute_process(COMMAND cp -L \"\${lib}\" \${CMAKE_INSTALL_PREFIX}/lib/)
            # Also copy to bin/ for easy access when running from bin directory
            execute_process(COMMAND cp -L \"\${lib}\" \${CMAKE_INSTALL_PREFIX}/bin/)
        endif()
    endforeach()
    
    # Copy built mosquitto broker binary (aarch64)
    if(EXISTS \"${CMAKE_BINARY_DIR}/mosquitto-build/build/src/mosquitto\")
        message(STATUS \"Copying built mosquitto broker to install...\")
        execute_process(COMMAND cp -L \"${CMAKE_BINARY_DIR}/mosquitto-build/build/src/mosquitto\" \${CMAKE_INSTALL_PREFIX}/bin/)
        message(STATUS \"\")
        message(STATUS \"MQTT Setup Complete!\")
        message(STATUS \"Libraries are co-located with binaries in: \${CMAKE_INSTALL_PREFIX}/bin/\")
        message(STATUS \"To use mosquitto utilities, set: export LD_LIBRARY_PATH=\${CMAKE_INSTALL_PREFIX}/bin:\\\$LD_LIBRARY_PATH\")
        message(STATUS \"\")
    else()
        message(WARNING \"Mosquitto broker binary not found - will rely on system mosquitto or manual deployment\")
    endif()
    
    # Copy RKNN runtime library
    execute_process(COMMAND cp -L ${RKNN_RT_LIB} \${CMAKE_INSTALL_PREFIX}/lib/)
    
    # Copy RGA library and its versioned counterparts
    execute_process(
        COMMAND bash -c \"find $(dirname ${RGA_LIB}) -name 'librga.so*' -type f -o -type l | xargs -I{} cp -L {} \${CMAKE_INSTALL_PREFIX}/lib/\"
    )
    
    # Copy TurboJPEG library and its versioned counterparts
    execute_process(
        COMMAND bash -c \"find $(dirname ${TURBOJPEG_LIB}) -name 'libturbojpeg.so*' -type f -o -type l | xargs -I{} cp -L {} \${CMAKE_INSTALL_PREFIX}/lib/\"
    )
    
    # Copy OpenCV libraries
    file(GLOB OPENCV_LIB_FILES \"${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libopencv_*.so*\")
    foreach(lib \${OPENCV_LIB_FILES})
        execute_process(COMMAND cp -L \${lib} \${CMAKE_INSTALL_PREFIX}/lib/)
    endforeach()

    # Copy tbb libraries
    file(GLOB TBB_LIB_FILES \"${OECORE_TARGET_SYSROOT_ABS}/usr/lib/libtbb*.so*\")
    foreach(lib \${TBB_LIB_FILES})
        execute_process(COMMAND cp -L \${lib} \${CMAKE_INSTALL_PREFIX}/lib/)
    endforeach()
    message(STATUS \"Bundling GStreamer runtime from: ${EXT_BUNDLE_TGZ}\")
  if(EXISTS \"${EXT_BUNDLE_TGZ}\")
    # Ensure destination dirs exist
    file(MAKE_DIRECTORY \${CMAKE_INSTALL_PREFIX}/lib)
    file(MAKE_DIRECTORY \${CMAKE_INSTALL_PREFIX}/lib/gstreamer-1.0)

    # Extract to a temp dir (so we can control final layout)
    set(_unpack_dir \${CMAKE_BINARY_DIR}/_extbundle_unpack)
    file(REMOVE_RECURSE \${_unpack_dir})
    file(MAKE_DIRECTORY \${_unpack_dir})

    # Unpack tarball; it contains 'lib/' and 'gstreamer-1.0/' at top level
    execute_process(
      COMMAND \"${CMAKE_COMMAND}\" -E tar xzf \"${EXT_BUNDLE_TGZ}\"
      WORKING_DIRECTORY \${_unpack_dir}
      RESULT_VARIABLE _tar_rv
    )
    if(NOT _tar_rv EQUAL 0)
      message(FATAL_ERROR \"Failed to extract ${EXT_BUNDLE_TGZ} (rv=\${_tar_rv})\")
    endif()

    # Copy versioned runtime libs into lib/
    if(EXISTS \${_unpack_dir}/lib)
      execute_process(COMMAND \"${CMAKE_COMMAND}\" -E copy_directory
        \${_unpack_dir}/lib \${CMAKE_INSTALL_PREFIX}/lib)
    endif()

    # Copy plugin modules into lib/gstreamer-1.0/
    if(EXISTS \${_unpack_dir}/gstreamer-1.0)
      execute_process(COMMAND \"${CMAKE_COMMAND}\" -E copy_directory
        \${_unpack_dir}/gstreamer-1.0 \${CMAKE_INSTALL_PREFIX}/lib/gstreamer-1.0)
    endif()

    # Clean temp
    file(REMOVE_RECURSE \${_unpack_dir})

    message(STATUS \"GStreamer runtime + plugins bundled under: \${CMAKE_INSTALL_PREFIX}/lib\")
  else()
    message(WARNING \"ext-bundle tarball missing; skipping GStreamer runtime bundling\")
  endif()
")
