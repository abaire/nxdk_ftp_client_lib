configure_file(configure.h.in configure.h @ONLY)

add_library(
        ftp_client
        STATIC
        ftp_client.c
        ftp_client.h
)

target_link_libraries(
        ftp_client
        PRIVATE
        NXDK::NXDK
)

target_include_directories(
        ftp_client
        PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"
        PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}"
)

set_compile_and_link_options(ftp_client)
