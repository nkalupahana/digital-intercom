#include "NFC.h"
#include "Slice.h"
#include "errors.h"
#include "utils.h"
#include <cstdint>
#include <optional>
#include <span>
#include <tlv.h>

namespace DigitalID {
uint8_t rbuf[PN532_PACKBUFFSIZ];
uint8_t sbuf[PN532_PACKBUFFSIZ];
WriteSlice writeSlice(sbuf, PN532_PACKBUFFSIZ);

bool checkIfValid() {
  writeSlice.reset();
  // SELECT NDEF
  CHECK_RETURN_BOOL(writeSlice.appendApduCommand(
      0x00, 0xA4, 0x04, 0x00, {{0xD2, 0x76, 0x00, 0x00, 0x85, 0x01, 0x01}}));
  auto data =
      NFC::exchangeData("Sending SELECT NDEF: ", writeSlice.span(), rbuf);

  return data.has_value();
}

} // namespace DigitalID