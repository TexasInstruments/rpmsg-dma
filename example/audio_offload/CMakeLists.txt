include(FindPkgConfig)

pkg_check_modules(fftw3 REQUIRED IMPORTED_TARGET fftw3)
pkg_check_modules(sndfile REQUIRED IMPORTED_TARGET sndfile)
pkg_check_modules(alsa REQUIRED IMPORTED_TARGET alsa)

file(GLOB_RECURSE SRC_FILES "src/*.c")

add_executable(rpmsg_audio_offload_example ${SRC_FILES})

target_compile_options(rpmsg_audio_offload_example PRIVATE -Wall -g -O2)

target_include_directories(rpmsg_audio_offload_example PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/inc
)

target_link_libraries(rpmsg_audio_offload_example
	PkgConfig::fftw3
	PkgConfig::sndfile
	PkgConfig::alsa
	rpmsg_dma
)

install(TARGETS rpmsg_audio_offload_example RUNTIME DESTINATION bin)
