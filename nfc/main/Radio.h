#pragma once

#include <cstdint>
#include <span>

namespace Radio {
enum class MessageType {
  HASHED_TRACK_2 = 'H',
  DIGITAL_ID = 'D',
};

bool setup();
bool send(const std::span<uint8_t> data);
} // namespace Radio