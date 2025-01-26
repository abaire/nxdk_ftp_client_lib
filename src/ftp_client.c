#include "ftp_client.h"

#include <lwip/sockets.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "configure.h"
#include "lwip/errno.h"

#define BUFFER_SIZE 1023
#define MAX_SEND_OPERATIONS 4

typedef enum FTPClientState {
  FTP_CLIENT_STATE_DISCONNECTED,
  FTP_CLIENT_STATE_CONNECTED_AWAIT_220,
  FTP_CLIENT_STATE_USERNAME_AWAIT_331,
  FTP_CLIENT_STATE_PASSWORD_REJECTED,
  FTP_CLIENT_STATE_PASSWORD_AWAIT_230,
  FTP_CLIENT_STATE_TYPE_BINARY_AWAIT_200,
  FTP_CLIENT_STATE_PASV_AWAIT_227,
  FTP_CLIENT_STATE_FULLY_CONNECTED,
} FTPClientState;

struct FileSend {
  int socket;
  const void *buffer;
  size_t buffer_length;
  size_t offset;
  bool buffer_owned;

  void *userdata;

  //! Callback to be invoked when the send is completed.
  void (*on_complete)(bool successful, void *userdata);
};

struct FTPClient {
  struct sockaddr_in control_sockaddr;
  struct sockaddr_in data_sockaddr;

  char *username;
  char *password;

  int control_socket;

  FTPClientState state;

  char recv_buffer[BUFFER_SIZE + 1];
  size_t recv_buffer_len;

  char send_buffer[BUFFER_SIZE + 1];
  size_t send_buffer_len;

  //! Null terminated array of FileSend instances describing files being stored
  //! to the server.
  struct FileSend *file_send_buffer[MAX_SEND_OPERATIONS];
};

static void FreeFileSend(struct FileSend *send_operation) {
  if (!send_operation) {
    return;
  }

  if (send_operation->socket >= 0) {
    close(send_operation->socket);
    send_operation->socket = -1;
  }

  if (send_operation->buffer_owned) {
    free((void *)send_operation->buffer);
  }
  free(send_operation);
}

static void FindAndFreeFileSend(FTPClient *context,
                                struct FileSend *send_operation) {
  for (size_t i = 0; i < MAX_SEND_OPERATIONS; ++i) {
    if (context->file_send_buffer[i] == send_operation) {
      context->file_send_buffer[i] = NULL;
      break;
    }
  }

  FreeFileSend(send_operation);
}

FTPClientInitStatus FTPClientInit(FTPClient **context,
                                  uint32_t ipv4_ip_host_ordered,
                                  uint16_t port_host_ordered,
                                  const char *username, const char *password) {
  if (!context) {
    return FTP_CLIENT_INIT_STATUS_INVALID_CONTEXT;
  }

  *context = (FTPClient *)calloc(1, sizeof(FTPClient));
  if (!*context) {
    return FTP_CLIENT_INIT_STATUS_OUT_OF_MEMORY;
  }

  FTPClient *client = *context;
  client->control_socket = -1;
  client->state = FTP_CLIENT_STATE_DISCONNECTED;

  client->control_sockaddr.sin_family = AF_INET;
  client->control_sockaddr.sin_addr.s_addr = htonl(ipv4_ip_host_ordered);
  client->control_sockaddr.sin_port = htons(port_host_ordered);
#ifdef __APPLE__
  client->control_sockaddr.sin_len = sizeof(client->control_sockaddr);
#endif

  client->data_sockaddr.sin_family = AF_INET;
  client->data_sockaddr.sin_addr.s_addr = 0xFFFFFFFF;
  client->data_sockaddr.sin_port = 0;
#ifdef __APPLE__
  client->data_sockaddr.sin_len = sizeof(client->data_sockaddr);
#endif

  if (username) {
    client->username = strdup(username);
    if (!client->username) {
      free(client);
      *context = NULL;
      return FTP_CLIENT_INIT_STATUS_OUT_OF_MEMORY;
    }
  }
  if (password) {
    client->password = strdup(password);
    if (!client->password) {
      free(client->username);
      free(client);
      *context = NULL;
      return FTP_CLIENT_INIT_STATUS_OUT_OF_MEMORY;
    }
  }

  return FTP_CLIENT_INIT_STATUS_SUCCESS;
}

void FTPClientDestroy(FTPClient **context) {
  if (!context || !*context) {
    return;
  }

  FTPClientClose(*context);

  if ((*context)->username) {
    free((*context)->username);
  }
  if ((*context)->password) {
    free((*context)->password);
  }

  for (size_t i = 0; i < MAX_SEND_OPERATIONS; ++i) {
    FreeFileSend((*context)->file_send_buffer[i]);
    (*context)->file_send_buffer[i] = NULL;
  }

  free(*context);
  *context = NULL;
}

