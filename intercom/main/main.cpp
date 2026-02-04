#include "../../../constants.h"
#include "talk.h"
#include "tcpClient.h"
#include "util.h"

#include <Arduino.h>
#include <AudioTools.h>
#include <AudioTools/Communication/UDPStream.h>
#include <AudioTools/CoreAudio/AudioOutput.h>
#include <AudioTools/CoreAudio/AudioStreams.h>
#include <AudioTools/CoreAudio/AudioTypes.h>
#include <RHReliableDatagram.h>
#include <RH_RF69.h>

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

enum class State { IDLE, LISTEN, TALK };
State state = State::IDLE;

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
  setupTalk();

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
    case Command::HEARTBEAT:
      break;
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

        if (len != CREDIT_CARD_DATA_LEN) {
          ESP_LOGE(TAG, "Received invalid credit card data (actual length: %d). Not sending", len);
        } else {
          sendCreditCardEvent(std::span<uint8_t, CREDIT_CARD_DATA_LEN>{
              msgBuf, CREDIT_CARD_DATA_LEN});
        }
      }
    }

    // Doorbell
    audioMonitorCopier.copy();
    if (volumeMeter.volume() > DOORBELL_TRIGGER_VOLUME &&
        millis() - last_trigger_time > DOORBELL_REPEAT_TIME) {
      last_trigger_time = millis();
      ESP_LOGI(TAG, "Doorbell triggered!");
      sendBuzzerEvent();
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
    // TODO: need to edit to I2S_NUM_1 in ESP_I2S.cpp to get this to work
    if (count % halfWavelength == 0) {
      sample = -1 * sample;
    }

    // TODO: does not work with delay(10), comment that out if you're testing
    // dac_i2s.write(sample);
    // count++;
    break;
  }
  }
  delay(10);
}