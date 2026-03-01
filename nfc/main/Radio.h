#pragma once

#include <cstdint>
#include <span>

namespace Radio {
enum class MessageType {
  CREDIT_CARD = 'C',
  DIGITAL_ID = 'D',
};

bool setup();
bool send(const std::span<uint8_t> data);
} // namespace Radio