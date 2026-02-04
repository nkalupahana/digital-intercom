#include "tcpClient.h"
#include "util.h"

#include <cerrno>
#include <cstring>
#include <esp_netif.h>
#include <optional>
#include <sys/socket.h>
#include <unistd.h>

int tcpSocket = -1;

void connectToTCPServer() {
  if (tcpSocket > 0) {
    ESP_LOGI(TAG, "Closing existing TCP socket");
    close(tcpSocket);
  }

  struct sockaddr_in dest_addr;
  dest_addr.sin_addr.s_addr = inet_addr(STRING(BRIDGE_IP));
  dest_addr.sin_family = AF_INET;
  dest_addr.sin_port = htons(TCP_PORT);

  while (true) {
    ESP_LOGI(TAG, "Creating TCP socket");
    tcpSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (tcpSocket < 0) {
      ESP_LOGE(TAG, "Unable to create socket. Trying again: %s\n",
               strerror(errno));
      continue;
    }
    ESP_LOGI(TAG, "Trying to connect to %s:%d", STRING(BRIDGE_IP), TCP_PORT);
    int result =
        connect(tcpSocket, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (result == 0) {
      break;
    }
    ESP_LOGE(TAG, "Unable to connect to %s:%d: %s", STRING(BRIDGE_IP), TCP_PORT,
             strerror(errno));
    close(tcpSocket);
    delay(1000);
  }
  ESP_LOGI(TAG, "Successfully connected to %s:%d", STRING(BRIDGE_IP), TCP_PORT);
}

std::optional<Command> getCommand() {
  char cmd = '\x00';
  int result = recv(tcpSocket, &cmd, 1, MSG_DONTWAIT);
  if (result == 0) {
    // 0 means socket a peer performed an orderly shutdown, but we still want to
    // connect
    ESP_LOGE(TAG,
             "Peer performed an orderly shutdown. Attempting to reconnect");
    connectToTCPServer();
    return Command::RESET;
  }
  if (result <= 0) {
    if (errno == EAGAIN && errno == EWOULDBLOCK) {
      return std::nullopt;
    }
    ESP_LOGE(TAG, "Failed to recv data %s. Attempting to reconnect",
             strerror(errno));
    connectToTCPServer();
    return Command::RESET;
  }
  ESP_LOGI(TAG, "Got cmd %c %d", cmd, result);
  switch (cmd) {
  case (char)Command::OPEN_DOOR:
    return Command::OPEN_DOOR;
  case (char)Command::LISTEN_ON:
    return Command::LISTEN_ON;
  case (char)Command::LISTEN_STOP:
    return Command::LISTEN_STOP;
  case (char)Command::TALK_ON:
    return Command::TALK_ON;
  default:
    ESP_LOGE(TAG,
             "Got unexpected command. Going to disconnect and reconnect: %c",
             cmd);
    connectToTCPServer();
    return Command::RESET;
  }
}

void sendBuzzerEvent() {
  char value = (char)OutputEvent::BUZZER;
  write(tcpSocket, &value, 1);
}
void sendCreditCardEvent(std::span<uint8_t, CREDIT_CARD_DATA_LEN> buffer) {
  char value = (char)OutputEvent::CREDIT_CARD;
  write(tcpSocket, &value, 1);
  write(tcpSocket, buffer.data(), buffer.size());
}
