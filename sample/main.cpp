#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wignored-attributes"
#include <hal/debug.h>
#pragma clang diagnostic pop
#include <SDL.h>
#include <errno.h>
#include <hal/fileio.h>
#include <hal/video.h>
#include <lwip/netif.h>
#include <pbkit/pbkit.h>

#include "configure.h"
#include "ftp_client.h"

static constexpr int kFramebufferWidth = 640;
static constexpr int kFramebufferHeight = 480;
static constexpr int kDelayOnFailureMilliseconds = 4000;
static constexpr int kConnectTimeoutMilliseconds = 10000;

static constexpr char kTestFilename[]{"nxdk_ftp_client_lib_test.txt"};
static constexpr char kTestData[]{
    "This is a file that was\n"
    "transmitted from the nxdk_ftp_client_lib sample program.\n"
    "\n"
    "\n"
    "\n"
    "\n"
    "This sentence is not true."};

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

static void ConnectAndSendTestFile() {
  FTPClient *client;
  int port = FTP_SERVER_PORT;
  FTPClientInit(&client, ntohl(inet_addr(FTP_SERVER_IP)), port, FTP_USER,
                FTP_PASSWORD);

  {
    debugPrint("Connecting to %s:%d\n", FTP_SERVER_IP, port);
    pb_show_debug_screen();
    FTPClientConnectStatus status =
        FTPClientConnect(client, kConnectTimeoutMilliseconds);
    if (status != FTP_CLIENT_CONNECT_STATUS_SUCCESS) {
      debugPrint("Connection failed: %d %s\n", status, strerror(errno));
      FTPClientDestroy(&client);
      return;
    }
  }

  FTPClientProcessStatus status = ProcessLoop(client, 100);
  if (FTPClientProcessStatusIsError(status)) {
    debugPrint("Failed to authenticate with server: %d\n", status);
    pb_show_debug_screen();
    FTPClientDestroy(&client);
    return;
  }

  debugPrint("Sending STOR request...\n");
  pb_show_debug_screen();
  if (!FTPClientSendBuffer(client, kTestFilename, kTestData, sizeof(kTestData),
                           nullptr, nullptr)) {
    debugPrint("Failed to initiate file send.\n");
    FTPClientDestroy(&client);
    return;
  }

  debugPrint("Sending file...\n");
  pb_show_debug_screen();
  status = ProcessLoop(client, 100);
  if (FTPClientProcessStatusIsError(status)) {
    debugPrint("Failed to send file to server: %d %s\n", status,
               strerror(errno));
    FTPClientDestroy(&client);
    return;
  }

  debugPrint("Completed, closing...\n");
  pb_show_debug_screen();

  FTPClientDestroy(&client);
}

int main() {
  debugPrint("Set video mode");
  XVideoSetMode(kFramebufferWidth, kFramebufferHeight, 32, REFRESH_DEFAULT);
  pb_show_debug_screen();

  // Reserve 4 times the size of the default framebuffers to allow for
  // antialiasing.
  int status = pb_init();
  if (status) {
    debugPrint("pb_init Error %d\n", status);
    pb_show_debug_screen();
    Sleep(kDelayOnFailureMilliseconds);
    return 1;
  }

  if (SDL_Init(SDL_INIT_GAMECONTROLLER)) {
    debugPrint("Failed to initialize SDL_GAMECONTROLLER.");
    debugPrint("%s", SDL_GetError());
    pb_show_debug_screen();
    Sleep(kDelayOnFailureMilliseconds);
    return 1;
  }

#if defined(NET_INIT_AUTOMATIC)
  debugPrint("Initializing network using automatic config...\n");
  pb_show_debug_screen();
  int net_init_result = nxNetInit(nullptr);
#else
  nx_net_parameters_t net_params = {};
  net_params.ipv4_dns1 = inet_addr(STATIC_DNS_1);
  net_params.ipv4_dns2 = inet_addr(STATIC_DNS_2);
#if defined(NET_INIT_DHCP)
  debugPrint("Initializing network using DHCP...\n");
  pb_show_debug_screen();
  net_params.ipv4_mode = NX_NET_DHCP;
#else
  debugPrint("Initializing network using static IP...\n");
  pb_show_debug_screen();
  net_params.ipv4_mode = NX_NET_STATIC;
  net_params.ipv4_ip = inet_addr(STATIC_IP);
  net_params.ipv4_gateway = inet_addr(STATIC_GATEWAY);
  net_params.ipv4_netmask = inet_addr(STATIC_NETMASK);
#endif  // defined(NET_INIT_DHCP)
  int net_init_result = nxNetInit(&params);
#endif  // defined(NET_INIT_AUTOMATIC)

  if (net_init_result) {
    debugPrint("nxNetInit failed: %d\n", net_init_result);
    pb_show_debug_screen();
  } else {
    char ip_address[32] = {0};
    char gateway[32] = {0};
    char netmask[32] = {0};
    debugPrint(
        "Network initialized:\nIP: %s\nGateway: %s\nNetmask: %s\n",
        ip4addr_ntoa_r(netif_ip4_addr(netif_default), ip_address,
                       sizeof(ip_address)),
        ip4addr_ntoa_r(netif_ip4_gw(netif_default), gateway, sizeof(gateway)),
        ip4addr_ntoa_r(netif_ip4_netmask(netif_default), netmask,
                       sizeof(netmask)));
    pb_show_debug_screen();

    ConnectAndSendTestFile();
  }

  // TODO: Uncomment after https://github.com/XboxDev/nxdk/pull/703 is merged.
  // if (nxNetShutdown()) {
  //   debugPrint("nxNetShutdown failed\n");
  // }

  debugPrint("Rebooting in 4 seconds...\n");
  pb_show_debug_screen();
  Sleep(4000);
  pb_kill();

  return 0;
}
