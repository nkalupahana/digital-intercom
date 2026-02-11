#pragma once

#include <HardwareSerial.h>
#include <cstdint>
#include <span>

constexpr size_t PN532_PACKBUFFSIZ = 255;

inline void printHex(const char *pre, std::span<const uint8_t> data) {
  Serial.print(pre);
  for (const uint8_t byte : data) {
    Serial.printf("%02X", byte);
  }
  Serial.println();
}

constexpr const char *TAG = "nfc";
inline void errorHang() {
  ESP_LOGE(TAG, "Encountered unrecoverable error. Hanging");
  while (true) {
    delay(100);
  }
}