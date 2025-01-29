set(CMAKE_TOOLCHAIN_FILE ${CMAKE_SOURCE_DIR}/cmake/host-toolchain.cmake)

include(FetchContent)
FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/b514bdc898e2951020cbdca1304b75f5950d1f59.zip
        DOWNLOAD_EXTRACT_TIMESTAMP true
)

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

macro(set_common_target_options TARGET_NAME)
    target_include_directories(
            "${TARGET_NAME}"
            PRIVATE
            .
            stubs
            "${CMAKE_SOURCE_DIR}/src"
            "${CMAKE_CURRENT_BINARY_DIR}"
    )
endmacro()

#
# NXDK stub library
#
add_library(
        nxdk_stubs
        stubs/net/lwip/inet.h
        stubs/nxdk/net.c
        stubs/nxdk/net.h
)

target_include_directories(
        nxdk_stubs
        PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/stubs/net"
        "${CMAKE_CURRENT_SOURCE_DIR}/stubs"
)

add_library(NXDK::NXDK ALIAS nxdk_stubs)

#
# ftp_client tests
#
add_executable(
        test_ftp_client
        test_ftp_client.cpp
)
set_common_target_options(test_ftp_client)
target_link_libraries(test_ftp_client
        nxdk_ftp_client_lib::client
        NXDK::NXDK
        GTest::gmock_main
)

gtest_discover_tests(test_ftp_client)
