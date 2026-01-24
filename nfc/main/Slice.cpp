#include "Slice.h"
#include "errors.h"
#include <HardwareSerial.h>
#include <cstdint>
#include <cstring>
#include <string_view>

ReadSlice::ReadSlice(const uint8_t *data, size_t len)
    : data_(data), len_(len) {}

size_t ReadSlice::len() const { return len_; }
const uint8_t *ReadSlice::data() const { return data_; }
std::span<const uint8_t> ReadSlice::span() const { return {data(), len()}; }

uint8_t ReadSlice::readByte() {
  // Returning 0 is sus, but this makes the API easier to use, and we'll
  // see a print showing that something is wrong
  CHECK_PRINT_RETURN_VAL("ERROR: Not enough data to ready byte", len_ >= 1, 0);

  uint8_t value = *data_;
  data_++;
  len_--;
  return value;
}

uint8_t ReadSlice::readByteFromEnd() {
  // Returning 0 is sus, but this makes the API easier to use, and we'll
  // see a print showing that something is wrong
  CHECK_PRINT_RETURN_VAL("ERROR: Not enough data to read byte from end",
                         len_ >= 1, 0);
  len_--;
  uint8_t value = data_[len_];
  return value;
}

bool ReadSlice::windowToPN532Response() {

  CHECK_PRINT_RETURN_BOOL("ERROR: Not enough data to window to PN532 response",
                          len_ >= 8);
  CHECK_PRINT_RETURN_BOOL("ERROR: Invalid PN532 preamble or start code",
                          readByte() == 0x00 && readByte() == 0x00 &&
                              readByte() == 0xFF);

  size_t len = readByte();
  uint8_t lenChecksum = readByte();
  CHECK_PRINT_RETURN_BOOL("ERROR: Invalid PN532 length",
                          (uint8_t)(len + lenChecksum) == 0x00);
  CHECK_PRINT_RETURN_BOOL("ERROR: Not enough data for PN532 response",
                          len_ >= len + 2);
  len_ = len + 2;
  CHECK_PRINT_RETURN_BOOL("ERROR: Invalid PN532 TFI", readByte() == 0xD5);
  CHECK_PRINT_RETURN_BOOL("ERROR: Invalid PN532 postamble",
                          readByteFromEnd() == 0x00);
  uint8_t _ = readByteFromEnd(); // checksum

  return true;
}

WriteSlice::WriteSlice(uint8_t *data, size_t len)
    : data_(data), len_(len), startingLen_(len) {}

size_t WriteSlice::len() const { return startingLen_ - len_; }
uint8_t *WriteSlice::data() const { return data_ - len(); }
std::span<const uint8_t> WriteSlice::span() const { return {data(), len()}; }

void WriteSlice::reset() {
  data_ = data_ - len();
  len_ = startingLen_;
}

void WriteSlice::appendUnsafe(const std::span<const uint8_t> data) {
  size_t len = data.size();
  memcpy(data_, data.data(), len);
  data_ += len;
  len_ -= len;
}

bool WriteSlice::fill(uint8_t value, size_t count) {
  CHECK_PRINT_RETURN_BOOL("ERROR: Not enough space to fill data",
                          len_ >= count);

  memset(data_, value, count);
  data_ += count;
  len_ -= count;
  return true;
}

uint8_t *WriteSlice::deferAppend(size_t len) {
  CHECK_PRINT_RETURN_VAL("ERROR: Not enough space to defer append", len_ >= len,
                         nullptr);

  uint8_t *ptr = data_;
  data_ += len;
  len_ -= len;
  return ptr;
}

bool WriteSlice::append(const std::span<const uint8_t> data) {
  CHECK_PRINT_RETURN_BOOL(
      "ERROR: Not enough space to append data - bufferLen: %zu, dataLen: %zu",
      len_ >= data.size(), len_, data.size());

  appendUnsafe(data);
  return true;
}

bool WriteSlice::appendApduCommand(uint8_t cla, uint8_t ins, uint8_t p1,
                                   uint8_t p2,
                                   const std::span<const uint8_t> data) {
  size_t len = data.size();
  // CLA + INS + P1 + P2 + Lc + Data + Le
  size_t numToWrite = 5 + len + 1;
  CHECK_PRINT_RETURN_BOOL(
      "ERROR: Not enough space to write APDU command - bufferLen: %zu, "
      "commandLen: %zu",
      len <= 0xFF && len_ >= numToWrite, len_, numToWrite);

  appendUnsafe({{cla, ins, p1, p2, (uint8_t)len}});
  appendUnsafe(data);
  appendUnsafe({{0x00}}); // Le

  return true;
}

