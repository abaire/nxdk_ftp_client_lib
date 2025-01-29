configure_file(configure.h.in configure.h @ONLY)

add_library(
        nxdk_ftp_client
        STATIC
        ftp_client.c
        ftp_client.h
)

target_link_libraries(
        nxdk_ftp_client
        PRIVATE
        NXDK::NXDK
)

target_include_directories(
        nxdk_ftp_client
        PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"
        PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}"
)

set_compile_and_link_options(nxdk_ftp_client)

add_library(nxdk_ftp_client_lib::client ALIAS nxdk_ftp_client)