void FTPClientClose(FTPClient *context) {
  if (context->control_socket >= 0) {
    close(context->control_socket);
    context->control_socket = -1;
  }
}

FTPClientConnectStatus FTPClientConnect(FTPClient *context,
                                        uint32_t timeout_milliseconds) {
  if (!context) {
    return FTP_CLIENT_CONNECT_STATUS_INVALID_CONTEXT;
  }
  if (context->control_socket >= 0) {
    FTPClientClose(context);
  }

  context->control_socket = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (context->control_socket < 0) {
    return FTP_CLIENT_CONNECT_STATUS_SOCKET_CREATE_FAILED;
  }

  if (fcntl(context->control_socket, F_SETFL, O_NONBLOCK) < 0) {
    return FTP_CLIENT_CONNECT_STATUS_SOCKET_CREATE_FAILED;
  }

  if (connect(context->control_socket,
              (struct sockaddr *)&context->control_sockaddr,
              sizeof(struct sockaddr)) < 0 &&
      errno != EWOULDBLOCK && errno != EINPROGRESS) {
    close(context->control_socket);
    context->control_socket = -1;
    return FTP_CLIENT_CONNECT_STATUS_CONNECT_FAILED;
  }

  fd_set fdset;
  FD_ZERO(&fdset);
  FD_SET(context->control_socket, &fdset);

  struct timeval tv;
  tv.tv_sec = timeout_milliseconds / 1000;
  tv.tv_usec = (timeout_milliseconds % 1000) * 1000;

  if (select(context->control_socket + 1, &fdset, NULL, NULL, &tv) == 1) {
    int so_error;
    socklen_t len = sizeof(so_error);
    getsockopt(context->control_socket, SOL_SOCKET, SO_ERROR, &so_error, &len);

    if (!so_error) {
      context->state = FTP_CLIENT_STATE_CONNECTED_AWAIT_220;
      return FTP_CLIENT_CONNECT_STATUS_SUCCESS;
    }

    errno = so_error;
    return FTP_CLIENT_CONNECT_STATUS_CONNECT_FAILED;
  }

  return FTP_CLIENT_CONNECT_STATUS_CONNECT_TIMEOUT;
}

bool FTPClientIsFullyConnected(FTPClient *context) {
  return context && context->state >= FTP_CLIENT_STATE_FULLY_CONNECTED;
}

#define RESERVE_SEND(BUFFER_NAME, BUFFER_SIZE_NAME, DATA_LENGTH)       \
  char *BUFFER_NAME = context->send_buffer + context->send_buffer_len; \
  size_t BUFFER_SIZE_NAME = BUFFER_SIZE - context->send_buffer_len;    \
  if (BUFFER_SIZE_NAME < (DATA_LENGTH)) {                              \
    return FTP_CLIENT_PROCESS_BUFFER_OVERFLOW;                         \
  }

#define VALIDATE_SEND(BYTES_WRITTEN)         \
  if ((BYTES_WRITTEN) <= 0) {                \
    return FTP_CLIENT_PROCESS_STATUS_CLOSED; \
  }                                          \
  context->send_buffer_len += (BYTES_WRITTEN);

static FTPClientProcessStatus Handle220(FTPClient *context) {
  if (!context->username) {
    context->state = FTP_CLIENT_STATE_FULLY_CONNECTED;
  } else {
    context->state = FTP_CLIENT_STATE_USERNAME_AWAIT_331;

    RESERVE_SEND(send_buffer, send_buffer_size, 7 + strlen(context->username))
    int bytes_written = snprintf(send_buffer, send_buffer_size, "USER %s\r\n",
                                 context->username);
    VALIDATE_SEND(bytes_written)
  }

  return FTP_CLIENT_PROCESS_STATUS_SUCCESS;
}

static FTPClientProcessStatus Handle331(FTPClient *context) {
  if (!context->password) {
    close(context->control_socket);
    context->control_socket = -1;
    context->state = FTP_CLIENT_STATE_PASSWORD_REJECTED;
    return false;
  }

  context->state = FTP_CLIENT_STATE_PASSWORD_AWAIT_230;
  RESERVE_SEND(send_buffer, send_buffer_size, 7 + strlen(context->password))
  int bytes_written =
      snprintf(send_buffer, send_buffer_size, "PASS %s\r\n", context->password);
  VALIDATE_SEND(bytes_written)

  return FTP_CLIENT_PROCESS_STATUS_SUCCESS;
}

