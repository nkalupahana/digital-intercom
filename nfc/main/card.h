#pragma once

#include "Slice.h"
#include <cstdint>
#include <optional>
#include <span>
#include <tlv.h>

namespace Card {
bool sendECPFrame();
bool tryCheckmark(TLVS &tlvs, WriteSlice &writeSlice, std::span<uint8_t> rbuf,
                  WriteSlice &track2Slice);
std::optional<std::span<const uint8_t>> getTrack2Data();
} // namespace Card