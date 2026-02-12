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

std::optional<ReadSlice> performHandoff() {
  std::optional<ReadSlice> readSliceOpt;
  ReadSlice readSlice{nullptr, 0};

  // SELECT CC
  writeSlice.reset();
  CHECK_RETURN_OPT(
      writeSlice.appendApduCommand(0x00, 0xA4, 0x00, 0x0C, {{0xE1, 0x03}}));
  readSliceOpt =
      NFC::exchangeData("Sending SELECT CC File: ", writeSlice.span(), rbuf);
  CHECK_RETURN_OPT(readSliceOpt);
  readSlice = *readSliceOpt;

  printHex("CC file: ", readSlice.span());

  // Read CC length (first two bytes)
  writeSlice.reset();
  CHECK_RETURN_OPT(writeSlice.append({{0x00, 0xB0, 0x00, 0x00, 0x02}}));
  readSliceOpt =
      NFC::exchangeData("Reading CC length: ", writeSlice.span(), rbuf);
  CHECK_RETURN_OPT(readSliceOpt);
  readSlice = *readSliceOpt;
  printHex("CC length: ", readSlice.span());

  // Assert byte 0 is 0x00, and length is 2
  CHECK_PRINT_RETURN_OPT("CC length is not 2", readSlice.span().size() == 2);
  CHECK_PRINT_RETURN_OPT("CC length byte 0 is not 0x00",
                         readSlice.span()[0] == 0x00);
  uint8_t ccLength = readSlice.span()[1]; // No +2 is intentional

  // Read CC data
  writeSlice.reset();
  CHECK_RETURN_OPT(writeSlice.append({{0x00, 0xB0, 0x00, 0x00, ccLength}}));
  readSliceOpt =
      NFC::exchangeData("Reading CC data: ", writeSlice.span(), rbuf);
  CHECK_RETURN_OPT(readSliceOpt);
  readSlice = *readSliceOpt;
  printHex("CC data: ", readSlice.span());

  // TODO: parse file out of CC data. for now, hardcoding 0xE104
  // NDEF select file
  writeSlice.reset();
  CHECK_RETURN_OPT(
      writeSlice.appendApduCommand(0x00, 0xA4, 0x00, 0x0C, {{0xE1, 0x04}}));
  readSliceOpt =
      NFC::exchangeData("Sending NDEF Select File: ", writeSlice.span(), rbuf);
  CHECK_RETURN_OPT(readSliceOpt);
  readSlice = *readSliceOpt;

  // Read NDEF record length
  writeSlice.reset();
  CHECK_RETURN_OPT(writeSlice.append({{0x00, 0xB0, 0x00, 0x00, 0x02}}));
  readSliceOpt =
      NFC::exchangeData("Sending Read binary: ", writeSlice.span(), rbuf);
  CHECK_RETURN_OPT(readSliceOpt);
  readSlice = *readSliceOpt;
  printHex("Binary length: ", readSlice.span());

  // Assert byte 0 is 0x00, and length is 2
  CHECK_PRINT_RETURN_OPT("Binary length is not 2",
                         readSlice.span().size() == 2);
  CHECK_PRINT_RETURN_OPT("Binary length byte 0 is not 0x00",
                         readSlice.span()[0] == 0x00);
  uint8_t binaryLength = readSlice.span()[1] + 2;

  // Read NDEF record. Should contain an NDEF record of type Tp (service
  // parameter) with data including "urn:nfc:sn:handover". We may want to parse
  // and verify this more robustly.
  writeSlice.reset();
  CHECK_RETURN_OPT(writeSlice.append({{0x00, 0xB0, 0x00, 0x00, binaryLength}}));
  readSliceOpt =
      NFC::exchangeData("Sending Read binary: ", writeSlice.span(), rbuf);
  CHECK_RETURN_OPT(readSliceOpt);
  readSlice = *readSliceOpt;
  printHex("Binary: ", readSlice.span());

  auto serviceParameter =
      std::ranges::search(readSlice.span(), std::string_view("Tp"));
  CHECK_PRINT_RETURN_OPT("Service parameter not found",
                         serviceParameter.size() > 0);
  auto handoverData = std::ranges::search(
      readSlice.span(), std::string_view("urn:nfc:sn:handover"));
  CHECK_PRINT_RETURN_OPT("Handover data not found", handoverData.size() > 0);

  // TODO: technically, we could have static handover here.
  // We should test with Android to check in case it does static
  // handover. If not, just keeping our implementation of negotiated
  // handover works for me.

  // Select the handover service (Ts)

  return std::nullopt;
}

} // namespace DigitalID