# Edge-AI RPMsg inference example

cmake_minimum_required(VERSION 3.10)
project(rpmsg_inference_example)

# Enable C++ support
enable_language(CXX)
set(CMAKE_CXX_STANDARD 17)

# Find TVM installation (neo-TVM)
# User must specify via: cmake -DTVM_ROOT=/path/to/neo-tvm ..
if(NOT DEFINED TVM_ROOT)
    # Try environment variable
    set(TVM_ROOT $ENV{NEO_TVM_PATH})
    if(NOT TVM_ROOT)
        message(FATAL_ERROR "TVM_ROOT not specified. Please set via:\n"
                           "  cmake -DTVM_ROOT=/path/to/neo-tvm ..\n"
                           "Or set environment variable:\n"
                           "  export NEO_TVM_PATH=/path/to/neo-tvm")
    endif()
endif()

message(STATUS "Using TVM_ROOT: ${TVM_ROOT}")

# Source files
set(EDGE_AI_SOURCES
    src/main.cpp
    src/tvm_inference_client.cpp
    src/dsp_task_client.cpp
    src/pipeline_manager.cpp
    src/pipeline_common.cpp
    src/audio_utils.cpp
    src/speech_enhancement_pipeline.cpp
    src/audio_classification_pipeline.cpp
    src/tvm_pipeline.cpp
    src/stft_istft_pipeline.cpp
)

# Create the executable
add_executable(rpmsg_inference_example ${EDGE_AI_SOURCES})

# Include directories
target_include_directories(rpmsg_inference_example PRIVATE
    include
    ${TVM_ROOT}/include
    ${TVM_ROOT}/3rdparty/dlpack/include
    ${TVM_ROOT}/3rdparty/dmlc-core/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../library/include
)

# Preprocessor definitions
target_compile_definitions(rpmsg_inference_example PRIVATE
    DMLC_USE_LOGGING_LIBRARY=<tvm/runtime/logging.h>
)

# Find TVM runtime library
if(NOT TVM_RUNTIME_LIB)
    # Use the ARM64 build (for local builds)
    set(TVM_ARM64_BUILD_PATH "${TVM_ROOT}")
    set(TVM_RUNTIME_LIB "${TVM_ARM64_BUILD_PATH}/libtvm_runtime.so")
endif()

# Verify the library exists
if(NOT EXISTS ${TVM_RUNTIME_LIB})
    message(FATAL_ERROR "TVM runtime not found: ${TVM_RUNTIME_LIB}")
endif()

message(STATUS "Using TVM runtime: ${TVM_RUNTIME_LIB}")

# Find json-c library for JSON pipeline configuration
find_package(PkgConfig REQUIRED)
pkg_check_modules(JSON_C REQUIRED json-c)

pkg_get_variable(SYSTEMD_SYSTEM_UNITDIR systemd systemdsystemunitdir)
if(NOT SYSTEMD_SYSTEM_UNITDIR)
    set(SYSTEMD_SYSTEM_UNITDIR "${CMAKE_INSTALL_PREFIX}/lib/systemd/system")
endif()

# Add json-c include directory
target_include_directories(rpmsg_inference_example PRIVATE ${JSON_C_INCLUDE_DIRS})

# Find audio libraries for WAV file processing
find_library(SNDFILE_LIB sndfile REQUIRED)

# Find ALSA for live PCM capture via --device
find_library(ALSA_LIB asound REQUIRED)

# Link with required libraries
target_link_libraries(rpmsg_inference_example
    ${TVM_RUNTIME_LIB}
    ti_rpmsg_dma
    ${JSON_C_LIBRARIES}
    ${SNDFILE_LIB}
    ${ALSA_LIB}
    pthread
    dl
)

# Install target
install(TARGETS rpmsg_inference_example DESTINATION ${CMAKE_INSTALL_BINDIR})

# ── Model loader daemon ──────────────────────────────────────────────────────
# Loads Module::LoadFromFile once at boot and serves inference requests over a
# Unix domain socket so that rpmsg_inference_example skips artifact loading.

add_executable(tvm_model_daemon
    src/tvm_model_daemon.cpp
    src/tvm_inference_client.cpp
)

target_include_directories(tvm_model_daemon PRIVATE
    include
    ${TVM_ROOT}/include
    ${TVM_ROOT}/3rdparty/dlpack/include
    ${TVM_ROOT}/3rdparty/dmlc-core/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../library/include
)

target_compile_definitions(tvm_model_daemon PRIVATE
    DMLC_USE_LOGGING_LIBRARY=<tvm/runtime/logging.h>
)

target_link_libraries(tvm_model_daemon
    ${TVM_RUNTIME_LIB}
    ti_rpmsg_dma
    dl
    pthread
)

install(TARGETS tvm_model_daemon DESTINATION ${CMAKE_INSTALL_BINDIR})

# Install JSON pipeline configuration files to /usr/share/tvm_inference/json/
install(FILES
${CMAKE_CURRENT_SOURCE_DIR}/json_files/pipeline_stft_istft.json
${CMAKE_CURRENT_SOURCE_DIR}/json_files/pipeline_tvm_inference.json
${CMAKE_CURRENT_SOURCE_DIR}/json_files/pipeline_speech_enhancement.json
${CMAKE_CURRENT_SOURCE_DIR}/json_files/pipeline_audio_classification_yamnet.json
${CMAKE_CURRENT_SOURCE_DIR}/json_files/pipeline_audio_classification_vggish.json
DESTINATION ${CMAKE_INSTALL_DATADIR}/tvm_inference/json)

# Install class label files to /usr/share/tvm_inference/labels/
install(FILES
${CMAKE_CURRENT_SOURCE_DIR}/labels/yamnet_label_list.txt
${CMAKE_CURRENT_SOURCE_DIR}/labels/vggish_label_list.txt
DESTINATION ${CMAKE_INSTALL_DATADIR}/tvm_inference/labels)

# Install input files to /usr/share/tvm_inference/input/
install(FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/input_audio/input_audio.wav
    ${CMAKE_CURRENT_SOURCE_DIR}/input_audio/speech_whistling2.wav
    ${CMAKE_CURRENT_SOURCE_DIR}/input_audio/miaow_16k.wav
    ${CMAKE_CURRENT_SOURCE_DIR}/input_audio/urbansound_16k.wav
    ${CMAKE_CURRENT_SOURCE_DIR}/artifacts_bin/gcrn_fixed_input.bin
    DESTINATION ${CMAKE_INSTALL_DATADIR}/tvm_inference/input)

# Install systemd services
install(FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/tvm-model-preload.service
    ${CMAKE_CURRENT_SOURCE_DIR}/tvm-model-daemon.service
    DESTINATION ${SYSTEMD_SYSTEM_UNITDIR})

# Create the model cache directory at install time
install(DIRECTORY DESTINATION /var/lib/tvm_inference)
