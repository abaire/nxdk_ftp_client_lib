#include <arpa/inet.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <csignal>
#include <fstream>
#include <future>
#include <thread>

#include "ftp_client.h"
#include "guard_flag.h"

using ::testing::ElementsAre;

static constexpr uint32_t kSelectTimeoutMilliseconds = 500;
static constexpr auto kTestTimeout = std::chrono::seconds(10);

TEST(RuntimeConfig, ftp_client_init__null_context__returns_error) {
  ASSERT_EQ(
      FTPClientInit(nullptr, ntohl(inet_addr("127.0.0.1")), 21, "user", "pass"),
      FTP_CLIENT_INIT_STATUS_INVALID_CONTEXT);
}

TEST(RuntimeConfig, ftp_client_init__returns_success) {
  FTPClient *context;
  ASSERT_EQ(FTPClientInit(&context, ntohl(inet_addr("127.0.0.1")), 21, "user",
                          "pass"),
            FTP_CLIENT_INIT_STATUS_SUCCESS);

  FTPClientDestroy(&context);
}

TEST(RuntimeConfig, ftp_client_destroy__resets_pointer) {
  FTPClient *context;
  FTPClientInit(&context, ntohl(inet_addr("127.0.0.1")), 21, "user", "pass");

  FTPClientDestroy(&context);

  ASSERT_EQ(context, nullptr);
}

TEST(RuntimeConfig, ftp_client_init__no_username__returns_success) {
  FTPClient *context;
  ASSERT_EQ(FTPClientInit(&context, ntohl(inet_addr("127.0.0.1")), 21, nullptr,
                          "pass"),
            FTP_CLIENT_INIT_STATUS_SUCCESS);

  FTPClientDestroy(&context);
}

TEST(RuntimeConfig, ftp_client_init__no_password__returns_success) {
  FTPClient *context;
  ASSERT_EQ(FTPClientInit(&context, ntohl(inet_addr("127.0.0.1")), 21, "user",
                          nullptr),
            FTP_CLIENT_INIT_STATUS_SUCCESS);

  FTPClientDestroy(&context);
}

TEST(RuntimeConfig, ftp_client_destroy__null_context__does_nothing) {
  FTPClientDestroy(nullptr);
}

TEST(RuntimeConfig, ftp_client_connect__with_invalid_context__returns_error) {
  ASSERT_EQ(FTPClientConnect(nullptr, 0),
            FTP_CLIENT_CONNECT_STATUS_INVALID_CONTEXT);
}

class FTPServerFixture : public ::testing::Test {
 public:
  int server_socket{-1};
  int data_socket{-1};
  int control_port{0};
  sockaddr_in server_addr{};
  sockaddr_in client_addr{};
  std::thread server_thread;

  std::thread watchdog_thread;
  GuardFlag test_completed;

  std::string received_data;

  GuardFlag server_ready;
  GuardFlag connection_quiescent;

  std::function<std::string(const sockaddr_in *)> on_connect = OnConnect;
  std::function<std::string(const std::string &)> on_user = OnUser;
  std::function<std::string(const std::string &)> on_password = OnPassword;

  std::vector<sockaddr_in> connect_events;
  std::vector<std::string> user_events;
  std::vector<std::string> pass_events;
  std::vector<std::string> stor_events;
  std::vector<std::string> type_events;

  void SetUp() override {
    watchdog_thread = std::thread([this]() {
      if (!test_completed.Await(kTestTimeout)) {
        raise(SIGALRM);
      }
    });

    server_thread = std::thread(&FTPServerFixture::ServerThreadProc, this);
    server_ready.Await();
  }

  void TearDown() override {
    test_completed.SetAndClamp();
    watchdog_thread.join();

    if (data_socket >= 0) {
      shutdown(data_socket, SHUT_RDWR);
      close(data_socket);
      data_socket = -1;
    }
    if (server_socket >= 0) {
      shutdown(data_socket, SHUT_RDWR);
      close(server_socket);
      server_socket = -1;
    }

    auto future =
        std::async(std::launch::async, &std::thread::join, &server_thread);
    if (future.wait_for(kTestTimeout) == std::future_status::timeout) {
      std::terminate();
    }
  }

