#include "Slice.h"
#include "errors.h"
#include "utils.h"
#include <HardwareSerial.h>
#include <PN532.h>
#include <PN532_SPI.h>
#include <SPI.h>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace NFC {
constexpr size_t PN532_SS = 5;
PN532_SPI pn532spi(SPI, PN532_SS);
PN532 nfc(pn532spi);

bool setup() {
  nfc.begin();

  uint32_t versiondata = nfc.getFirmwareVersion();
  while (!versiondata) {
    ESP_LOGI(TAG, "Waiting for PN532 to initialize...");
    versiondata = nfc.getFirmwareVersion();
    delay(1000);
  }

  ESP_LOGI(TAG, "Found chip PN5%02X", (versiondata >> 24) & 0xFF);
  ESP_LOGI(TAG, "Firmware ver. %d.%d", (versiondata >> 16) & 0xFF,
           (versiondata >> 8) & 0xFF);

  if (!nfc.SAMConfig()) {
    ESP_LOGE(TAG, "Failed to configure SAM!");
    return false;
  }

  if (!nfc.setPassiveActivationRetries(0x00)) {
    ESP_LOGI(TAG, "Failed to configure retries!");
    return false;
  }

  return true;
}

bool inListPassiveTarget() { return nfc.inListPassiveTarget(); }

bool writeRegister(uint16_t addr, uint8_t value) {
  return nfc.writeRegister(addr, value);
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
} // namespace NFC