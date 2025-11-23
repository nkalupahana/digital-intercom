#pragma once

#include "errors.h"

#include <HardwareSerial.h>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

class ReadSlice {
public:
  ReadSlice(const uint8_t *data, size_t len);

  size_t len() const;
  const uint8_t *data() const;
  std::span<const uint8_t> span() const;

  uint8_t readByte();
  uint8_t readByteFromEnd();

  bool windowToPN532Response();

private:
  const uint8_t *data_;
  size_t len_;
};

class WriteSlice {
public:
  WriteSlice(uint8_t *data, size_t len);

  size_t len() const;
  uint8_t *data() const;
  std::span<const uint8_t> span() const;

  void reset();

  void appendUnsafe(const std::span<const uint8_t> data);

  bool fill(uint8_t value, size_t count);

  uint8_t *deferAppend(size_t len);

  bool append(const std::span<const uint8_t> data);

  // le (length expected) is always set to 0
  bool appendApduCommand(uint8_t cla, uint8_t ins, uint8_t p1, uint8_t p2,
                         const std::span<const uint8_t> data);

  // le (length expected) is always set to 0
  bool appendApduCommand(uint8_t cla, uint8_t ins, uint8_t p1, uint8_t p2,
                         std::string_view cmd);

  bool appendFromDol(ReadSlice &dol);

  // le (length expected) is always set to 0

  // TODO: Avoid using std::function
  bool
  appendApduCommand(uint8_t cla, uint8_t ins, uint8_t p1, uint8_t p2,
                    const std::function<bool(WriteSlice &)> &appendCommandF) {
    // CLA + INS + P1 + P2 + Lc + Le
    size_t minSize = 6;
    if (len_ < minSize) {
      Serial.printf("ERROR: Not enough space to write APDU command\n");
      return false;
    }

    appendUnsafe({{cla, ins, p1, p2}});
    // Reserve space for Lc
    uint8_t *lcPtr = deferAppend(1);
    CHECK_RETURN_BOOL(lcPtr);

    uint8_t *initialData = data_;
    CHECK_RETURN_BOOL(appendCommandF(*this));
    if (data_ - initialData > 0xFF) {
      Serial.printf("ERROR: APDU command length exceeds 255 bytes\n");
      return false;
    }
    *lcPtr = data_ - initialData;
    append({{0x00}}); // Le

    return true;
  }

  template <typename T> bool appendTLV(uint8_t tag, T &&appendValueF) {
    CHECK_RETURN_BOOL(append({{tag}}));

    // Reserve space for length
    uint8_t *lengthPtr = deferAppend(1);
    CHECK_RETURN_BOOL(lengthPtr);

    uint8_t *initialData = data_;
    CHECK_RETURN_BOOL(appendValueF(*this));
    if (data_ - initialData > 0xFF) {
      printf("ERROR: TLV length exceeds 255 bytes\n");
      return false;
    }
    *lengthPtr = data_ - initialData;

    return true;
  }

private:
  uint8_t *data_;
  size_t len_;
  size_t startingLen_;
};