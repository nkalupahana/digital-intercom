// #define MIFAREDEBUG
// #define PN532DEBUG
#include "SPI.h"
#include "Slice.h"
#include "errors.h"

#include <PN532_SPI.h>
#include <PN532.h>
#include <tlv.h>

#include <cstdint>
#include <optional>
#include <span>
#include <NdefRecord.h>
#include <NdefMessage.h>

constexpr size_t PN532_SS = 5;

constexpr size_t PN532_PACKBUFFSIZ = 255;
constexpr size_t RECORD_BUFSIZ = 300;
constexpr size_t CDOL_BUFSIZ = 64;
constexpr size_t AFL_BUFSIZE = 16;
constexpr size_t TRACK2_TAG = 0x57;
constexpr size_t TRACK2_TAG_BUFSIZ = 19;

PN532_SPI pn532spi(SPI, PN532_SS);
PN532 nfc(pn532spi);

void setup() {
  Serial.begin(115200);

  Serial.println("HELLO!");
  nfc.begin();

  uint32_t versiondata = 0;
  while (!versiondata) {
    Serial.println("Waiting for PN532 to initialize...");
    versiondata = nfc.getFirmwareVersion();
    delay(1000);
  }

  // Got ok data, print it out!
  Serial.print("Found chip PN5");
  Serial.println((versiondata >> 24) & 0xFF, HEX);
  Serial.print("Firmware ver. ");
  Serial.print((versiondata >> 16) & 0xFF, DEC);
  Serial.print('.');
  Serial.println((versiondata >> 8) & 0xFF, DEC);

  if (!nfc.SAMConfig()) {
    Serial.println("Failed to configure SAM!");
  }

  if (!nfc.setPassiveActivationRetries(0x00)) {
    Serial.println("Failed to configure retries!");
  }
}

void printHex(const char *pre, std::span<const uint8_t> data) {
  Serial.print(pre);
  for (const uint8_t byte : data) {
    Serial.printf("%02X", byte);
  }
  Serial.println();
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
  CHECK_PRINT_RETURN_OPT("Failed to inCommunicateThru", nfc.inCommunicateThru(toSend.data(), toSend.size(), recvBuf.data(), &recvLen));

  ReadSlice readSlice(recvBuf.data(), recvLen);
  return readSlice;
}

bool exchangeDataICT(std::span<const uint8_t> toSend) {
  CHECK_PRINT_RETURN_BOOL("Failed to inCommunicateThru", nfc.inCommunicateThru(toSend.data(), toSend.size()));

  return false;
}