 private:
  static std::string OnConnect(const sockaddr_in *_ignored) {
    return "220 Welcome to the test FTP server\r\n";
  }

  static std::string OnUser(const std::string &_ignored) {
    return "331 User name okay, send password.\r\n";
  }

  static std::string OnPassword(const std::string &_ignored) {
    return "230 User logged in, proceed.\r\n";
  }

  void ServerThreadProc() {
    server_socket = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
    ASSERT_NE(server_socket, -1) << "Failed to create socket";

    int opt = 1;
    ASSERT_EQ(
        setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)),
        0)
        << "Failed to set SO_REUSEADDR " << strerror(errno) << std::endl;

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    server_addr.sin_port = 0;
#ifdef __APPLE__
    server_addr.sin_len = sizeof(server_addr);
#endif

    ASSERT_EQ(
        bind(server_socket, reinterpret_cast<struct sockaddr *>(&server_addr),
             sizeof(server_addr)),
        0)
        << "Failed to bind server socket " << strerror(errno);

    socklen_t data_addr_len = sizeof(server_addr);
    ASSERT_EQ(getsockname(server_socket,
                          reinterpret_cast<struct sockaddr *>(&server_addr),
                          &data_addr_len),
              0)
        << "Failed to get control socket name " << strerror(errno);
    control_port = ntohs(server_addr.sin_port);

    ASSERT_EQ(listen(server_socket, 5), 0)
        << "Failed to listen on server socket " << strerror(errno);

    server_ready.Set();

    socklen_t client_addr_len = sizeof(client_addr);
    int client_socket =
        accept(server_socket, reinterpret_cast<struct sockaddr *>(&client_addr),
               &client_addr_len);
    ASSERT_NE(client_socket, -1)
        << "Failed to accept connection " << errno << " " << strerror(errno);

    {
      connect_events.emplace_back(client_addr);
      auto response = on_connect(&client_addr);
      SendAll(client_socket, response.c_str(), response.size());
    }

    while (true) {
      char buffer[1024];

      fd_set read_fds;
      FD_ZERO(&read_fds);
      FD_SET(client_socket, &read_fds);

      struct timeval tv{kSelectTimeoutMilliseconds / 1000,
                        (kSelectTimeoutMilliseconds % 1000) * 1000};

      int select_response =
          select(client_socket + 1, &read_fds, nullptr, nullptr, &tv);
      ASSERT_GE(select_response, 0);

      if (!select_response) {
        connection_quiescent.Set();
        continue;
      }

      ssize_t bytes_received = recv(client_socket, buffer, sizeof(buffer), 0);
      if (bytes_received <= 0) {
        break;
      }

      std::string command(buffer, bytes_received);

      if (command.find("USER") != std::string::npos) {
        user_events.emplace_back(command);
        auto response = on_user(command);
        SendAll(client_socket, response.c_str(), response.size());
      } else if (command.find("PASS") != std::string::npos) {
        pass_events.emplace_back(command);
        auto response = on_password(command);
        SendAll(client_socket, response.c_str(), response.size());
      } else if (command.find("TYPE I") != std::string::npos) {
        type_events.emplace_back(command);
        SendAll(client_socket, "200 Switching to Binary mode.\r\n", 31);
      } else if (command.find("PASV") != std::string::npos) {
        OnPasv(client_socket);
      } else if (command.find("STOR") != std::string::npos) {
        stor_events.emplace_back(command);
        OnStore(client_socket);
      } else if (command.find("QUIT") != std::string::npos) {
        SendAll(client_socket, "221 Goodbye.\r\n", 14);
        break;
      }
    }

    connection_quiescent.SetAndClamp();
    shutdown(client_socket, SHUT_RDWR);
    close(client_socket);
  }

  void OnPasv(int client_socket) {
    if (data_socket < 0) {
      data_socket = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
      ASSERT_NE(data_socket, -1) << "Failed to create data socket";
    }

    sockaddr_in data_addr{0};
    data_addr.sin_family = AF_INET;
    data_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    data_addr.sin_port = 0;

    ASSERT_EQ(bind(data_socket, reinterpret_cast<struct sockaddr *>(&data_addr),
                   sizeof(data_addr)),
              0)
        << "Failed to bind data socket";

    socklen_t data_addr_len = sizeof(data_addr);
    ASSERT_EQ(getsockname(data_socket,
                          reinterpret_cast<struct sockaddr *>(&data_addr),
                          &data_addr_len),
              0)
        << "Failed to get data socket name " << strerror(errno);
    int data_port = ntohs(data_addr.sin_port);

    ASSERT_EQ(listen(data_socket, 4), 0) << "Failed to listen on data socket";

    std::string pasv_response = "227 Entering Passive Mode (127,0,0,1," +
                                std::to_string(data_port / 256) + "," +
                                std::to_string(data_port % 256) + ").\r\n";
    SendAll(client_socket, pasv_response.c_str(), pasv_response.size());
  }

  void OnStore(int client_socket) {
    SendAll(client_socket, "150 Go ahead.\r\n", 15);

    socklen_t client_addr_len = sizeof(client_addr);
    int data_client_socket =
        accept(data_socket, reinterpret_cast<struct sockaddr *>(&client_addr),
               &client_addr_len);
    ASSERT_NE(data_client_socket, -1) << "Failed to accept data connection";

    char data_buffer[1024];
    ssize_t bytes_received;
    while ((bytes_received = recv(data_client_socket, data_buffer,
                                  sizeof(data_buffer), 0)) > 0) {
      received_data.append(data_buffer, bytes_received);
    }

    close(data_client_socket);
    SendAll(client_socket, "226 Transfer complete.\r\n", 24);
  }

  static void SendAll(int sock, const void *buffer, size_t buffer_len) {
    if (sock < 0 || !buffer_len) {
      return;
    }
    const auto *send_head = reinterpret_cast<const uint8_t *>(buffer);

    fd_set write_fds;
    FD_ZERO(&write_fds);
    FD_SET(sock, &write_fds);

    struct timeval tv{kSelectTimeoutMilliseconds / 1000,
                      (kSelectTimeoutMilliseconds % 1000) * 1000};

    while (buffer_len) {
      int select_response = select(sock + 1, nullptr, &write_fds, nullptr, &tv);
      ASSERT_GE(select_response, 0)
          << "Select failed sending data " << strerror(errno);
      ASSERT_GT(select_response, 0) << "Write timed out";

      ssize_t bytes_written = send(sock, send_head, buffer_len, 0);
      ASSERT_GE(bytes_written, 0) << "Send failed " << strerror(errno);
      ASSERT_GT(bytes_written, 0) << "Socket closed while sending";

      send_head += bytes_written;
      buffer_len -= bytes_written;
    }
  }
};

