cmake_minimum_required(VERSION 3.18)
project(nxdk_ftp_client_lib)

set(
        CMAKE_MODULE_PATH
        ${CMAKE_MODULE_PATH}
        "${CMAKE_CURRENT_LIST_DIR}/cmake/modules/"
)

set(CMAKE_VERBOSE_MAKEFILE ON CACHE BOOL "ON")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(_CMAKE_PROCESSING_LANGUAGE "CXX")

include(CheckSymbolExists)
include(FindPkgConfig)

# Determine if this build is for the Xbox or the host system.
if (CMAKE_TOOLCHAIN_FILE MATCHES "toolchain-nxdk.cmake")
    set(IS_TARGET_BUILD ON)
    find_package(NXDK REQUIRED)
else ()
    set(IS_TARGET_BUILD OFF)
endif ()


option(
        BUILD_SAMPLE
        "Build the sample program ISO"
        OFF
)

set(
        NETWORK_MODE
        AUTO
        CACHE STRING
        "Configure the network mode. Must be one of AUTO, DHCP, or STATIC"
)

if (${NETWORK_MODE} STREQUAL "DHCP")
    set(NET_INIT_DHCP ON CACHE INTERNAL "Use DHCP network config")
elseif (${NETWORK_MODE} STREQUAL "STATIC")
    set(NET_INIT_STATIC ON CACHE INTERNAL "Use static network config")
else ()
    set(NET_INIT_AUTOMATIC ON CACHE INTERNAL "Use dashboard network config")
endif ()

option(
        FORCE_FTP_PASV_IP_TO_CONTROL_IP
        "Ignore the IP portion of the FTP server's response to PASV and use the FTP_SERVER_IP instead."
        ON
)

set(
        FTP_SERVER_IP
        "10.0.2.2"
        CACHE STRING
        "IP address of the FTP server that the sample should connect to"
)

set(
        FTP_SERVER_PORT
        "2122"
        CACHE STRING
        "Port of the FTP server that the sample should connect to"
)

set(
        FTP_USER
        "xbox"
        CACHE STRING
        "Username for the FTP server that the sample should connect to"
)

set(
        FTP_PASSWORD
        "xbox"
        CACHE STRING
        "Password for the FTP server that the sample should connect to"
)

set(
        STATIC_IP
        "10.0.2.99"
        CACHE STRING
        "Static IP address to use in the sample program if USE_DHCP is OFF"
)

set(
        STATIC_GATEWAY
        "10.0.2.2"
        CACHE STRING
        "Gateway address to use in the sample program if USE_DHCP is OFF"
)

set(
        STATIC_NETMASK
        "255.255.255.0"
        CACHE STRING
        "Netmask to use in the sample program if USE_DHCP is OFF"
)

set(
        STATIC_DNS_1
        "8.8.8.8"
        CACHE STRING
        "DNS server to use in the sample program if USE_DHCP is OFF"
)

set(
        STATIC_DNS_2
        "8.8.4.4"
        CACHE STRING
        "Secondary DNS server to use in the sample program if USE_DHCP is OFF"
)


macro(set_opt_compile_and_link_options TARGET_NAME)
    target_compile_options(
            "${TARGET_NAME}"
            PRIVATE
            -O3
            -fno-strict-aliasing
            -Wall
            $<$<COMPILE_LANGUAGE:CXX>:-Wno-builtin-macro-redefined>   # Suppress warning from NXDK undef of __STDC_NO_THREADS__
    )
    target_link_options(
            "${TARGET_NAME}"
            PRIVATE
            "/debug:none"
    )
endmacro()

macro(set_compile_and_link_options TARGET_NAME)
    if (CMAKE_BUILD_TYPE MATCHES Debug)
        target_compile_options(
                "${TARGET_NAME}"
                PRIVATE
                -g
                -gdwarf-4
                -O0
                -Wall
                $<$<COMPILE_LANGUAGE:CXX>:-Wno-builtin-macro-redefined>   # Suppress warning from NXDK undef of __STDC_NO_THREADS__
        )
        if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            target_compile_options("${TARGET_NAME}" PRIVATE -fstandalone-debug)
        endif ()
        target_link_options(
                "${TARGET_NAME}"
                PRIVATE
                "/debug:full"
        )
    else ()
        set_opt_compile_and_link_options("${TARGET_NAME}")
    endif ()
endmacro()


add_subdirectory(src)


if (IS_TARGET_BUILD)
    if (BUILD_SAMPLE)
        add_subdirectory(sample)
    endif ()
else ()
    enable_testing()
    add_subdirectory(tests/host)
endif ()
