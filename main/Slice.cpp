#include "Slice.h"
#include "HardwareSerial.h"
#include <cstdint>
#include <cstring>
#include <initializer_list>

ReadSlice::ReadSlice(const uint8_t *data, size_t len)
    : data_(data), len_(len) {}

size_t ReadSlice::len() const { return len_; }
const uint8_t *ReadSlice::data() const { return data_; }

uint8_t ReadSlice::readByte() {
  if (len_ < 1) {
    Serial.printf("ERROR: Not enough data to read byte\n");
    // Returning 0 is sus, but this makes the API easier to use, and we'll
    // see a print showing that something is wrong
    return 0;
  }

  uint8_t value = *data_;
  data_++;
  len_--;
  return value;
}

uint8_t ReadSlice::readByteFromEnd() {
  if (len_ < 1) {
    Serial.printf("ERROR: Not enough data to read byte from end\n");
    // Returning 0 is sus, but this makes the API easier to use, and we'll
    // see a print showing that something is wrong
    return 0;
  }

  len_--;
  uint8_t value = data_[len_];
  return value;
}

bool ReadSlice::windowToPN532Response() {
  if (len_ < 8) {
    Serial.printf("ERROR: Not enough data to window to PN532 response\n");
    return false;
  }
  if (readByte() != 0x00 || readByte() != 0x00 || readByte() != 0xFF) {
    Serial.printf("ERROR: Invalid PN532 preamble or start code\n");
    return false;
  }
  size_t len = readByte();
  uint8_t lenChecksum = readByte();
  if ((uint8_t)(len + lenChecksum) != 0x00) {
    Serial.printf("ERROR: Invalid PN532 length checksum\n");
    return false;
  }
  if (len_ < len + 2) {
    Serial.printf("ERROR: Not enough data for PN532 response\n");
    return false;
  }
  len_ = len + 2;
  if (uint8_t tfi = readByte() != 0xD5) {
    Serial.printf("ERROR: Invalid PN532 TFI %d\n", tfi);
    return false;
  }
  if (uint8_t postamble = readByteFromEnd() != 0) {
    Serial.printf("ERROR: Invalid PN532 postamble %d\n", postamble);
    return false;
  }
  uint8_t _ = readByteFromEnd(); // checksum

  return true;
}

WriteSlice::WriteSlice(uint8_t *data, size_t len)
    : data_(data), len_(len), startingLen_(len) {}

size_t WriteSlice::len() const { return startingLen_ - len_; }
uint8_t *WriteSlice::data() const { return data_ - len(); }

void WriteSlice::reset() {
  data_ = data_ - len();
  len_ = startingLen_;
}

void WriteSlice::appendUnsafe(const std::initializer_list<uint8_t> &data) {
  size_t dataLen = data.size();
  memcpy(data_, data.begin(), dataLen);
  data_ += dataLen;
  len_ -= dataLen;
}

void WriteSlice::appendUnsafe(const uint8_t *data, size_t len) {
  memcpy(data_, data, len);
  data_ += len;
  len_ -= len;
}

bool WriteSlice::fill(uint8_t value, size_t count) {
  if (len_ < count) {
    Serial.println("ERROR: Not enough space to fill data\n");
    return false;
  }

  memset(data_, value, count);
  data_ += count;
  len_ -= count;
  return true;
}

uint8_t *WriteSlice::deferAppend(size_t len) {
  if (len_ < len) {
    Serial.printf("ERROR: Not enough space to defer append - bufferLen: %zu, "
                  "dataLen: %zu\n",
                  len_, len);
    return nullptr;
  }

  uint8_t *ptr = data_;
  data_ += len;
  len_ -= len;
  return ptr;
}

bool WriteSlice::append(const std::initializer_list<uint8_t> &data) {
  if (len_ < data.size()) {
    // clang-format off
      Serial.printf("ERROR: Not enough space to append data - bufferLen: %zu, dataLen: %zu\n", len_, data.size());
    // clang-format on
    return false;
  }

  appendUnsafe(data);
  return true;
}

bool WriteSlice::append(const uint8_t *data, size_t len) {
  if (len_ < len) {
    Serial.printf("ERROR: Not enough space to append data - bufferLen: %zu, "
                  "dataLen: %zu\n",
                  len_, len);
    return false;
  }

  appendUnsafe(data, len);
  return true;
}

bool WriteSlice::appendApduCommand(uint8_t cla, uint8_t ins, uint8_t p1,
                                   uint8_t p2,
                                   const std::initializer_list<uint8_t> &data) {
  return appendApduCommand(cla, ins, p1, p2, data.begin(), data.size());
}