static FTPClientProcessStatus ProcessLoop(FTPClient *context,
                                          uint32_t timeout_milliseconds) {
  auto status = FTP_CLIENT_PROCESS_STATUS_SUCCESS;
  do {
    status = FTPClientProcess(context, timeout_milliseconds);
  } while (status == FTP_CLIENT_PROCESS_STATUS_SUCCESS ||
           (status == FTP_CLIENT_PROCESS_STATUS_TIMEOUT &&
            FTPClientHasSendPending(context)));
  return status;
}

static void SendCompletedCallback(bool successful, void *userdata) {
  *reinterpret_cast<bool *>(userdata) = successful;
}

TEST_F(FTPServerFixture, ftp_client_connect__connects) {
  FTPClient *context;
  FTPClientInit(&context, ntohl(inet_addr("127.0.0.1")), control_port, nullptr,
                "pass");

  EXPECT_EQ(FTPClientConnect(context, 300), FTP_CLIENT_CONNECT_STATUS_SUCCESS);

  FTPClientDestroy(&context);
}

TEST_F(FTPServerFixture, ftp_client_connect__without_username__connects) {
  FTPClient *context;
  FTPClientInit(&context, ntohl(inet_addr("127.0.0.1")), control_port, nullptr,
                nullptr);
  ASSERT_EQ(FTPClientConnect(context, 300), FTP_CLIENT_CONNECT_STATUS_SUCCESS);

  EXPECT_FALSE(FTPClientProcessStatusIsError(ProcessLoop(context, 100)));

  connection_quiescent.ClearAndAwait();

  EXPECT_TRUE(FTPClientIsFullyConnected(context));

  FTPClientDestroy(&context);
}

