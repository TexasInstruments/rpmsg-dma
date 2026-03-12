cmake_minimum_required(VERSION 3.10)
project(ti_rpmsg_dma C)

set(CMAKE_CXX_STANDARD 23)
include(GNUInstallDirs)


# Define build options (ON by default)
option(BUILD_LIB "Build the rpmsg_dma library" ON)
option(BUILD_AUDIO_OFFLOAD_EXAMPLE "Build the audio_offload example" ON)
option(BUILD_2DFFT_OFFLOAD_EXAMPLE "Build the 2dfft_offload example" ON)
option(BUILD_SIGNAL_CHAIN_BIQUAD_EXAMPLE "Build the sigchain_biquad example" ON)

# Global include path
include_directories(${CMAKE_SOURCE_DIR}/library/include)

# Conditionally add subdirectories
if(BUILD_LIB)
    add_subdirectory(library)
endif()

if(BUILD_AUDIO_OFFLOAD_EXAMPLE)
    add_subdirectory(example/audio_offload)
endif()

if(BUILD_2DFFT_OFFLOAD_EXAMPLE)
    add_subdirectory(example/2dfft)
endif()

if(BUILD_SIGNAL_CHAIN_BIQUAD_EXAMPLE)
	add_subdirectory(example/sigchain_biquad)
endif()
