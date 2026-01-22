// #define MIFAREDEBUG
// #define PN532DEBUG
#include "SPI.h"
#include "Slice.h"
#include "errors.h"

#include <PN532.h>
#include <PN532_SPI.h>
#include <tlv.h>

#include <cstdint>
#include <optional>
#include <span>

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

bool tryCheckmark(TLVS &tlvs, WriteSlice &writeSlice, std::span<uint8_t> rbuf,
                  WriteSlice &track2Slice) {
  std::optional<ReadSlice> readSliceOpt;
  ReadSlice readSlice{nullptr, 0};
  // Read records
  TLVNode *aflNode = tlvs.findTLV(0x94);
  CHECK_PRINT_RETURN_BOOL("No AFL data found", aflNode);
  static uint8_t aflBuf[AFL_BUFSIZE];
  size_t aflLen = aflNode->getValueLength();
  CHECK_PRINT_RETURN_BOOL(
      "aflBuf is not large enough - bufLen: %zu - requiredlen: %zu",
      aflLen <= AFL_BUFSIZE, AFL_BUFSIZE, aflLen);
  memcpy(aflBuf, aflNode->getValue(), aflLen);
  ReadSlice aflSlice{aflBuf, aflLen};
  tlvs.reset();

  // Figure out the last sent block number
  readSliceOpt = exchangeDataICT("R(NACK): ", {{0xB2}}, rbuf);
  CHECK_RETURN_BOOL(readSliceOpt);
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
        CHECK_RETURN_BOOL(readSliceOpt);
        readSlice = *readSliceOpt;

        uint8_t responsePcb = readSlice.readByte();
        CHECK_PRINT_RETURN_BOOL("Failed to buffer record data",
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
        CHECK_PRINT_RETURN_BOOL("Found duplicate CDOL tags",
                                cdolSlice.len() == 0);
        cdolSlice.append({cdolNode->getValue(), cdolNode->getValueLength()});
      }
      if (TLVNode *track2Node = tlvs.findTLV(TRACK2_TAG)) {
        CHECK_PRINT_RETURN_BOOL("Found duplicate Track 2 tags",
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
    CHECK_PRINT_RETURN_BOOL("No CDOL or Track 2 Equivalent Data found",
                            track2Slice.len());
    return true;
  }
  printHex("CDOL data: ", cdolSlice.span());
  writeSlice.reset();
  bool builtAC = writeSlice.appendApduCommand(
      0x80, 0xAE, 0x50, 0x00, [](WriteSlice &slice) {
        ReadSlice cdolReadSlice{cdolSlice.data(), cdolSlice.len()};
        return slice.appendFromDol(cdolReadSlice);
      });
  CHECK_PRINT_RETURN_BOOL("Failed to build GENERATE AC command", builtAC);

  readSliceOpt = exchangeData("Sending GENERATE AC: ", writeSlice.span(), rbuf);
  CHECK_PRINT_RETURN_BOOL("Failed to do GENERATE AC", readSliceOpt);

  return true;
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

    // SELECT PPSE
    writeSlice.reset();
    CHECK_RETURN_OPT(
        writeSlice.appendApduCommand(0x00, 0xA4, 0x04, 0x00, "2PAY.SYS.DDF01"));
    readSliceOpt =
        exchangeData("Sending SELECT PPSE: ", writeSlice.span(), rbuf);
    CHECK_RETURN_OPT(readSliceOpt);
    readSlice = *readSliceOpt;

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

    // FIX
    if (!tryCheckmark(tlvs, writeSlice, rbuf, track2Slice)) {
      Serial.println("Failed to get checkmark, but might still have Track 2 "
                     "Equivalent Data");
    }

    CHECK_PRINT_RETURN_OPT(
        "No Track 2 Equivalent Data found after reading records",
        track2Slice.len());
    return track2Slice.span();
  } else {
    // Set CIU_BitFraming register to send 8 bits
    uint16_t addr = 0x633D;
    CHECK_PRINT_RETURN_OPT("Failed to set CIU_BitFraming for ECP",
                           nfc.writeRegister(addr, 0x00));

    // Send the ECP frame
    exchangeDataICT({{0x6a, 0x02, 0xc8, 0x01, 0x00, 0x03, 0x00, 0x06, 0x7f,
                      0x01, 0x00, 0x00, 0x00, 0x4d, 0xef}});
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