static FTPClientProcessStatus Handle230(FTPClient *context) {
  context->state = FTP_CLIENT_STATE_TYPE_BINARY_AWAIT_200;
  RESERVE_SEND(send_buffer, send_buffer_size, 8)
  int bytes_written = snprintf(send_buffer, send_buffer_size, "TYPE I\r\n");
  VALIDATE_SEND(bytes_written)

  return FTP_CLIENT_PROCESS_STATUS_SUCCESS;
}

static FTPClientProcessStatus Handle200(FTPClient *context) {
  context->state = FTP_CLIENT_STATE_PASV_AWAIT_227;
  RESERVE_SEND(send_buffer, send_buffer_size, 6)
  int bytes_written = snprintf(send_buffer, send_buffer_size, "PASV\r\n");
  VALIDATE_SEND(bytes_written)

  return FTP_CLIENT_PROCESS_STATUS_SUCCESS;
}

static FTPClientProcessStatus Handle227(FTPClient *context) {
  const char *data_info_start = strchr(context->recv_buffer, '(');
  if (!data_info_start || !strchr(data_info_start, ')')) {
    return FTP_CLIENT_PROCESS_PASV_RESPONSE_INVALID;
  }

  int address[4] = {0};
  int port[2] = {0};
  if (sscanf(data_info_start + 1, "%d,%d,%d,%d,%d,%d", address, address + 1,
             address + 2, address + 3, port, port + 1) != 6) {
    return FTP_CLIENT_PROCESS_PASV_RESPONSE_INVALID;
  }

  char server_address[32] = "";
  snprintf(server_address, sizeof(server_address), "%d.%d.%d.%d", address[0],
           address[1], address[2], address[3]);
#ifndef FORCE_FTP_PASV_IP_TO_CONTROL_IP
  context->data_sockaddr.sin_addr.s_addr = inet_addr(server_address);
#else
  context->data_sockaddr.sin_addr.s_addr =
      context->control_sockaddr.sin_addr.s_addr;
#endif
  context->data_sockaddr.sin_port = htons((port[0] * 256 + port[1]) & 0xFFFF);

  context->state = FTP_CLIENT_STATE_FULLY_CONNECTED;

  return FTP_CLIENT_PROCESS_STATUS_SUCCESS;
}

static FTPClientProcessStatus Handle150(FTPClient *context) {
  for (size_t i = 0; i < MAX_SEND_OPERATIONS; ++i) {
    struct FileSend *fs = context->file_send_buffer[i];
    if (!fs || fs->socket >= 0) {
      continue;
    }

    fs->socket = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fs->socket < 0) {
      return FTP_CLIENT_PROCESS_STATUS_CREATE_DATA_SOCKET_FAILED;
    }

    if (fcntl(context->control_socket, F_SETFL, O_NONBLOCK) < 0) {
      return FTP_CLIENT_PROCESS_STATUS_CREATE_DATA_SOCKET_FAILED;
    }

    if (connect(fs->socket, (struct sockaddr *)&context->data_sockaddr,
                sizeof(struct sockaddr)) < 0 &&
        errno != EWOULDBLOCK && errno != EINPROGRESS) {
      close(fs->socket);
      fs->socket = -1;
      return FTP_CLIENT_PROCESS_STATUS_DATA_SOCKET_CONNECT_FAILED;
    }

    break;
  }

  return FTP_CLIENT_PROCESS_STATUS_SUCCESS;
}

#undef VALIDATE_SEND
#undef RESERVE_SEND

static FTPClientProcessStatus ProcessResponse(FTPClient *context) {
  if (context->recv_buffer_len < 3) {
    return FTP_CLIENT_PROCESS_STATUS_SUCCESS;
  }

  if (!strncmp(context->recv_buffer, "220", 3)) {
    return Handle220(context);
  }

  if (!strncmp(context->recv_buffer, "331", 3)) {
    return Handle331(context);
  }

  if (!strncmp(context->recv_buffer, "230", 3)) {
    return Handle230(context);
  }

  if (!strncmp(context->recv_buffer, "200", 3)) {
    return Handle200(context);
  }

  if (!strncmp(context->recv_buffer, "227", 3)) {
    return Handle227(context);
  }

  if (!strncmp(context->recv_buffer, "150", 3)) {
    return Handle150(context);
  }

  return FTP_CLIENT_PROCESS_STATUS_SUCCESS;
}

