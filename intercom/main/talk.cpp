#include "talk.h"
#include "util.h"

#include <Adafruit_TLV320DAC3100.h>
#include <Arduino.h>
#include <AudioTools.h>
#include <ESP_I2S.h>
#include <WiFiUdp.h>

Adafruit_TLV320DAC3100 dac;
I2SClass dac_i2s;

void setupTalk(WiFiUDP& talkUdp) {
  // Relay
  pinMode(TALK_RELAY_PIN, OUTPUT);
  digitalWrite(TALK_RELAY_PIN, LOW);

  // DAC
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
  if (!dac.setCodecInterface(TLV320DAC3100_FORMAT_I2S,
                             TLV320DAC3100_DATA_LEN_16)) {
    ESP_LOGE(TAG, "Failed to configure codec interface!");
    errorHang();
  }

  if (!dac.setCodecClockInput(TLV320DAC3100_CODEC_CLKIN_PLL) ||
      !dac.setPLLClockInput(TLV320DAC3100_PLL_CLKIN_BCLK)) {
    ESP_LOGE(TAG, "Failed to configure codec clocks!");
    errorHang();
  }

  if (!dac.setPLLValues(1, 1, 8, 0)) {
    ESP_LOGE(TAG, "Failed to configure PLL values!");
    errorHang();
  }

  if (!dac.setNDAC(true, 8) || !dac.setMDAC(true, 2) || !dac.setDOSR(128)) {
    ESP_LOGE(TAG, "Failed to configure DAC dividers!");
    errorHang();
  }

  if (!dac.powerPLL(true)) {
    Serial.println("Failed to power up PLL!");
    errorHang();
  }

  if (!dac.setDACDataPath(true, true, TLV320_DAC_PATH_NORMAL,
                          TLV320_DAC_PATH_NORMAL, TLV320_VOLUME_STEP_1SAMPLE)) {
    ESP_LOGE(TAG, "Failed to configure DAC data path!");
    errorHang();
  }

  if (!dac.configureAnalogInputs(TLV320_DAC_ROUTE_MIXER, // Left DAC to mixer
                                 TLV320_DAC_ROUTE_MIXER, // Right DAC to mixer
                                 false, false, false,    // No AIN routing
                                 false)) {               // No HPL->HPR
    Serial.println("Failed to configure DAC routing!");
  }

  if (!dac.setDACVolumeControl(
          false, false, TLV320_VOL_INDEPENDENT) || // Unmute both channels TODO:
                                                   // only unmute one channel
      !dac.setChannelVolume(
          false, 18) ||
      !dac.setChannelVolume(true, 18)) {
    ESP_LOGE(TAG, "Failed to configure DAC volume control!");
    errorHang();
  }

  if (!dac.configureHeadphoneDriver(
          true, true,                   // Power up both drivers
          TLV320_HP_COMMON_1_35V,       // Default common mode
          false) ||                     // Don't power down on SCD
      !dac.configureHPL_PGA(0, true) || // Set HPL gain, unmute
      !dac.configureHPR_PGA(0, true) || // Set HPR gain, unmute
      !dac.setHPLVolume(true, 6) ||     // Enable and set HPL volume
      !dac.setHPRVolume(true, 6)) {     // Enable and set HPR volume
    ESP_LOGE(TAG, "Failed to configure headphone outputs!");
    errorHang();
  }

  if (!dac.setChannelVolume(false, -20) || !dac.setChannelVolume(true, -20)) {
    ESP_LOGE(TAG, "Failed to set DAC channel volumes!");
    errorHang();
  }

  if (!dac.enableSpeaker(false)) {
    ESP_LOGE(TAG, "Failed to disable speaker!");
    errorHang();
  }

  dac_i2s.setPins(DAC_BCLK_PIN, DAC_WS_PIN, DAC_DOUT_PIN);
  if (!dac_i2s.begin(I2S_MODE_STD, DAC_SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT,
                     I2S_SLOT_MODE_MONO)) {
    ESP_LOGE(TAG, "Failed to initialize I2S!");
  }

  // UDP
  int udpResult = talkUdp.begin(AUDIO_IN_PORT);
  if (udpResult != 1) {
    ESP_LOGE(TAG, "Failed to start UDP server on port %d", AUDIO_IN_PORT);
    errorHang();
  }
}

void writeAudioSamples(uint8_t* audioBuffer, size_t packetSize) {
  dac_i2s.write(audioBuffer, packetSize);
}