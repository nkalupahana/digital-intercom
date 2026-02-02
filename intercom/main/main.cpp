#include "../../../constants.h"
#include "AudioTools/CoreAudio/AudioStreams.h"
#include <Adafruit_TLV320DAC3100.h>
#include <Arduino.h>
#include <AudioTools.h>
#include <AudioTools/Communication/UDPStream.h>
#include <AudioTools/CoreAudio/AudioOutput.h>
#include <AudioTools/CoreAudio/AudioTypes.h>
#include <ESP_I2S.h>
#include <RHReliableDatagram.h>
#include <RH_RF69.h>
#include <cerrno>
#include <cstring>
#include <esp_netif.h>
#include <optional>
#include <sys/socket.h>
#include <unistd.h>

#define STR(str) #str
#define STRING(str) STR(str)

constexpr const char *TAG = "intercom";

// Idle - Radio
RH_RF69 driver(SS, 26);
RHReliableDatagram manager(driver, RADIO_INTERCOM_ADDRESS);
uint8_t msgBuf[RH_RF69_MAX_MESSAGE_LEN];
constexpr int RADIO_RESET_PIN = 16;

// Idle - Doorbell
constexpr float DOORBELL_TRIGGER_VOLUME = 1500;
constexpr int DOORBELL_REPEAT_TIME = 5000;

// Open door
constexpr int DOOR_RELAY_PIN = 25;
constexpr int OPEN_DOOR_TIME = 1000;

// Listen
constexpr float AUDIO_SCALE = 20;
AudioInfo info(32000, 1, 16);
AnalogAudioStream audioInAnalog;
UDPStream audioOutUdp(STRING(WIFI_SSID), STRING(WIFI_PASSWORD));
VolumeStream volume((AudioOutput &)audioOutUdp);
StreamCopy audioOutCopier(volume, audioInAnalog);
VolumeMeter volumeMeter;
StreamCopy audioMonitorCopier(volumeMeter, audioInAnalog);
constexpr int LISTEN_RELAY_PIN = 33;

// TCP Server
int tcpSocket = -1;

enum class State { IDLE, LISTEN };
enum class Command {
  OPEN_DOOR = 'D',
  LISTEN_ON = 'L',
  LISTEN_STOP = 'S',
  RESET = 'R', // Internal only command. Not sent by the TCP server
};

State state = State::IDLE;

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
  default:
    ESP_LOGE(TAG,
             "Got unexpected command. Going to disconnect and reconnect: %c",
             cmd);
    connectToTCPServer();
    return Command::RESET;
  }
}

void setup() {
  Serial.begin(115200);
  ESP_LOGI(TAG, "Initializing digital intercom...");

  // Idle - Radio
  pinMode(RADIO_RESET_PIN, OUTPUT);
  digitalWrite(RADIO_RESET_PIN, LOW);
  delay(100);
  digitalWrite(RADIO_RESET_PIN, HIGH);
  delay(100);
  digitalWrite(RADIO_RESET_PIN, LOW);
  delay(100);
  bool radioInitialized = manager.init();
  if (!radioInitialized) {
    ESP_LOGI(TAG, "Waiting for radio to initialize...");
    delay(1000);
  }
  ESP_LOGI(TAG, "Bridge IP: %s\n", STRING(BRIDGE_IP));

  driver.setTxPower(RADIO_POWER, true);
  driver.setFrequency(RADIO_FREQUENCY);

  // Open door
  pinMode(DOOR_RELAY_PIN, OUTPUT);
  digitalWrite(DOOR_RELAY_PIN, LOW);

  ESP_LOGI(TAG, "Setting up Audio Tools");
  // Listen
  pinMode(LISTEN_RELAY_PIN, OUTPUT);
  digitalWrite(LISTEN_RELAY_PIN, LOW);
  AudioToolsLogger.begin(Serial, AudioToolsLogLevel::Warning);
  audioOutUdp.begin(STRING(BRIDGE_IP), AUDIO_OUT_PORT);

  auto config_vol = volume.defaultConfig();
  config_vol.copyFrom(info);
  config_vol.allow_boost = true;
  config_vol.volume = AUDIO_SCALE;
  volume.begin(config_vol);

  auto analogInConfig = audioInAnalog.defaultConfig(RX_MODE);
  analogInConfig.copyFrom(info);
  analogInConfig.channels = 1;
  audioInAnalog.begin(analogInConfig);

  // Idle - doorbell
  volumeMeter.begin(analogInConfig);

  ESP_LOGI(TAG, "Setting up TCP Server");
  connectToTCPServer();

  ESP_LOGI(TAG, "Digital intercom initialized.");
}

int last_trigger_time = 0;

void loop() {
  std::optional<Command> cmd = getCommand();
  if (cmd) {
    ESP_LOGI(TAG, "Got command: %c\n", *cmd);
    switch (*cmd) {
    case Command::OPEN_DOOR: {
      ESP_LOGI(TAG, "Opening door...");
      digitalWrite(DOOR_RELAY_PIN, HIGH);
      delay(OPEN_DOOR_TIME);
      digitalWrite(DOOR_RELAY_PIN, LOW);
      ESP_LOGI(TAG, "Door opened");
      break;
    }
    case Command::LISTEN_ON: {
      if (state == State::LISTEN) {
        ESP_LOGW(TAG, "Setting state to LISTEN when already in LISTEN");
      }
      state = State::LISTEN;
      break;
    }
    case Command::LISTEN_STOP:
    case Command::RESET: {
      if (state == State::IDLE) {
        ESP_LOGW(TAG, "Setting state to IDLE when already in IDLE");
      }
      state = State::IDLE;
      break;
    }
    }
  }

  switch (state) {
  case State::IDLE: {
    /// Reset everything to base state
    // Radio
    if (manager.available()) {
      ESP_LOGI(TAG, "Radio message received.");
      uint8_t len = sizeof(msgBuf);
      uint8_t from;

      if (manager.recvfromAck(msgBuf, &len, &from)) {
        Serial.print("got request from : 0x");
        Serial.print(from, HEX);
        Serial.print(": ");
        for (int i = 0; i < len; i++) {
          Serial.print(msgBuf[i], HEX);
        }
        Serial.println();
      }
    }

    // Doorbell
    audioMonitorCopier.copy();
    if (volumeMeter.volume() > DOORBELL_TRIGGER_VOLUME &&
        millis() - last_trigger_time > DOORBELL_REPEAT_TIME) {
      last_trigger_time = millis();
      ESP_LOGI(TAG, "Doorbell triggered!");
      // TODO: send TCP message to bridge about doorbell
    }

    // Open door
    digitalWrite(DOOR_RELAY_PIN, LOW);

    // Listen
    digitalWrite(LISTEN_RELAY_PIN, LOW);
    break;
  }
  case State::LISTEN: {
    digitalWrite(LISTEN_RELAY_PIN, HIGH);
    audioOutCopier.copy();
    break;
  }
  }
  delay(10);
}