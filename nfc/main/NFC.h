#pragma once

#include "Slice.h"
#include <PN532.h>
#include <PN532_SPI.h>
#include <SPI.h>
#include <cstdint>
#include <optional>
#include <span>

namespace NFC {
bool setup();
bool writeRegister(uint16_t addr, uint8_t value);
bool inListPassiveTarget();
std::optional<ReadSlice> exchangeData(const char *pre,
                                      std::span<const uint8_t> toSend,
                                      std::span<uint8_t> recvBuf);
std::optional<ReadSlice> exchangeDataICT(const char *pre,
                                         std::span<const uint8_t> toSend,
                                         std::span<uint8_t> recvBuf);
bool exchangeDataICT(std::span<const uint8_t> toSend);
} // namespace NFC