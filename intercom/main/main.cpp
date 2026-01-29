#include <Arduino.h>
#include <AudioTools.h>
#include "AudioTools/Communication/UDPStream.h"
#include <Adafruit_TLV320DAC3100.h>
#include <ESP_I2S.h>
#include <RadioHead.h>

// Open door
constexpr int DOOR_RELAY_PIN = 25;
constexpr int OPEN_DOOR_TIME = 1000;

// Listen
AudioInfo info(22050, 1, 16);
AnalogAudioStream audioInAnalog;
UDPStream audioOutUdp(WIFI_SSID, WIFI_PASSWORD);
StreamCopy audioOutCopier(audioOutUdp, audioInAnalog);
constexpr int LISTEN_RELAY_PIN = 33;

enum class State {
  IDLE,
  OPEN_DOOR,
  LISTEN
};

State state = State::LISTEN;

void setup() {
  Serial.begin(115200);
  Serial.println("Initializing digital intercom...");

  // Open door
  pinMode(DOOR_RELAY_PIN, OUTPUT);
  digitalWrite(DOOR_RELAY_PIN, LOW);

  // Listen
  pinMode(LISTEN_RELAY_PIN, OUTPUT);
  digitalWrite(LISTEN_RELAY_PIN, LOW);
  AudioToolsLogger.begin(Serial, AudioToolsLogLevel::Info);
  audioOutUdp.begin(UDP_TARGET_IP, atoi(UDP_TARGET_PORT)); // TODO: do not atoi

  auto analogInConfig = audioInAnalog.defaultConfig(RX_MODE);  
  analogInConfig.copyFrom(info);
  analogInConfig.channels = 1;
  audioInAnalog.begin(analogInConfig);

  Serial.println("Digital intercom initialized.");
}

void loop() {
  if (state == State::IDLE) {
    /// Reset everything to base state
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