// #define MIFAREDEBUG
// #define PN532DEBUG
#include "errors.h"

#include "Card.h"
#include "Crypto.h"
#include "DigitalID.h"
#include "NFC.h"
#include "Radio.h"
#include "utils.h"
#include <cstdint>
#include <mbedtls/sha256.h>
#include <optional>
#include <span>

void setup() {
  Serial.begin(115200);

  bool nfcInitialized = NFC::setup();
  if (!nfcInitialized) {
    ESP_LOGE(TAG, "Failed to initialize NFC");
    errorHang();
  }

  bool radioInitialized = Radio::setup();
  if (!radioInitialized) {
    ESP_LOGE(TAG, "Failed to initialize radio");
    errorHang();
  }

  Crypto::setup();
  DigitalID::setupBLEServer();

  ESP_LOGI(TAG, "Ready!");
}

void loop() {
  bool foundCard = NFC::inListPassiveTarget();

  if (foundCard) {
    ESP_LOGI(TAG, "Found something!");
    std::optional<ReadSlice> ppseOutputOpt = Card::checkIfValid();

    if (ppseOutputOpt) {
      ESP_LOGI(TAG, "Card is valid");
      const std::optional<std::span<const uint8_t>> track2DataOpt =
          Card::getTrack2Data(*ppseOutputOpt);
      CHECK_RETURN(track2DataOpt);
      const std::span<const uint8_t> track2Data = *track2DataOpt;
      printHex("Track 2 Equivalent Data: ", track2Data);
      CHECK_PRINT_RETURN("Track 2 Equivalent Data must be at least 8 bytes",
                         track2Data.size() >= 8);

      // Hash
      const int hashBufferSize = 35; // 1 byte for message type, 32 bytes for
                                     // hash, 2 bytes for last 4 of track 2 data
      uint8_t output[hashBufferSize];
      output[0] = static_cast<uint8_t>(Radio::MessageType::HASHED_TRACK_2);
      int ret =
          mbedtls_sha256(track2Data.data(), track2Data.size(), output + 1, 0);
      CHECK_PRINT_RETURN("Failed to hash data", ret == 0);

      // Add last 4 to the end
      output[hashBufferSize - 2] = track2Data[7];
      output[hashBufferSize - 1] = track2Data[8];

      // Send
      bool success = Radio::send({output, hashBufferSize});
      if (success) {
        ESP_LOGI(TAG, "Track 2 Hashed Data sent successfully");
      } else {
        ESP_LOGE(TAG, "Track 2 Hashed Data send failed");
      }

      delay(3000);
      return;
    }

    bool digitalIDValid = DigitalID::checkIfValid();
    if (digitalIDValid) {
      ESP_LOGI(TAG, "Digital ID is valid");
      DigitalID::performHandoff();
      delay(3000);
      return;
    }

    ESP_LOGE(TAG, "Unknown card type");
    delay(500);
  } else {
    bool success = Card::sendECPFrame();
    CHECK_PRINT_RETURN("Failed to send ECP frame", success);
  }
}