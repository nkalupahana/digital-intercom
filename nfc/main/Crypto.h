#pragma once

#include "NimBLECharacteristic.h"
#include <cstdint>
#include <span>

namespace Crypto {
bool setIdent(NimBLECharacteristic *identCharacteristic,
              std::span<const uint8_t> encodedDevicePublicKey);
bool test(std::span<const uint8_t> deviceXY);
} // namespace Crypto