static FTPClientProcessStatus ReadControlSocket(FTPClient *context) {
  ssize_t bytes_read = recv(context->control_socket,
                            context->recv_buffer + context->recv_buffer_len,
                            BUFFER_SIZE - context->recv_buffer_len, 0);
  if (bytes_read < 0) {
    close(context->control_socket);
    context->control_socket = -1;
    return FTP_CLIENT_PROCESS_STATUS_READ_FAILED;
  }
  if (!bytes_read) {
    close(context->control_socket);
    context->control_socket = -1;
    return FTP_CLIENT_PROCESS_STATUS_CLOSED;
  }

  context->recv_buffer_len += bytes_read;

  char *terminator = strstr(context->recv_buffer, "\r\n");
  if (terminator) {
    *terminator = 0;

    FTPClientProcessStatus result = ProcessResponse(context);
    if (result != FTP_CLIENT_PROCESS_STATUS_SUCCESS) {
      close(context->control_socket);
      context->control_socket = -1;
      return result;
    }

    char *end_of_response = terminator + 2;
    size_t response_length = end_of_response - context->recv_buffer;
    size_t remaining = context->recv_buffer_len - response_length;

    if (remaining) {
      memmove(context->recv_buffer, end_of_response, remaining);
    }
    context->recv_buffer_len = remaining;
  }

  return FTP_CLIENT_PROCESS_STATUS_SUCCESS;
}

static FTPClientProcessStatus WriteControlSocket(FTPClient *context) {
  ssize_t bytes_written = write(context->control_socket, context->send_buffer,
                                context->send_buffer_len);
  if (bytes_written < 0) {
    close(context->control_socket);
    context->control_socket = -1;
    return FTP_CLIENT_PROCESS_STATUS_WRITE_FAILED;
  }
  if (!bytes_written) {
    close(context->control_socket);
    context->control_socket = -1;
    return FTP_CLIENT_PROCESS_STATUS_CLOSED;
  }

  size_t remaining = context->send_buffer_len - bytes_written;
  if (remaining) {
    memmove(context->send_buffer, context->send_buffer + bytes_written,
            remaining);
  }
  context->send_buffer_len = remaining;

  return FTP_CLIENT_PROCESS_STATUS_SUCCESS;
}

static FTPClientProcessStatus WriteDataSocket(struct FileSend *fs) {
  if (!fs || fs->socket < 0) {
    return FTP_CLIENT_PROCESS_STATUS_CLOSED;
  }

  ssize_t bytes_to_send = fs->buffer_length - fs->offset;
  ssize_t bytes_written =
      write(fs->socket, fs->buffer + fs->offset, bytes_to_send);
  if (bytes_written < 0) {
    close(fs->socket);
    fs->socket = -1;
    if (fs->on_complete) {
      fs->on_complete(false, fs->userdata);
    }

    return FTP_CLIENT_PROCESS_STATUS_SUCCESS;
  }

  if (!bytes_written) {
    close(fs->socket);
    fs->socket = -1;
    if (fs->on_complete) {
      fs->on_complete(false, fs->userdata);
    }
  }

  fs->offset += bytes_written;
  if (fs->offset == fs->buffer_length) {
    shutdown(fs->socket, O_RDWR);
    if (fs->on_complete) {
      fs->on_complete(true, fs->userdata);
    }
  }

  return FTP_CLIENT_PROCESS_STATUS_SUCCESS;
}

