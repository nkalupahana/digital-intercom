#pragma once

#include <cstdint>
#include <span>

namespace Crypto {
bool test(std::span<const uint8_t> deviceXY);
}