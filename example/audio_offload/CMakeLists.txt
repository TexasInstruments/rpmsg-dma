include(FindPkgConfig)

find_library(FFTW_LIB fftw3 REQUIRED)
find_library(SNDFILE_LIB sndfile REQUIRED)
find_library(ALSA_LIB asound REQUIRED)

file(GLOB_RECURSE SRC_FILES "src/*.c")

add_executable(rpmsg_audio_offload_example ${SRC_FILES})

target_compile_options(rpmsg_audio_offload_example PRIVATE -Wall -g -O2)

target_include_directories(rpmsg_audio_offload_example PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/inc
)

target_link_libraries(rpmsg_audio_offload_example
        ${FFTW_LIB}
        ${SNDFILE_LIB}
        ${ALSA_LIB}
	ti_rpmsg_dma
)

install(TARGETS rpmsg_audio_offload_example RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/config/dsp_offload.cfg
        DESTINATION /etc)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/audio_sample/sample_audio.wav
        DESTINATION /opt)
