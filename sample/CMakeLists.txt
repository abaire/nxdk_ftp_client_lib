find_package(NXDK_SDL2 REQUIRED)
include(XBEUtils REQUIRED)


add_executable(
        ftp_client_sample
        main.cpp
)

target_link_libraries(
        ftp_client_sample
        NXDK::NXDK
        NXDK::SDL2
        nxdk_ftp_client_lib::client
)

configure_file(configure.h.in configure.h @ONLY)

set_compile_and_link_options(ftp_client_sample)
target_include_directories(
        ftp_client_sample
        PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}"
)

# Pull debug info out of the binary into a host-side linked binary.
split_debug(ftp_client_sample)
set(EXECUTABLE_BINARY "${CMAKE_CURRENT_BINARY_DIR}/ftp_client_sample.exe")


add_xbe(xbe_file "${EXECUTABLE_BINARY}" TITLE "FTP Client sample")
add_xiso(ftp_client_sample_xiso xbe_file)
