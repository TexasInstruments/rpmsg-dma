cmake_minimum_required(VERSION 3.10)
project(rpmsg_dma C)

# Set C standard
set(CMAKE_C_STANDARD 23)

# Include top-level include directory
include_directories(include)

# Add subdirectories for library and example app
add_subdirectory(src)
add_subdirectory(example/audio_offload)

# Enable install directory variables like CMAKE_INSTALL_LIBDIR
include(GNUInstallDirs)

