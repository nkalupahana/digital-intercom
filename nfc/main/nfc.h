#pragma once

#include "HardwareSerial.h"
#include "Slice.h"
#include "errors.h"
#include "utils.h"
#include <PN532.h>
#include <PN532_SPI.h>
#include <SPI.h>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

constexpr size_t PN532_SS = 5;
constexpr size_t PN532_PACKBUFFSIZ = 255;
PN532_SPI pn532spi(SPI, PN532_SS);
PN532 nfc(pn532spi);

bool nfcSetup() {
  nfc.begin();

  uint32_t versiondata = nfc.getFirmwareVersion();
  while (!versiondata) {
    Serial.println("Waiting for PN532 to initialize...");
    versiondata = nfc.getFirmwareVersion();
    delay(1000);
  }

  Serial.print("Found chip PN5");
  Serial.println((versiondata >> 24) & 0xFF, HEX);
  Serial.print("Firmware ver. ");
  Serial.print((versiondata >> 16) & 0xFF, DEC);
  Serial.print('.');
  Serial.println((versiondata >> 8) & 0xFF, DEC);

  if (!nfc.SAMConfig()) {
    Serial.println("Failed to configure SAM!");
    return false;
  }

  if (!nfc.setPassiveActivationRetries(0x00)) {
    Serial.println("Failed to configure retries!");
    return false;
  }

  return true;
}

std::optional<ReadSlice> exchangeData(const char *pre,
                                      std::span<const uint8_t> toSend,
                                      std::span<uint8_t> recvBuf) {
  printHex(pre, toSend);

  uint8_t recvLen = recvBuf.size();
  bool success = nfc.inDataExchange(toSend.data(), toSend.size(),
                                    recvBuf.data(), &recvLen);
  CHECK_PRINT_RETURN_OPT("Failed to get response for InDataExchange", success);

  std::span<const uint8_t> recvSpan = recvBuf.subspan(0, recvLen);
  printHex("Received data: ", recvSpan);

  ReadSlice readSlice(recvSpan.data(), recvSpan.size());
  uint8_t sw2 = readSlice.readByteFromEnd();
  uint8_t sw1 = readSlice.readByteFromEnd();
  uint16_t sw = (sw1 << 8) | sw2;
  CHECK_PRINT_RETURN_OPT("Unexpected status word: %02x\n", sw == 0x9000, sw);

  return readSlice;
}

std::optional<ReadSlice> exchangeDataICT(const char *pre,
                                         std::span<const uint8_t> toSend,
                                         std::span<uint8_t> recvBuf) {
  printHex(pre, toSend);

  uint8_t recvLen = recvBuf.size();
  CHECK_PRINT_RETURN_OPT("Failed to inCommunicateThru",
                         nfc.inCommunicateThru(toSend.data(), toSend.size(),
                                               recvBuf.data(), &recvLen));

  ReadSlice readSlice(recvBuf.data(), recvLen);
  return readSlice;
}

bool exchangeDataICT(std::span<const uint8_t> toSend) {
  CHECK_PRINT_RETURN_BOOL("Failed to inCommunicateThru",
                          nfc.inCommunicateThru(toSend.data(), toSend.size()));

  return false;
}