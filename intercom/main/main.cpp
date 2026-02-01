#include "AudioTools/Communication/UDPStream.h"
#include "AudioTools/CoreAudio/AudioOutput.h"
#include "AudioTools/CoreAudio/AudioTypes.h"
#include <Adafruit_TLV320DAC3100.h>
#include <Arduino.h>
#include <AudioTools.h>
#include <ESP_I2S.h>
#include <RHReliableDatagram.h>
#include <RH_RF69.h>
#include "../../../constants.h"

// Idle - Radio
RH_RF69 driver(SS, 26);
RHReliableDatagram manager(driver, RADIO_INTERCOM_ADDRESS);
uint8_t msgBuf[RH_RF69_MAX_MESSAGE_LEN];
constexpr int RADIO_RESET_PIN = 16;

// Open door
constexpr int DOOR_RELAY_PIN = 25;
constexpr int OPEN_DOOR_TIME = 1000;

// Listen
constexpr float AUDIO_SCALE = 20;
AudioInfo info(22050, 1, 16);
AnalogAudioStream audioInAnalog;
UDPStream audioOutUdp(WIFI_SSID, WIFI_PASSWORD);
VolumeStream volume((AudioOutput &)audioOutUdp);
StreamCopy audioOutCopier(volume, audioInAnalog);
constexpr int LISTEN_RELAY_PIN = 33;

enum class State { IDLE, OPEN_DOOR, LISTEN };

State state = State::IDLE;

void setup() {
  Serial.begin(115200);
  Serial.println("Initializing digital intercom...");

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
    Serial.println("Waiting for radio to initialize...");
    delay(1000);
  }

  driver.setTxPower(RADIO_POWER, true);
  driver.setFrequency(RADIO_FREQUENCY);

  // Open door
  pinMode(DOOR_RELAY_PIN, OUTPUT);
  digitalWrite(DOOR_RELAY_PIN, LOW);

  // Listen
  pinMode(LISTEN_RELAY_PIN, OUTPUT);
  digitalWrite(LISTEN_RELAY_PIN, LOW);
  AudioToolsLogger.begin(Serial, AudioToolsLogLevel::Info);
  audioOutUdp.begin(UDP_TARGET_IP, atoi(UDP_TARGET_PORT)); // TODO: do not atoi

  auto config_vol = volume.defaultConfig();
  config_vol.copyFrom(info);
  config_vol.allow_boost = true;
  config_vol.volume = AUDIO_SCALE;
  volume.begin(config_vol);

  auto analogInConfig = audioInAnalog.defaultConfig(RX_MODE);
  analogInConfig.copyFrom(info);
  analogInConfig.channels = 1;
  audioInAnalog.begin(analogInConfig);

  Serial.println("Digital intercom initialized.");
}

void loop() {
  if (state == State::IDLE) {
    /// Reset everything to base state
    // Radio
    if (manager.available()) {
      Serial.println("Radio message received.");
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

    // Open door
    digitalWrite(DOOR_RELAY_PIN, LOW);

    // Listen
    digitalWrite(LISTEN_RELAY_PIN, LOW);

    // TODO: Listen for tone in
  } else if (state == State::OPEN_DOOR) {
    Serial.println("Opening door...");
    digitalWrite(DOOR_RELAY_PIN, HIGH);
    delay(OPEN_DOOR_TIME);
    digitalWrite(DOOR_RELAY_PIN, LOW);

    Serial.println("Door opened, returning to idle.");
    state = State::IDLE;
  } else if (state == State::LISTEN) {
    digitalWrite(LISTEN_RELAY_PIN, HIGH);
    audioOutCopier.copy();
  }

  delay(10);
}