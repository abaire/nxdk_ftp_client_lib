#ifndef FTP_CLIENT_H
#define FTP_CLIENT_H

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wignored-attributes"
#include <lwip/inet.h>
#pragma clang diagnostic pop
#include <nxdk/net.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Context for the nxdk FTP client.
typedef struct FTPClient FTPClient;

typedef enum FTPClientInitStatus {
  FTP_CLIENT_INIT_STATUS_SUCCESS,
  FTP_CLIENT_INIT_STATUS_INVALID_CONTEXT,
  FTP_CLIENT_INIT_STATUS_OUT_OF_MEMORY,
  FTP_CLIENT_INIT_STATUS_INVALID_IP,
  FTP_CLIENT_INIT_STATUS_INVALID_PORT,
} FTPClientInitStatus;

//! Creates a new FTPClient instance.
FTPClientInitStatus FTPClientInit(FTPClient **context,
                                  uint32_t ipv4_ip_host_ordered,
                                  uint16_t port_host_ordered,
                                  const char *username, const char *password);

//! Destroys an FTPClient instance.
void FTPClientDestroy(FTPClient **context);

//! Close the sockets of the given FTPClient.
void FTPClientClose(FTPClient *context);

typedef enum FTPClientConnectStatus {
  FTP_CLIENT_CONNECT_STATUS_SUCCESS = 0,
  FTP_CLIENT_CONNECT_STATUS_INVALID_CONTEXT = 1000,
  FTP_CLIENT_CONNECT_STATUS_SOCKET_CREATE_FAILED = 2000,
  FTP_CLIENT_CONNECT_STATUS_CONNECT_FAILED = 3000,
  FTP_CLIENT_CONNECT_STATUS_CONNECT_TIMEOUT = 3001,
} FTPClientConnectStatus;

FTPClientConnectStatus FTPClientConnect(FTPClient *context,
                                        uint32_t timeout_milliseconds);

typedef enum FTPClientProcessStatus {
  FTP_CLIENT_PROCESS_STATUS_SUCCESS = 0,
  FTP_CLIENT_PROCESS_STATUS_TIMEOUT = 1,
  FTP_CLIENT_PROCESS_STATUS_SELECT_FAILED = 1000,
  FTP_CLIENT_PROCESS_STATUS_READ_FAILED = 1001,
  FTP_CLIENT_PROCESS_STATUS_WRITE_FAILED = 1002,
  FTP_CLIENT_PROCESS_STATUS_SOCKET_EXCEPTION = 1003,
  FTP_CLIENT_PROCESS_STATUS_CLOSED = 2000,
  FTP_CLIENT_PROCESS_PASV_RESPONSE_INVALID = 2001,
  FTP_CLIENT_PROCESS_STATUS_CREATE_DATA_SOCKET_FAILED = 5000,
  FTP_CLIENT_PROCESS_STATUS_CREATE_DATA_BUFFER_FAILED = 5001,
  FTP_CLIENT_PROCESS_STATUS_CREATE_DATA_FILE_READ_FAILED = 5002,
  FTP_CLIENT_PROCESS_STATUS_DATA_SOCKET_CONNECT_FAILED = 6000,
  FTP_CLIENT_PROCESS_STATUS_DATA_SOCKET_EXCEPTION = 6001,
  FTP_CLIENT_PROCESS_BUFFER_OVERFLOW = 8000,
} FTPClientProcessStatus;

FTPClientProcessStatus FTPClientProcess(FTPClient *context,
                                        uint32_t timeout_milliseconds);

bool FTPClientIsFullyConnected(FTPClient *context);

bool FTPClientHasSendPending(FTPClient *context);

bool FTPClientProcessStatusIsError(FTPClientProcessStatus status);

bool FTPClientSendBuffer(FTPClient *context, const char *filename,
                         const void *buffer, size_t buffer_len,
                         void (*on_complete)(bool successful, void *userdata),
                         void *userdata);

bool FTPClientCopyAndSendBuffer(FTPClient *context, const char *filename,
                                const void *buffer, size_t buffer_len,
                                void (*on_complete)(bool successful,
                                                    void *userdata),
                                void *userdata);

bool FTPClientSendFile(FTPClient *context, const char *local_filename,
                       const char *remote_filename,
                       void (*on_complete)(bool successful, void *userdata),
                       void *userdata);

//! Retrieves the `errno` value related to the most recent failure.
int FTPClientErrno(FTPClient *context);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // FTP_CLIENT_H
