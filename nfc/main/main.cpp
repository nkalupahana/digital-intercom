// #define MIFAREDEBUG
// #define PN532DEBUG
#define RH_ESP32_USE_HSPI
#include "errors.h"

#include <PN532.h>
#include <PN532_SPI.h>
#include <tlv.h>

#include "../../../constants.h"
#include "Card.h"
#include "NFC.h"
#include "utils.h"
#include <RHReliableDatagram.h>
#include <RH_RF69.h>
#include <cstdint>
#include <mbedtls/sha256.h>
#include <optional>
#include <span>

// Radio
RH_RF69 driver(15, 26);
RHReliableDatagram manager(driver, RADIO_SCANNER_ADDRESS);
constexpr int RADIO_RESET_PIN = 16;

void setup() {
  Serial.begin(115200);

  pinMode(RADIO_RESET_PIN, OUTPUT);
  digitalWrite(RADIO_RESET_PIN, LOW);
  delay(100);
  digitalWrite(RADIO_RESET_PIN, HIGH);
  delay(100);
  digitalWrite(RADIO_RESET_PIN, LOW);
  delay(100);

  bool nfcInitialized = NFC::setup();
  if (!nfcInitialized) {
    ESP_LOGE(TAG, "Failed to initialize NFC");
    errorHang();
  }

  bool radioInitialized = manager.init();
  while (!radioInitialized) {
    ESP_LOGI(TAG, "Waiting for radio to initialize...");
    radioInitialized = manager.init();
    delay(1000);
  }

  driver.setTxPower(RADIO_POWER, true);
  driver.setFrequency(RADIO_FREQUENCY);

  ESP_LOGI(TAG, "Ready!");
}

void loop() {
  bool foundCard = NFC::inListPassiveTarget();

  if (foundCard) {
    ESP_LOGI(TAG, "Found something!");
    std::optional<ReadSlice> ppseOutputOpt = Card::checkIfValid();

    if (ppseOutputOpt) {
      const std::optional<std::span<const uint8_t>> track2DataOpt =
          Card::getTrack2Data(*ppseOutputOpt);
      CHECK_RETURN(track2DataOpt);
      const std::span<const uint8_t> track2Data = *track2DataOpt;
      printHex("Track 2 Equivalent Data: ", track2Data);
      CHECK_PRINT_RETURN("Track 2 Equivalent Data must be at least 8 bytes",
                         track2Data.size() >= 8);

      // Hash
      const int hashBufferSize =
          34; // 32 bytes for hash, 2 bytes for last 4 of track 2 data
      unsigned char output[hashBufferSize];
      int ret = mbedtls_sha256(track2Data.data(), track2Data.size(), output, 0);
      CHECK_PRINT_RETURN("Failed to hash data", ret == 0);

      // Add last 4 to the end
      output[hashBufferSize - 2] = track2Data[6];
      output[hashBufferSize - 1] = track2Data[7];

      // Send
      bool success =
          manager.sendtoWait(output, hashBufferSize, RADIO_INTERCOM_ADDRESS);
      if (success) {
        ESP_LOGI(TAG, "Data sent successfully");
      } else {
        ESP_LOGE(TAG, "Data send failed");
      }

      delay(3000);
    } else {
      ESP_LOGE(TAG, "Tap from unknown card type");
    }
  } else {
    bool success = Card::sendECPFrame();
    CHECK_PRINT_RETURN("Failed to send ECP frame", success);
  }
}