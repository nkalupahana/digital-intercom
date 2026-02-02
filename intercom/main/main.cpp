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
#define HANG() while (true) { delay(100); }

constexpr const char *TAG = "intercom";

// Idle - Radio
constexpr int RADIO_IRQ_PIN = 26;
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

// Talk
Adafruit_TLV320DAC3100 dac;
I2SClass dac_i2s;
constexpr int DAC_BCLK_PIN = 14;
constexpr int DAC_WS_PIN = 15;
constexpr int DAC_DOUT_PIN = 27;
constexpr int DAC_RESET_PIN = 17;
constexpr int DAC_SAMPLE_RATE = 44100;
constexpr int TALK_RELAY_PIN = 32;

// TCP Server
int tcpSocket = -1;

enum class State { IDLE, LISTEN, TALK };
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

  // Talk
  pinMode(TALK_RELAY_PIN, OUTPUT);
  digitalWrite(TALK_RELAY_PIN, LOW);

  pinMode(DAC_RESET_PIN, OUTPUT);
  digitalWrite(DAC_RESET_PIN, LOW);
  delay(100);
  digitalWrite(DAC_RESET_PIN, HIGH);
  delay(100);
  bool dacInitialized = dac.begin();
  if (!dacInitialized) {
    ESP_LOGI(TAG, "Waiting for DAC to initialize...");
    delay(1000);
  }
  dac.reset();
  if (!dac.setCodecInterface(TLV320DAC3100_FORMAT_I2S, TLV320DAC3100_DATA_LEN_16)) {
    ESP_LOGE(TAG, "Failed to configure codec interface!");
    HANG();
  }

  if (!dac.setCodecClockInput(TLV320DAC3100_CODEC_CLKIN_PLL) ||
      !dac.setPLLClockInput(TLV320DAC3100_PLL_CLKIN_BCLK)) {
    ESP_LOGE(TAG, "Failed to configure codec clocks!");
    HANG();
  }

  if (!dac.setPLLValues(1, 1, 8, 0)) {
    ESP_LOGE(TAG, "Failed to configure PLL values!");
    HANG();
  }

  if (!dac.setNDAC(true, 8) ||
      !dac.setMDAC(true, 2) ||
      !dac.setDOSR(128)) {
    ESP_LOGE(TAG, "Failed to configure DAC dividers!");
    HANG();
  }

  if (!dac.powerPLL(true)) {
    Serial.println("Failed to power up PLL!");
    HANG();
  }

  if (!dac.setDACDataPath(true, true, 
    TLV320_DAC_PATH_NORMAL,
    TLV320_DAC_PATH_NORMAL,
    TLV320_VOLUME_STEP_1SAMPLE)) {
    ESP_LOGE(TAG, "Failed to configure DAC data path!");
    HANG();
  }

  if (!dac.configureAnalogInputs(TLV320_DAC_ROUTE_MIXER, // Left DAC to mixer
    TLV320_DAC_ROUTE_MIXER, // Right DAC to mixer
    false, false, false,    // No AIN routing
    false)) {               // No HPL->HPR
    Serial.println("Failed to configure DAC routing!");
  }

  if (!dac.setDACVolumeControl(
    false, false, TLV320_VOL_INDEPENDENT) || // Unmute both channels TODO: only unmute one channel
    !dac.setChannelVolume(false, 0) ||        // Left DAC +0dB TODO figure out the correct volume
    !dac.setChannelVolume(true, 0)) {         // Right DAC +0dB
    ESP_LOGE(TAG, "Failed to configure DAC volume control!");
    HANG();
  }

  if (!dac.configureHeadphoneDriver(
    true, true,                     // Power up both drivers
    TLV320_HP_COMMON_1_35V,         // Default common mode
    false) ||                       // Don't power down on SCD
    !dac.configureHPL_PGA(0, true) || // Set HPL gain, unmute
    !dac.configureHPR_PGA(0, true) || // Set HPR gain, unmute
    !dac.setHPLVolume(true, 6) ||     // Enable and set HPL volume
    !dac.setHPRVolume(true, 6)) {     // Enable and set HPR volume
    ESP_LOGE(TAG, "Failed to configure headphone outputs!");
    HANG();
  }

  if (!dac.setChannelVolume(false, -40) ||
  !dac.setChannelVolume(true, -40)) {
    ESP_LOGE(TAG, "Failed to set DAC channel volumes!");
    HANG();
  }

  if (!dac.enableSpeaker(false)
  ) {
    ESP_LOGE(TAG, "Failed to disable speaker!");
    HANG();
  }

  dac_i2s.setPins(DAC_BCLK_PIN, DAC_WS_PIN, DAC_DOUT_PIN);
  if (!dac_i2s.begin(I2S_MODE_STD, DAC_SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO)) {
    ESP_LOGE(TAG, "Failed to initialize I2S!");
  }

  ESP_LOGI(TAG, "Setting up TCP Server");
  connectToTCPServer();

  ESP_LOGI(TAG, "Digital intercom initialized.");
}

int last_trigger_time = 0;
const int amplitude = 5000;
const int frequency = 440;
const int halfWavelength = (DAC_SAMPLE_RATE / frequency);
int16_t sample = amplitude; // current sample value
int count = 0;

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

    // Talk
    digitalWrite(TALK_RELAY_PIN, LOW);
    break;
  }
  case State::LISTEN: {
    digitalWrite(LISTEN_RELAY_PIN, HIGH);
    audioOutCopier.copy();
    break;
  }
  case State::TALK: {
    digitalWrite(TALK_RELAY_PIN, HIGH);
    // TODO: send real audio to DAC
    if (count % halfWavelength == 0) {
      sample = -1 * sample;
    }
  
    // TODO: does not work with delay(10), comment that out if you're testing
    dac_i2s.write(sample);
    count++;
    break;
  }
  }
  delay(10);
}