TEST_F(FTPServerFixture, ftp_client_connect__sends_username_and_password) {
  FTPClient *context;
  FTPClientInit(&context, ntohl(inet_addr("127.0.0.1")), control_port,
                "username", "password");
  ASSERT_EQ(FTPClientConnect(context, 300), FTP_CLIENT_CONNECT_STATUS_SUCCESS);

  EXPECT_FALSE(FTPClientProcessStatusIsError(ProcessLoop(context, 100)));

  connection_quiescent.ClearAndAwait();
  EXPECT_THAT(user_events, ElementsAre("USER username\r\n"));
  EXPECT_THAT(pass_events, ElementsAre("PASS password\r\n"));
  EXPECT_THAT(type_events, ElementsAre("TYPE I\r\n"));

  EXPECT_TRUE(FTPClientIsFullyConnected(context));

  FTPClientDestroy(&context);
}

TEST_F(FTPServerFixture, ftp_client_send_buffer) {
  FTPClient *context;
  FTPClientInit(&context, ntohl(inet_addr("127.0.0.1")), control_port,
                "username", "password");
  ASSERT_EQ(FTPClientConnect(context, 300), FTP_CLIENT_CONNECT_STATUS_SUCCESS);

  EXPECT_FALSE(FTPClientProcessStatusIsError(ProcessLoop(context, 100)));

  const char buffer[] = "This is the content of the buffer\r\nWith two lines.";
  EXPECT_TRUE(FTPClientSendBuffer(context, "test.txt", buffer, sizeof(buffer),
                                  nullptr, nullptr));

  auto result = ProcessLoop(context, 100);
  EXPECT_FALSE(FTPClientProcessStatusIsError(result))
      << "  " << strerror(errno);
  EXPECT_FALSE(FTPClientProcessStatusIsError(ProcessLoop(context, 100)));

  connection_quiescent.ClearAndAwait();
  EXPECT_STREQ(received_data.c_str(), buffer);

  FTPClientDestroy(&context);
}

TEST_F(FTPServerFixture, ftp_client_copy_and_send_buffer) {
  FTPClient *context;
  FTPClientInit(&context, ntohl(inet_addr("127.0.0.1")), control_port,
                "username", "password");
  ASSERT_EQ(FTPClientConnect(context, 300), FTP_CLIENT_CONNECT_STATUS_SUCCESS);

  EXPECT_FALSE(FTPClientProcessStatusIsError(ProcessLoop(context, 100)));

  const char buffer[] = "This is the content of the buffer\r\nWith two lines.";
  EXPECT_TRUE(FTPClientCopyAndSendBuffer(context, "test.txt", buffer,
                                         sizeof(buffer), nullptr, nullptr));

  auto result = ProcessLoop(context, 100);
  EXPECT_FALSE(FTPClientProcessStatusIsError(result))
      << "  " << strerror(errno);
  EXPECT_FALSE(FTPClientProcessStatusIsError(ProcessLoop(context, 100)));

  connection_quiescent.ClearAndAwait();
  EXPECT_STREQ(received_data.c_str(), buffer);

  FTPClientDestroy(&context);
}

