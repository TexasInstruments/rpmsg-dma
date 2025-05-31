# Collect all source files from src/
file(GLOB_RECURSE DEMO_SOURCES src/*.c)

# Create executable
add_executable(rpmsg_audio_offload_example ${DEMO_SOURCES})

target_include_directories(rpmsg_audio_offload_example PRIVATE inc)

find_library(FFTW_LIB fftw3 REQUIRED)
find_library(SNDFILE_LIB sndfile REQUIRED)
find_library(ALSA_LIB asound REQUIRED)

target_link_libraries(rpmsg_audio_offload_example
PRIVATE
ti_rpmsg_dma
${FFTW_LIB}
${SNDFILE_LIB}
${ALSA_LIB}
)

install(TARGETS rpmsg_audio_offload_example RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/config/dsp_offload.cfg
        DESTINATION /etc)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/audio_sample/sample_audio.wav
        DESTINATION /opt)