FTPClientProcessStatus FTPClientProcess(FTPClient *context,
                                        uint32_t timeout_milliseconds) {
  if (context->control_socket < 0) {
    return FTP_CLIENT_PROCESS_STATUS_CLOSED;
  }

  int max_fd = context->control_socket;

  fd_set read_fds;
  FD_ZERO(&read_fds);
  FD_SET(context->control_socket, &read_fds);

  fd_set write_fds;
  FD_ZERO(&write_fds);
  if (context->send_buffer_len) {
    FD_SET(context->control_socket, &write_fds);
  }
  for (size_t i = 0; i < MAX_SEND_OPERATIONS; ++i) {
    struct FileSend *fs = context->file_send_buffer[i];
    if (!fs || fs->socket < 0) {
      continue;
    }

    FD_SET(fs->socket, &write_fds);

    if (fs->socket > max_fd) {
      max_fd = fs->socket;
    }
  }

  struct timeval tv;
  tv.tv_sec = timeout_milliseconds / 1000;
  tv.tv_usec = (timeout_milliseconds % 1000) * 1000;

  const int select_response =
      select(max_fd + 1, &read_fds, &write_fds, NULL, &tv);

  if (select_response < 0) {
    return FTP_CLIENT_PROCESS_STATUS_SELECT_FAILED;
  }
  if (!select_response) {
    return FTP_CLIENT_PROCESS_STATUS_TIMEOUT;
  }

  if (FD_ISSET(context->control_socket, &read_fds)) {
    FTPClientProcessStatus result = ReadControlSocket(context);
    if (result != FTP_CLIENT_PROCESS_STATUS_SUCCESS) {
      return result;
    }
  }

  if (context->send_buffer_len &&
      FD_ISSET(context->control_socket, &write_fds)) {
    FTPClientProcessStatus result = WriteControlSocket(context);
    if (result != FTP_CLIENT_PROCESS_STATUS_SUCCESS) {
      return result;
    }
  }

  for (size_t i = 0; i < MAX_SEND_OPERATIONS; ++i) {
    struct FileSend *fs = context->file_send_buffer[i];
    if (!fs || fs->socket < 0) {
      continue;
    }

    if (FD_ISSET(fs->socket, &write_fds)) {
      FTPClientProcessStatus result = WriteDataSocket(fs);
      if (result != FTP_CLIENT_PROCESS_STATUS_SUCCESS) {
        return result;
      }
      if (fs->offset >= fs->buffer_length) {
        FindAndFreeFileSend(context, fs);
      }
    }
  }

  return FTP_CLIENT_PROCESS_STATUS_SUCCESS;
}

bool FTPClientHasSendPending(FTPClient *context) {
  if (!context) {
    return false;
  }
  if (context->send_buffer_len) {
    return true;
  }
  for (size_t i = 0; i < MAX_SEND_OPERATIONS; ++i) {
    if (context->file_send_buffer[i]) {
      return true;
    }
  }

  return false;
}

bool FTPClientProcessStatusIsError(FTPClientProcessStatus status) {
  return status != FTP_CLIENT_PROCESS_STATUS_SUCCESS &&
         status != FTP_CLIENT_PROCESS_STATUS_TIMEOUT;
}

static bool SendBuffer(FTPClient *context, const char *filename,
                       const void *buffer, size_t buffer_len,
                       void (*on_complete)(bool successful, void *userdata),
                       void *userdata, bool copy_buffer) {
  if (!FTPClientIsFullyConnected(context) || !filename || !buffer ||
      !buffer_len) {
    return false;
  }

  struct FileSend *send_operation = NULL;
  for (size_t i = 0; i < MAX_SEND_OPERATIONS; ++i) {
    if (!context->file_send_buffer[i]) {
      send_operation = (struct FileSend *)calloc(1, sizeof(*send_operation));
      context->file_send_buffer[i] = send_operation;
      break;
    }
  }

  if (!send_operation) {
    return false;
  }

  send_operation->socket = -1;
  char *send_buffer = context->send_buffer + context->send_buffer_len;
  size_t send_buffer_available = BUFFER_SIZE - context->send_buffer_len;
  if (send_buffer_available < 7 + strlen(filename)) {
    FindAndFreeFileSend(context, send_operation);
    return false;
  }

  int bytes_written =
      snprintf(send_buffer, send_buffer_available, "STOR %s\r\n", filename);
  if (bytes_written <= 0) {
    FindAndFreeFileSend(context, send_operation);
    return false;
  }
  context->send_buffer_len += bytes_written;

  if (copy_buffer) {
    void *copied_buffer = calloc(1, buffer_len);
    if (!copied_buffer) {
      FindAndFreeFileSend(context, send_operation);
      return false;
    }
    memcpy(copied_buffer, buffer, buffer_len);

    send_operation->buffer_owned = true;
    send_operation->buffer = copied_buffer;
  } else {
    send_operation->buffer_owned = false;
    send_operation->buffer = buffer;
  }

  send_operation->offset = 0;
  send_operation->buffer_length = buffer_len;
  send_operation->userdata = userdata;
  send_operation->on_complete = on_complete;

  return true;
}

bool FTPClientCopyAndSendBuffer(FTPClient *context, const char *filename,
                                const void *buffer, size_t buffer_len,
                                void (*on_complete)(bool successful,
                                                    void *userdata),
                                void *userdata) {
  return SendBuffer(context, filename, buffer, buffer_len, on_complete,
                    userdata, true);
}

bool FTPClientSendBuffer(FTPClient *context, const char *filename,
                         const void *buffer, size_t buffer_len,
                         void (*on_complete)(bool successful, void *userdata),
                         void *userdata) {
  return SendBuffer(context, filename, buffer, buffer_len, on_complete,
                    userdata, false);
}