TEST_F(FTPServerFixture, ftp_client_send_buffer__calls_callback) {
  FTPClient *context;
  FTPClientInit(&context, ntohl(inet_addr("127.0.0.1")), control_port,
                "username", "password");
  ASSERT_EQ(FTPClientConnect(context, 300), FTP_CLIENT_CONNECT_STATUS_SUCCESS);

  EXPECT_FALSE(FTPClientProcessStatusIsError(ProcessLoop(context, 100)));

  const char buffer[] = "This is the content of the buffer\r\nWith two lines.";
  bool send_completed = false;
  EXPECT_TRUE(FTPClientSendBuffer(context, "test.txt", buffer, sizeof(buffer),
                                  SendCompletedCallback, &send_completed));

  auto result = ProcessLoop(context, 100);
  EXPECT_FALSE(FTPClientProcessStatusIsError(result))
      << "  " << strerror(errno);
  EXPECT_FALSE(FTPClientProcessStatusIsError(ProcessLoop(context, 100)));

  connection_quiescent.ClearAndAwait();
  EXPECT_STREQ(received_data.c_str(), buffer);

  FTPClientDestroy(&context);
}

TEST_F(FTPServerFixture, ftp_client_copy_and_send_buffer__calls_callback) {
  FTPClient *context;
  FTPClientInit(&context, ntohl(inet_addr("127.0.0.1")), control_port,
                "username", "password");
  ASSERT_EQ(FTPClientConnect(context, 300), FTP_CLIENT_CONNECT_STATUS_SUCCESS);

  EXPECT_FALSE(FTPClientProcessStatusIsError(ProcessLoop(context, 100)));

  const char buffer[] = "This is the content of the buffer\r\nWith two lines.";
  bool send_completed = false;
  EXPECT_TRUE(FTPClientCopyAndSendBuffer(context, "test.txt", buffer,
                                         sizeof(buffer), SendCompletedCallback,
                                         &send_completed));

  auto result = ProcessLoop(context, 100);
  EXPECT_FALSE(FTPClientProcessStatusIsError(result))
      << "  " << strerror(errno);
  EXPECT_FALSE(FTPClientProcessStatusIsError(ProcessLoop(context, 100)));

  connection_quiescent.ClearAndAwait();
  EXPECT_STREQ(received_data.c_str(), buffer);

  FTPClientDestroy(&context);
}

TEST_F(FTPServerFixture, FTPClientSendFile__with_null_file__returns_false) {
  FTPClient *context;
  FTPClientInit(&context, ntohl(inet_addr("127.0.0.1")), control_port,
                "username", "password");
  ASSERT_EQ(FTPClientConnect(context, 300), FTP_CLIENT_CONNECT_STATUS_SUCCESS);

  EXPECT_FALSE(FTPClientProcessStatusIsError(ProcessLoop(context, 100)));

  bool send_completed = false;
  EXPECT_FALSE(FTPClientSendFile(context, nullptr, nullptr,
                                 SendCompletedCallback, &send_completed));

  FTPClientDestroy(&context);
}

TEST_F(FTPServerFixture, FTPClientSendFile__without_file__returns_false) {
  FTPClient *context;
  FTPClientInit(&context, ntohl(inet_addr("127.0.0.1")), control_port,
                "username", "password");
  ASSERT_EQ(FTPClientConnect(context, 300), FTP_CLIENT_CONNECT_STATUS_SUCCESS);

  EXPECT_FALSE(FTPClientProcessStatusIsError(ProcessLoop(context, 100)));

  bool send_completed = false;
  EXPECT_FALSE(FTPClientSendFile(context, "__this_file_does_not_exist___",
                                 nullptr, SendCompletedCallback,
                                 &send_completed));

  FTPClientDestroy(&context);
}

