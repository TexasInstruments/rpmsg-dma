cmake_minimum_required(VERSION 3.10)

project(ti-rpmsg-dma
	VERSION 1.0
	DESCRIPTION "RPMsg DMA Offload"
	HOMEPAGE_URL "https://github.com/TexasInstruments/rpmsg-dma"
	LANGUAGES C
)

find_library(TI_RPMSG_CHAR NAMES ti_rpmsg_char)
if(NOT TI_RPMSG_CHAR)
	message(FATAL_ERROR "Required ti_rpmsg_char library not found")
endif()

find_path(TI_RPMSG_CHAR_INCLUDE_DIR NAMES ti_rpmsg_char.h)
if(TI_RPMSG_CHAR_INCLUDE_DIR-NOTFOUND)
	message(FATAL_ERROR "Required headers not found")
endif()

add_library(rpmsg_dma SHARED
	src/dmabuf.c
	src/fw_loader.c
	src/rpmsg.c
)

target_include_directories(rpmsg_dma
	PRIVATE
		${CMAKE_SOURCE_DIR}/src
		${TI_RPMSG_CHAR_INCLUDE_DIR}
	PUBLIC
		${CMAKE_SOURCE_DIR}/include
)

target_compile_options(rpmsg_dma PRIVATE -Wall)

target_link_libraries(rpmsg_dma PRIVATE "-Wl,--no-undefined")
target_link_libraries(rpmsg_dma PRIVATE
	${TI_RPMSG_CHAR}
)

set(RPMSG_DMA_HEADERS
	include/dmabuf.h
	include/fw_loader.h
	include/rpmsg.h
)

set_target_properties(rpmsg_dma PROPERTIES VERSION ${PROJECT_VERSION})
set_target_properties(rpmsg_dma PROPERTIES SOVERSION ${PROJECT_VERSION_MAJOR})
set_target_properties(rpmsg_dma PROPERTIES PUBLIC_HEADER "${RPMSG_DMA_HEADERS}")

install(TARGETS rpmsg_dma
	LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
	PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

add_subdirectory(example/audio_offload)
