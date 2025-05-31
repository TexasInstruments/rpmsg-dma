cmake_minimum_required(VERSION 3.10)
project(ti_rpmsg_dma C)

set(CMAKE_CXX_STANDARD 23)
include(GNUInstallDirs)


# Define build options (ON by default)
option(BUILD_LIB "Build the rpmsg_dma library" ON)
option(BUILD_EXAMPLE "Build the audio_offload example" ON)

# Global include path
include_directories(${CMAKE_SOURCE_DIR}/include)

# Conditionally add subdirectories
if(BUILD_LIB)
    add_subdirectory(src)
endif()

if(BUILD_EXAMPLE)
    add_subdirectory(example/audio_offload)
endif()
