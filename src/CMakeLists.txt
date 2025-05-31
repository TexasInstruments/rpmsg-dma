file(GLOB LIB_SOURCES *.c)

# Build shared library
add_library(ti_rpmsg_dma SHARED ${LIB_SOURCES})

# Set version info properly
set_target_properties(ti_rpmsg_dma PROPERTIES
    VERSION 1.0.0
    SOVERSION 1
    OUTPUT_NAME "ti_rpmsg_dma"
)

find_library(RPMSG_CHAR_LIB ti_rpmsg_char REQUIRED)
target_link_libraries(ti_rpmsg_dma PRIVATE ${RPMSG_CHAR_LIB})

target_include_directories(ti_rpmsg_dma
    PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SYSROOT}/usr/include 
)

install(TARGETS ti_rpmsg_dma LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