bool WriteSlice::appendApduCommand(uint8_t cla, uint8_t ins, uint8_t p1,
                                   uint8_t p2, const char *cmd) {
  return appendApduCommand(cla, ins, p1, p2,
                           reinterpret_cast<const uint8_t *>(cmd), strlen(cmd));
}

bool WriteSlice::appendApduCommand(uint8_t cla, uint8_t ins, uint8_t p1,
                                   uint8_t p2, const uint8_t *data,
                                   size_t len) {
  // CLA + INS + P1 + P2 + Lc + Data + Le
  size_t numToWrite = 5 + len + 1;
  if (len_ < numToWrite || len > 0xFF) {
    Serial.printf("ERROR: Not enough space to write APDU command\n");
    return false;
  }

  appendUnsafe({cla, ins, p1, p2, (uint8_t)len});
  appendUnsafe(data, len);
  appendUnsafe({0x00}); // Le

  return true;
}

bool WriteSlice::appendFromDol(ReadSlice &dol) {
  while (dol.len() > 0) {
    uint16_t tag = dol.readByte();
    // This check is AI generated, but it seems correct by emprically checking
    if ((tag & 0x1F) == 0x1F) {
      tag = (tag << 8) | dol.readByte();
    }

    uint8_t requiredLen = dol.readByte();

    auto validateAndAppend = [&](std::initializer_list<uint8_t> data) {
      if (data.size() != requiredLen) {
        // clang-format off
          Serial.printf("ERROR: DOL length mismatch - tag: %04X - requiredLen: %u - dataLen: %zu\n", tag, requiredLen, data.size());
        // clang-format on
        return false;
      }
      return append(data);
    };

    switch (tag) {
    case 0x5F2A: // Transaction Currency Code - n3
      CHECK_AND_RETURN(validateAndAppend({0x08, 0x40})); // USD
      break;
    case 0x95: // Terminal Verification Results - b
      CHECK_AND_RETURN(
          validateAndAppend({0x00, 0x00, 0x00, 0x00, 0x00})); // No errors
      break;
    case 0x9A: // Transaction Date - n6 YYMMDD
      CHECK_AND_RETURN(validateAndAppend({0x25, 0x11, 0x15})); // Nov 15th, 2025
      break;
    case 0x9C: // Transaction Type - n2
      // Sale
      CHECK_AND_RETURN(validateAndAppend({0x00}));
      break;
    case 0x9F02: // Amount, Authorised (Numeric) - n12
      CHECK_AND_RETURN(
          validateAndAppend({0x00, 0x00, 0x00, 0x00, 0x00,
                             0x00})); // $0.00, would be 0x01, 0x23 for $1.23
      break;
    case 0x9F03: // Amount, Other (Numeric) - n12
      CHECK_AND_RETURN(
          validateAndAppend({0x00, 0x00, 0x00, 0x00, 0x00, 0x00})); // $0
      break;
    case 0x9F37: // Unpredictable Number - b
      CHECK_AND_RETURN(validateAndAppend({0x05, 0x01, 0x02, 0x03}));
      break;
    case 0x9F4E: // Merchant Name and Location - ans
      CHECK_AND_RETURN(validateAndAppend({'a', 'b', 'c', 'd', 'e', 'f', 'g',
                                          'h', 'i', 'j', 'k', 'l', 'm', 'n',
                                          'o', 'p', 'q', 'r', 's', 0x00}));
      break;
    case 0x9F66: // Terminal Transaction Qualifiers (TTQ)
      // Base: 37, 80, 40, 00
      // Also works: 37, A0, 40, 00
      // Also works: 37, 20, 40, 00
      CHECK_AND_RETURN(validateAndAppend({0x37, 0x20, 0x40, 0x00}));
      break;
    case 0x9F1A: // Terminal Country Code - n3
      CHECK_AND_RETURN(validateAndAppend({0x08, 0x40})); // US
      break;
    case 0x9F15:
      CHECK_AND_RETURN(validateAndAppend({0x41, 0x31})); // Passenger Bus
      break;
    // CDOL testing
    case 0x9F35: // Terminal type (discover?? untested)
      CHECK_AND_RETURN(validateAndAppend({0x25}));
      break;
    case 0x9F34: // CDM results
      CHECK_AND_RETURN(validateAndAppend(
          {0x3F, 0x00, 0x00})); // 3F = no CDM performed, 00 = always, 00 =
                                // unknown result (maybe 02 = success)
      break;
    default:
      Serial.printf("WARNING: Unknown DOL tag %04X, filling with zeros\n", tag);
      CHECK_AND_RETURN(fill(0, requiredLen));
    }
  }
  return true;
}