TEST_F(FTPServerFixture,
       FTPClientSendFile__without_remote_filename__sends_local_filename) {
  FTPClient *context;
  FTPClientInit(&context, ntohl(inet_addr("127.0.0.1")), control_port,
                "username", "password");
  ASSERT_EQ(FTPClientConnect(context, 300), FTP_CLIENT_CONNECT_STATUS_SUCCESS);

  EXPECT_FALSE(FTPClientProcessStatusIsError(ProcessLoop(context, 100)));

  auto temp_filename = testing::TempDir() + "this_is_a_test_file.txt";
  const char buffer[] = "This is the content of the buffer\r\nWith two lines.";
  std::ofstream outfile(temp_filename);
  outfile << buffer;
  outfile.close();

  bool send_completed = false;
  EXPECT_TRUE(FTPClientSendFile(context, temp_filename.c_str(), nullptr,
                                SendCompletedCallback, &send_completed));

  auto result = ProcessLoop(context, 100);
  EXPECT_FALSE(FTPClientProcessStatusIsError(result))
      << "  " << strerror(errno);
  EXPECT_FALSE(FTPClientProcessStatusIsError(ProcessLoop(context, 100)));

  connection_quiescent.ClearAndAwait();
  EXPECT_STREQ(received_data.c_str(), buffer);

  EXPECT_THAT(stor_events, ElementsAre("STOR " + temp_filename + "\r\n"));

  FTPClientDestroy(&context);
}

TEST_F(FTPServerFixture,
       FTPClientSendFile__with_remote_filename__sends_remote_filename) {
  FTPClient *context;
  FTPClientInit(&context, ntohl(inet_addr("127.0.0.1")), control_port,
                "username", "password");
  ASSERT_EQ(FTPClientConnect(context, 300), FTP_CLIENT_CONNECT_STATUS_SUCCESS);

  EXPECT_FALSE(FTPClientProcessStatusIsError(ProcessLoop(context, 100)));

  auto temp_filename = testing::TempDir() + "this_is_a_test_file.txt";
  const char buffer[] = "This is the content of the buffer\r\nWith two lines.";
  std::ofstream outfile(temp_filename);
  outfile << buffer;
  outfile.close();

  bool send_completed = false;
  EXPECT_TRUE(FTPClientSendFile(context, temp_filename.c_str(), "remoteFile",
                                SendCompletedCallback, &send_completed));

  auto result = ProcessLoop(context, 100);
  EXPECT_FALSE(FTPClientProcessStatusIsError(result))
      << "  " << strerror(errno);
  EXPECT_FALSE(FTPClientProcessStatusIsError(ProcessLoop(context, 100)));

  connection_quiescent.ClearAndAwait();
  EXPECT_STREQ(received_data.c_str(), buffer);

  EXPECT_THAT(stor_events, ElementsAre("STOR remoteFile\r\n"));

  FTPClientDestroy(&context);
}

TEST_F(FTPServerFixture,
       FTPClientSendFile__with_very_large_file__sends_everythinge) {
  FTPClient *context;
  FTPClientInit(&context, ntohl(inet_addr("127.0.0.1")), control_port,
                "username", "password");
  ASSERT_EQ(FTPClientConnect(context, 300), FTP_CLIENT_CONNECT_STATUS_SUCCESS);

  EXPECT_FALSE(FTPClientProcessStatusIsError(ProcessLoop(context, 100)));

  auto temp_filename = testing::TempDir() + "this_is_a_test_file.txt";
  std::string buffer;
  {
    std::stringstream builder;
    for (auto i = 0; i < 112; ++i) {
      builder << "abcdefghijklmnopqrstuvwxyz1234567890\n";
    }

    buffer = builder.str();
  }

  std::ofstream outfile(temp_filename);
  outfile << buffer;
  outfile.close();

  bool send_completed = false;
  EXPECT_TRUE(FTPClientSendFile(context, temp_filename.c_str(), "remoteFile",
                                SendCompletedCallback, &send_completed));

  auto result = ProcessLoop(context, 100);
  EXPECT_FALSE(FTPClientProcessStatusIsError(result))
      << "  " << strerror(errno);
  EXPECT_FALSE(FTPClientProcessStatusIsError(ProcessLoop(context, 100)));

  connection_quiescent.ClearAndAwait();
  EXPECT_EQ(received_data, buffer);

  EXPECT_THAT(stor_events, ElementsAre("STOR remoteFile\r\n"));

  FTPClientDestroy(&context);
}