bool WriteSlice::appendApduCommand(uint8_t cla, uint8_t ins, uint8_t p1,
                                   uint8_t p2, std::string_view cmd) {
  std::span<const uint8_t> byteCommand{
      reinterpret_cast<const uint8_t *>(cmd.data()), cmd.size()};
  return appendApduCommand(cla, ins, p1, p2, byteCommand);
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
      CHECK_PRINT_RETURN_BOOL("ERROR: DOL length mismatch - tag: %04X - "
                              "requiredLen: %u - dataLen: %zu",
                              data.size() == requiredLen, tag, requiredLen,
                              data.size());

      return append(data);
    };

    switch (tag) {
    case 0x5F2A: // Transaction Currency Code - n3
      CHECK_RETURN_BOOL(validateAndAppend({0x08, 0x40})); // USD
      break;
    case 0x95: // Terminal Verification Results - b
      CHECK_RETURN_BOOL(
          validateAndAppend({0x00, 0x00, 0x00, 0x00, 0x00})); // No errors
      break;
    case 0x9A: // Transaction Date - n6 YYMMDD
      CHECK_RETURN_BOOL(
          validateAndAppend({0x25, 0x11, 0x15})); // Nov 15th, 2025
      break;
    case 0x9C: // Transaction Type - n2
      // Sale
      CHECK_RETURN_BOOL(validateAndAppend({0x00}));
      break;
    case 0x9F02: // Amount, Authorised (Numeric) - n12
      CHECK_RETURN_BOOL(
          validateAndAppend({0x00, 0x00, 0x00, 0x00, 0x00,
                             0x00})); // $0.00, would be 0x01, 0x23 for $1.23
      break;
    case 0x9F03: // Amount, Other (Numeric) - n12
      CHECK_RETURN_BOOL(
          validateAndAppend({0x00, 0x00, 0x00, 0x00, 0x00, 0x00})); // $0
      break;
    case 0x9F21: // Transaction Time - n6
      CHECK_RETURN_BOOL(validateAndAppend({0x12, 0x00, 0x00})); // 12:00:00
      break;
    case 0x9F37: // Unpredictable Number - b
      CHECK_RETURN_BOOL(validateAndAppend({0x05, 0x01, 0x02, 0x03}));
      break;
    // case 0x9F4E: // Merchant Name and Location - ans

    //   CHECK_RETURN_BOOL(validateAndAppend({'a', 'b', 'c', 'd', 'e', 'f', 'g',
    //                                        'h', 'i', 'j', 'k', 'l', 'm', 'n',
    //                                        'o', 'p', 'q', 'r', 's', 0x00}));
    //   break;
    case 0x9F66: // Terminal Transaction Qualifiers (TTQ)
      // Base: 37, 80, 40, 00
      // Also works: 37, A0, 40, 00
      // Also works: 37, 20, 40, 00
      CHECK_RETURN_BOOL(validateAndAppend({0x37, 0x20, 0x40, 0x00}));
      break;
    case 0x9F6E: {// Third Party Data
      // AMEX: Enhances Contactless Reader Capabities
      CHECK_RETURN_BOOL(validateAndAppend({0xD8, 0xF0, 0xC0, 0x00}));
      break;
    }
    case 0x9F1A: // Terminal Country Code - n3
      CHECK_RETURN_BOOL(validateAndAppend({0x08, 0x40})); // US
      break;
    case 0x9F15:
      CHECK_RETURN_BOOL(validateAndAppend({0x41, 0x31})); // Passenger Bus
      break;
    // CDOL testing
    case 0x9F35: // Terminal type (discover?? untested)
      CHECK_RETURN_BOOL(validateAndAppend(
          {0x25})); // 25 – Unattended, Online with offline capability
      break;
    case 0x9F34: // CDM results
      CHECK_RETURN_BOOL(validateAndAppend(
          {0x3F, 0x00, 0x00})); // 3F = no CDM performed, 00 = always, 00 =
                                // unknown result (maybe 02 = success)
      break;
    default:
      Serial.printf("WARNING: Unknown DOL tag %04X, filling with zeros\n", tag);
      CHECK_RETURN_BOOL(fill(0, requiredLen));
    }
  }
  return true;
}