# nxdk_ftp_client_lib

A trivial FTP client library intended for use in programs for the original Microsoft Xbox.

# Usage

The library target is `nxdk_ftp_client_lib::client`

See `sample/main.cpp` for a basic application that connects to an FTP server and uploads a file.


## CMake

```cmake
include(FetchContent)
FetchContent_Declare(
        nxdkftplib
        GIT_REPOSITORY https://github.com/abaire/nxdk_ftp_client_lib.git
        GIT_TAG        e402a1180aa75e333f37cbe79daf7974975a61bf
)
FetchContent_MakeAvailable(nxdkftplib)

add_executable(
        YourExecutable
        PRIVATE
        nxdk_ftp_client_lib::client
)
```