std::optional<std::span<const uint8_t>> getTrack2Data() {
  bool foundCard = nfc.inListPassiveTarget();
  
  if (foundCard) {
    Serial.println("\nFound something!");
    std::optional<ReadSlice> readSliceOpt;
    ReadSlice readSlice{nullptr, 0};
    static TLVS tlvs;
    static uint8_t rbuf[PN532_PACKBUFFSIZ];
    static uint8_t sbuf[PN532_PACKBUFFSIZ];
    WriteSlice writeSlice(sbuf, PN532_PACKBUFFSIZ);

    // SELECT NDEF application
    writeSlice.reset();
    CHECK_RETURN_OPT(
      writeSlice.appendApduCommand(0x00, 0xA4, 0x04, 0x00, {{0xD2, 0x76, 0x00, 0x00, 0x85, 0x01, 0x01}}));
    readSliceOpt =
        exchangeData("Sending SELECT NDEF application: ", writeSlice.span(), rbuf);
    CHECK_RETURN_OPT(readSliceOpt);
    readSlice = *readSliceOpt;
    // SELECT CC
    writeSlice.reset();
    CHECK_RETURN_OPT(writeSlice.appendApduCommand(0x00, 0xA4, 0x00, 0x0C, {{0xE1, 0x03}}));
    readSliceOpt =
        exchangeData("Sending SELECT CC File: ", writeSlice.span(), rbuf);
    CHECK_RETURN_OPT(readSliceOpt);
    readSlice = *readSliceOpt;

    // READ CC data (read 15 bytes of data)
    writeSlice.reset();
    CHECK_RETURN_OPT(writeSlice.append({{0x00, 0xB0, 0x00, 0x00, 0x0F}}));
    readSliceOpt =
        exchangeData("Reading CC data: ", writeSlice.span(), rbuf);
    CHECK_RETURN_OPT(readSliceOpt);
    readSlice = *readSliceOpt;

    // TODO: Parse CC data to get the file ID (nearly always 0xE104)

    // NDEF Select file
    writeSlice.reset();
    CHECK_RETURN_OPT(writeSlice.appendApduCommand(0x00, 0xA4, 0x00, 0x0C, {{0xE1, 0x04}}));
    readSliceOpt =
        exchangeData("Sending NDEF Select File: ", writeSlice.span(), rbuf);
    CHECK_RETURN_OPT(readSliceOpt);
    readSlice = *readSliceOpt;

    // Read binary
    writeSlice.reset();
    CHECK_RETURN_OPT(writeSlice.append({{0x00, 0xB0, 0x00, 0x00, 0x2F}}));
    readSliceOpt =
        exchangeData("Sending Read binary: ", writeSlice.span(), rbuf);
    CHECK_RETURN_OPT(readSliceOpt);
    readSlice = *readSliceOpt;
    
    // TODO: Handle static handover
    /// Negotiated handover
    // Select Service
    NdefRecord serviceSelect = NdefRecord();
    serviceSelect.setTnf(TNF_WELL_KNOWN);
    serviceSelect.setId(0, 0);
    serviceSelect.setType((const byte*)"Ts", 2);
    serviceSelect.setPayload((const byte*)"\x13urn:nfc:sn:handover", 20);

    byte messageBuf[300];
    serviceSelect.encode(messageBuf, true, true);
    auto messageSpan = std::span<const uint8_t>(messageBuf, serviceSelect.getEncodedSize());
    printHex("Service Select: ", messageSpan);

    // Write to file: length + message
    writeSlice.reset();
    CHECK_RETURN_OPT(writeSlice.append({{0x00, 0xD6, 0x00, 0x00, static_cast<uint8_t>(messageSpan.size() + 2), 0, static_cast<uint8_t>(messageSpan.size())}}));
    writeSlice.append(messageSpan);
    readSliceOpt = exchangeData("Writing Service Select message: ", writeSlice.span(), rbuf);
    CHECK_RETURN_OPT(readSliceOpt);
    readSlice = *readSliceOpt;

    // Read back
    writeSlice.reset();
    CHECK_RETURN_OPT(writeSlice.append({{0x00, 0xB0, 0x00, 0x00, 0x2F}}));
    readSliceOpt =
        exchangeData("Sending Read binary: ", writeSlice.span(), rbuf);
    CHECK_RETURN_OPT(readSliceOpt);
    readSlice = *readSliceOpt;

    // TODO: check for 0x5465 (Te) + 0x00 (status)
    
    // Send handover request for BLE
    // Generated with Kotlin, TODO: understand what this is
    auto handoverRequestSpan = std::span<const uint8_t>({0x91, 0x02, 0x0A, 0x48, 0x72, 0x15, 0xD1, 0x02, 0x04, 0x61, 0x63, 0x01, 0x01, 0x30, 0x00, 0x1C, 0x1E, 0x06, 0x0A, 0x69, 0x73, 0x6F, 0x2E, 0x6F, 0x72, 0x67, 0x3A, 0x31, 0x38, 0x30, 0x31, 0x33, 0x3A, 0x72, 0x65, 0x61, 0x64, 0x65, 0x72, 0x65, 0x6E, 0x67, 0x61, 0x67, 0x65, 0x6D, 0x65, 0x6E, 0x74, 0x6D, 0x64, 0x6F, 0x63, 0x72, 0x65, 0x61, 0x64, 0x65, 0x72, 0xA1, 0x00, 0x63, 0x31, 0x2E, 0x30, 0x5A, 0x20, 0x15, 0x01, 0x61, 0x70, 0x70, 0x6C, 0x69, 0x63, 0x61, 0x74, 0x69, 0x6F, 0x6E, 0x2F, 0x76, 0x6E, 0x64, 0x2E, 0x62, 0x6C, 0x75, 0x65, 0x74, 0x6F, 0x6F, 0x74, 0x68, 0x2E, 0x6C, 0x65, 0x2E, 0x6F, 0x6F, 0x62, 0x30, 0x02, 0x1C, 0x00, 0x11, 0x07, 0xA4, 0xB2, 0x31, 0xD2, 0x94, 0x69, 0x0B, 0xA9, 0xF9, 0x4F, 0x3C, 0x09, 0x40, 0x60, 0x18, 0x82});
    // NFC
    // auto handoverRequestSpan = std::span<const uint8_t>({0x91, 0x02, 0x0A, 0x48, 0x72, 0x15, 0xD1, 0x02, 0x04, 0x61, 0x63, 0x01, 0x01, 0x30, 0x00, 0x1C, 0x1E, 0x06, 0x0A, 0x69, 0x73, 0x6F, 0x2E, 0x6F, 0x72, 0x67, 0x3A, 0x31, 0x38, 0x30, 0x31, 0x33, 0x3A, 0x72, 0x65, 0x61, 0x64, 0x65, 0x72, 0x65, 0x6E, 0x67, 0x61, 0x67, 0x65, 0x6D, 0x65, 0x6E, 0x74, 0x6D, 0x64, 0x6F, 0x63, 0x72, 0x65, 0x61, 0x64, 0x65, 0x72, 0xA1, 0x00, 0x63, 0x31, 0x2E, 0x30, 0x5A, 0x20, 0x15, 0x01, 0x61, 0x70, 0x70, 0x6C, 0x69, 0x63, 0x61, 0x74, 0x69, 0x6F, 0x6E, 0x2F, 0x76, 0x6E, 0x64, 0x2E, 0x62, 0x6C, 0x75, 0x65, 0x74, 0x6F, 0x6F, 0x74, 0x68, 0x2E, 0x6C, 0x65, 0x2E, 0x6F, 0x6F, 0x62, 0x30, 0x02, 0x1C, 0x00, 0x11, 0x07, 0x12, 0x25, 0x05, 0x33, 0x8E, 0x98, 0xBA, 0xA1, 0x76, 0x48, 0x10, 0xD3, 0xD2, 0xB1, 0x2D, 0x11});

    // Write to file: length + message
    writeSlice.reset();
    CHECK_RETURN_OPT(writeSlice.append({{0x00, 0xD6, 0x00, 0x00, static_cast<uint8_t>(handoverRequestSpan.size() + 2), 0, static_cast<uint8_t>(handoverRequestSpan.size())}}));
    writeSlice.append(handoverRequestSpan);
    readSliceOpt = exchangeData("Writing Handover Request: ", writeSlice.span(), rbuf);
    CHECK_RETURN_OPT(readSliceOpt);
    readSlice = *readSliceOpt;

    // TODO: retry until data is populated instead of fixed delay
    delay(300);

    // Read back
    writeSlice.reset();
    // TODOL this length should be dynamically read instead of hardcoded
    CHECK_RETURN_OPT(writeSlice.append({{0x00, 0xB0, 0x00, 0x00, 0xC4}}));
    readSliceOpt =
        exchangeData("Sending Read binary: ", writeSlice.span(), rbuf);
    CHECK_RETURN_OPT(readSliceOpt);
    readSlice = *readSliceOpt;
    
    delay(2000);
    return std::nullopt;

    // SELECT AID
    tlvs.decodeTLVs(rbuf, readSlice.len());
    TLVNode *aidNode = tlvs.findTLV(0x4F);
    CHECK_PRINT_RETURN_OPT("Failed to get AID from card!", aidNode);
    std::span<const uint8_t> aid{aidNode->getValue(),
                                 aidNode->getValueLength()};
    tlvs.reset();

    writeSlice.reset();
    CHECK_RETURN_OPT(writeSlice.appendApduCommand(0x00, 0xA4, 0x04, 0x00, aid));
    readSliceOpt =
        exchangeData("Sending SELECT AID: ", writeSlice.span(), rbuf);
    CHECK_RETURN_OPT(readSliceOpt);
    readSlice = *readSliceOpt;

    // GPO
    tlvs.decodeTLVs(readSlice.data(), readSlice.len());
    TLVNode *pdolNode = tlvs.findTLV(0x9F38);

    writeSlice.reset();
    if (pdolNode == nullptr) {
      tlvs.reset();
      Serial.println("Failed to find PDOL. Using empty DOL");
      CHECK_RETURN_OPT(
          writeSlice.appendApduCommand(0x80, 0xA8, 0x00, 0x00, {{0x83, 0x00}}));
    } else {
      ReadSlice pdol{pdolNode->getValue(), pdolNode->getValueLength()};
      tlvs.reset();
      CHECK_RETURN_OPT(writeSlice.appendApduCommand(
          0x80, 0xA8, 0x00, 0x00, [&pdol](WriteSlice &slice) {
            return slice.appendTLV(0x83, [&pdol](WriteSlice &slice) {
              return slice.appendFromDol(pdol);
            });
          }));
    }
    readSliceOpt = exchangeData("Sending GPO: ", writeSlice.span(), rbuf);
    CHECK_RETURN_OPT(readSliceOpt);
    readSlice = *readSliceOpt;

    // Try to get Track 2 data it it's already available
    static uint8_t track2Buf[TRACK2_TAG_BUFSIZ];
    WriteSlice track2Slice{track2Buf, sizeof(track2Buf)};
    tlvs.decodeTLVs(readSlice.data(), readSlice.len());
    TLVNode *track2Node = tlvs.findTLV(TRACK2_TAG);
    if (track2Node) {
      track2Slice.append(
          {track2Node->getValue(), track2Node->getValueLength()});
    }

    // Read records
    TLVNode *aflNode = tlvs.findTLV(0x94);
    CHECK_PRINT_RETURN_OPT("No AFL data found", aflNode);
    static uint8_t aflBuf[AFL_BUFSIZE];
    size_t aflLen = aflNode->getValueLength();
    CHECK_PRINT_RETURN_OPT(
        "aflBuf is not large enough - bufLen: %zu - requiredlen: %zu",
        aflLen <= AFL_BUFSIZE, AFL_BUFSIZE, aflLen);
    memcpy(aflBuf, aflNode->getValue(), aflLen);
    ReadSlice aflSlice{aflBuf, aflLen};
    tlvs.reset();

    // Figure out the last sent block number
    readSliceOpt = exchangeDataICT("R(NACK): ", {{0xB2}}, rbuf);
    CHECK_RETURN_OPT(readSliceOpt);
    readSlice = *readSliceOpt;
    uint8_t blockNum = ((readSlice.readByte() & 0xA0) != 0) ? 0x1 : 0x0;
    Serial.printf("blockNum: %02x\n", blockNum);

    static uint8_t cdolBuf[CDOL_BUFSIZ];
    static WriteSlice cdolSlice(cdolBuf, sizeof(cdolBuf));
    cdolSlice.reset();

    while (aflSlice.len() > 0) {
      uint8_t sfi = aflSlice.readByte();
      uint8_t recordToRead = aflSlice.readByte();
      uint8_t endRecord = aflSlice.readByte();
      // number of records included in data authentication
      uint8_t _ = aflSlice.readByte();
      for (; recordToRead <= endRecord; ++recordToRead) {
        Serial.printf("Reading record %02x\n", recordToRead);
        static uint8_t recordbuf[RECORD_BUFSIZ];
        WriteSlice recordSlice(recordbuf, RECORD_BUFSIZ);

        writeSlice.reset();
        blockNum ^= 1;
        uint8_t pcb = 0x2 | blockNum;
        uint8_t p2 = (sfi & 0xF8) | 0x4;
        readSliceOpt = exchangeDataICT(
            "Read Record: ", {{pcb, 0x00, 0xB2, recordToRead, p2, 0x00}}, rbuf);
        while (true) {
          CHECK_RETURN_OPT(readSliceOpt);
          readSlice = *readSliceOpt;

          uint8_t responsePcb = readSlice.readByte();
          CHECK_PRINT_RETURN_OPT("Failed to buffer record data",
                                 recordSlice.append(readSlice.span()));

          // If no more data, break
          if ((responsePcb & 0x10) == 0) {
            break;
          }
          // Send R(ACK) to get more data
          writeSlice.reset();
          blockNum ^= 1;
          pcb = 0xA2 | blockNum;
          readSliceOpt = exchangeDataICT("R(ACK): ", {{pcb}}, rbuf);
        }
        printHex("Full Read Record response: ", recordSlice.span());
        tlvs.decodeTLVs(recordbuf, recordSlice.len());

        if (TLVNode *cdolNode = tlvs.findTLV(0x8C)) {
          CHECK_PRINT_RETURN_OPT("Found duplicate CDOL tags",
                                 cdolSlice.len() == 0);
          cdolSlice.append({cdolNode->getValue(), cdolNode->getValueLength()});
        }
        if (TLVNode *track2Node = tlvs.findTLV(TRACK2_TAG)) {
          CHECK_PRINT_RETURN_OPT("Found duplicate Track 2 tags",
                                 track2Slice.len() == 0);
          track2Slice.append(
              {track2Node->getValue(), track2Node->getValueLength()});
        }
        tlvs.reset();
      }
    }

    if (cdolSlice.len() == 0) {
      // If there's no CDOL, then there's nothing else we can do, and we have to
      // just return whatever track 2 data we've found
      CHECK_PRINT_RETURN_OPT("No CDOL or Track 2 Equivalent Data found",
                             track2Slice.len());
      return track2Slice.span();
    }
    CHECK_PRINT_RETURN_VAL("No CDOL found", cdolSlice.len(),
                           track2Slice.span());
    printHex("CDOL data: ", cdolSlice.span());
    writeSlice.reset();
    bool builtAC = writeSlice.appendApduCommand(
        0x80, 0xAE, 0x50, 0x00, [](WriteSlice &slice) {
          ReadSlice cdolReadSlice{cdolSlice.data(), cdolSlice.len()};
          return slice.appendFromDol(cdolReadSlice);
        });
    CHECK_PRINT_RETURN_OPT("Failed to build GENERATE AC command", builtAC);

    readSliceOpt =
        exchangeData("Sending GENERATE AC: ", writeSlice.span(), rbuf);
    CHECK_PRINT_RETURN_OPT("Failed to send ", readSliceOpt);

    CHECK_PRINT_RETURN_OPT(
        "No Track 2 Equivalent Data found after reading records",
        track2Slice.len());
    return track2Slice.span();
  } else {
    // // Set CIU_BitFraming register to send 8 bits
    // uint16_t addr = 0x633D;
    // CHECK_PRINT_RETURN_OPT(
    //     "Failed to set CIU_BitFraming for ECP",
    //     nfc.writeRegister(addr, 0x00));

    // // Send the ECP frame
    // exchangeDataICT({{0x6a, 0x02, 0xc8, 0x01, 0x00, 0x03, 0x00, 0x06, 0x7f, 0x01, 0x00, 0x00, 0x00, 0x4d, 0xef}});
  }

  return std::nullopt;
}

void loop() {
  const std::optional<std::span<const uint8_t>> track2DataOpt = getTrack2Data();
  CHECK_RETURN(track2DataOpt);
  const std::span<const uint8_t> track2Data = *track2DataOpt;
  printHex("Track 2 Equivalent Data: ", track2Data);
  delay(3000);
}