#include "NFC.h"
#include "Slice.h"
#include "errors.h"
#include "utils.h"
#include <NdefRecord.h>
#include <cstdint>
#include <optional>
#include <span>
#include <tlv.h>

namespace DigitalID {
uint8_t rbuf[PN532_PACKBUFFSIZ];
uint8_t sbuf[PN532_PACKBUFFSIZ];
WriteSlice writeSlice(sbuf, PN532_PACKBUFFSIZ);

std::optional<std::span<const uint8_t>> readNdefFile(bool isCC) {
  std::optional<ReadSlice> readSliceOpt;
  ReadSlice readSlice{nullptr, 0};

  // Read return data length
  writeSlice.reset();
  CHECK_RETURN_OPT(writeSlice.append({{0x00, 0xB0, 0x00, 0x00, 0x02}}));
  readSliceOpt = NFC::exchangeData("Sending read: ", writeSlice.span(), rbuf);
  CHECK_RETURN_OPT(readSliceOpt);
  readSlice = *readSliceOpt;
  printHex("Data length: ", readSlice.span());

  // Assert byte 0 is 0x00, and length is 2.
  // It is possible that byte 0 is not 0x00.
  // In that case, we would need to read repeatedly with offset.
  // But I haven't seen this in practice, so we'll just assert it.
  CHECK_PRINT_RETURN_OPT("Binary length is not 2",
                         readSlice.span().size() == 2);
  CHECK_PRINT_RETURN_OPT("Binary length byte 0 is not 0x00",
                         readSlice.span()[0] == 0x00);
  uint8_t binaryLength = readSlice.span()[1] + (isCC ? 0 : 2);

  // Read data
  writeSlice.reset();
  CHECK_RETURN_OPT(writeSlice.append({{0x00, 0xB0, 0x00, 0x00, binaryLength}}));
  readSliceOpt = NFC::exchangeData("Sending read: ", writeSlice.span(), rbuf);
  CHECK_RETURN_OPT(readSliceOpt);
  readSlice = *readSliceOpt;

  return readSlice.span();
}

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

  // All CC/NDEF ADPU commands are from the Type 4 Tag Operation Specification

  // SELECT CC
  writeSlice.reset();
  CHECK_RETURN_OPT(
      writeSlice.appendApduCommand(0x00, 0xA4, 0x00, 0x0C, {{0xE1, 0x03}}));
  readSliceOpt =
      NFC::exchangeData("Sending SELECT CC File: ", writeSlice.span(), rbuf);
  CHECK_RETURN_OPT(readSliceOpt);
  readSlice = *readSliceOpt;

  printHex("Select CC file: ", readSlice.span());

  // Read CC
  auto ccData = readNdefFile(true);
  CHECK_RETURN_OPT(ccData);

  // TODO: parse file out of CC data, there is TLV encoding in here. for now,
  // hardcoding 0xE104 NDEF select file
  // https://discord.com/channels/@me/783194785975369738/1445435281934123008
  // SELECT NDEF File
  writeSlice.reset();
  CHECK_RETURN_OPT(
      writeSlice.appendApduCommand(0x00, 0xA4, 0x00, 0x0C, {{0xE1, 0x04}}));
  readSliceOpt =
      NFC::exchangeData("Sending NDEF Select File: ", writeSlice.span(), rbuf);
  CHECK_RETURN_OPT(readSliceOpt);
  readSlice = *readSliceOpt;

  // Read NDEF record. Should contain an NDEF record of type Tp (service
  // parameter) with data including "urn:nfc:sn:handover". We may want to
  // parse and verify this more robustly.
  auto ndefData = readNdefFile(false);
  CHECK_RETURN_OPT(ndefData);

  auto serviceParameter =
      std::ranges::search(*ndefData, std::string_view("Tp"));
  CHECK_PRINT_RETURN_OPT("Service parameter not found",
                         serviceParameter.size() > 0);
  auto handoverData =
      std::ranges::search(*ndefData, std::string_view("urn:nfc:sn:handover"));
  CHECK_PRINT_RETURN_OPT("Handover data not found", handoverData.size() > 0);

  // Technically, we could have static handover here.
  // However, both iOS and Android do negotiated handover
  // for digital IDs, so we have no need to support static handover.

  // Select the handover service (Ts)
  NdefRecord serviceSelect = NdefRecord();
  serviceSelect.setTnf(TNF_WELL_KNOWN);
  serviceSelect.setId(0, 0);
  serviceSelect.setType((const byte *)"Ts", 2);
  // I think \x13 is the length of the payload
  // (TNEP stuff we don't have the spec for)
  serviceSelect.setPayload((const byte *)"\x13urn:nfc:sn:handover", 20);

  // TODO: we probably want writeSlice to be able to take in an NDEFRecord
  // instead of doing all this garbage
  // Write to file: length + message
  byte messageBuf[300];
  serviceSelect.encode(messageBuf, true, true);
  auto messageSpan =
      std::span<const uint8_t>(messageBuf, serviceSelect.getEncodedSize());
  printHex("Service Select: ", messageSpan);

  writeSlice.reset();
  CHECK_RETURN_OPT(writeSlice.append(
      {{0x00, 0xD6, 0x00, 0x00, static_cast<uint8_t>(messageSpan.size() + 2), 0,
        static_cast<uint8_t>(messageSpan.size())}}));
  writeSlice.append(messageSpan);
  readSliceOpt = NFC::exchangeData(
      "Writing Service Select message: ", writeSlice.span(), rbuf);
  CHECK_RETURN_OPT(readSliceOpt);
  readSlice = *readSliceOpt;
  printHex("Service Select response: ", readSlice.span());

  // Read response
  auto serviceSelectedResponse = readNdefFile(false);
  CHECK_RETURN_OPT(serviceSelectedResponse);

  // Send handover request

  return std::nullopt;
}

} // namespace DigitalID