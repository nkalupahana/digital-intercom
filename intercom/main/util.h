#pragma once

#include <Arduino.h>

constexpr const char *TAG = "intercom";
#define STR(str) #str
#define STRING(str) STR(str)

inline void errorHang() {
  ESP_LOGE(TAG, "Encountered unrecoverable error. Hanging");
  while (true) {
    delay(100);
  }
}