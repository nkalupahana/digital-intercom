#pragma once

#include "Slice.h"
#include <optional>
#include <tlv.h>

namespace DigitalID {
void setupBLEServer();

bool checkIfValid();
std::optional<ReadSlice> performHandoff();
} // namespace DigitalID