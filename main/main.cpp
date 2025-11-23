// #define MIFAREDEBUG
// #define PN532DEBUG
#include "Slice.h"
#include "errors.h"

#include <Adafruit_PN532.h>
#include <tlv.h>

#include <cstdint>
#include <optional>
#include <span>

constexpr size_t PN532_SCK = 18;
constexpr size_t PN532_MOSI = 23;
constexpr size_t PN532_SS = 5;
constexpr size_t PN532_MISO = 19;

constexpr size_t PN532_PACKBUFFSIZ = 255;
constexpr size_t RECORD_BUFSIZ = 300;
constexpr size_t CDOL_BUFSIZ = 64;
constexpr size_t AFL_BUFSIZE = 16;
constexpr size_t TRACK2_TAG = 0x57;
constexpr size_t TRACK2_TAG_BUFSIZ = 19;

Adafruit_PN532 nfc(PN532_SCK, PN532_MISO, PN532_MOSI, PN532_SS);

bool sendCommand(std::span<const uint8_t> cmd) {
  CHECK_PRINT_RETURN_BOOL("Failed to send command",
                          nfc.sendCommandCheckAck(cmd.data(), cmd.size()));
  CHECK_PRINT_RETURN_BOOL("Response, never received for command",
                          nfc.waitready(1000));
  return true;
}

void setup() {
  Serial.begin(115200);

  Serial.println("HELLO! version 2");
  bool success = nfc.begin();
  if (!success) {
    Serial.println("Couldn't begin PN53x board");
    // halt
    while (1)
      ;
  }
  uint32_t versiondata = nfc.getFirmwareVersion();
  if (!versiondata) {
    Serial.println("Didn't find PN53x board");
    // halt
    while (1)
      ;
  }

  // Got ok data, print it out!
  Serial.print("Found chip PN5");
  Serial.println((versiondata >> 24) & 0xFF, HEX);
  Serial.print("Firmware ver. ");
  Serial.print((versiondata >> 16) & 0xFF, DEC);
  Serial.print('.');
  Serial.println((versiondata >> 8) & 0xFF, DEC);

  CHECK_PRINT_RETURN(
      "Failed to configure RF",
      sendCommand({{PN532_COMMAND_RFCONFIGURATION, 0x05, 0xFF, 0x01, 0x00}}));
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

  static byte sendBuf[PN532_PACKBUFFSIZ];
  size_t requiredLen = toSend.size() + 1;
  CHECK_PRINT_RETURN_OPT(
      "exchangeDataICT buffer too small - bufLen: %zu, requiredLen: %zu",
      requiredLen < PN532_PACKBUFFSIZ, PN532_PACKBUFFSIZ, requiredLen);
  sendBuf[0] = PN532_COMMAND_INCOMMUNICATETHRU;
  memcpy(&sendBuf[1], toSend.data(), toSend.size());

  std::span<const uint8_t> sendBufSpan{sendBuf, requiredLen};
  CHECK_PRINT_RETURN_OPT("Failed to send exchangeDataICT Command!",
                         sendCommand(sendBufSpan));

  nfc.readdata(recvBuf.data(), recvBuf.size());

  ReadSlice readSlice(recvBuf.data(), recvBuf.size());
  CHECK_PRINT_RETURN_OPT("Failed to window to PN532 response",
                         readSlice.windowToPN532Response());

  uint8_t cmd = readSlice.readByte();
  CHECK_PRINT_RETURN_OPT("ERROR: Expected INCOMMUNICATETHRU response, got %02X",
                         cmd == PN532_COMMAND_INCOMMUNICATETHRU + 1, cmd);
  uint8_t status = readSlice.readByte();
  CHECK_PRINT_RETURN_OPT("ERROR: Invalid status from INCOMMUNICATETHRU: %02X",
                         status == 0x00, status);

  return readSlice;
}

std::optional<std::span<const uint8_t>> getTrack2Data() {
  bool foundCard = nfc.inListPassiveTarget();

  if (foundCard) {
    Serial.println("\nFound something!");
    std::optional<ReadSlice> readSliceOpt;
    ReadSlice readSlice{nullptr, 0};
    static TLVS tlvs;
    static byte sbuf[PN532_PACKBUFFSIZ];
    static byte rbuf[PN532_PACKBUFFSIZ];
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

    // Get Track 2 data
    tlvs.decodeTLVs(readSlice.data(), readSlice.len());
    TLVNode *cardData = tlvs.findTLV(TRACK2_TAG);
    if (cardData) {
      return std::span<const uint8_t>{cardData->getValue(),
                                      cardData->getValueLength()};
    }

    // Read records
    TLVNode *aflNode = tlvs.findTLV(0x94);
    CHECK_PRINT_RETURN_OPT("No AFL data found", aflNode);
    static byte aflBuf[AFL_BUFSIZE];
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

    static byte cdolBuf[CDOL_BUFSIZ];
    static WriteSlice cdolSlice(cdolBuf, sizeof(cdolBuf));
    cdolSlice.reset();
    static byte track2Buf[TRACK2_TAG_BUFSIZ];
    WriteSlice track2Slice(track2Buf, sizeof(track2Buf));
    track2Slice.reset();
    while (aflSlice.len() > 0) {
      uint8_t sfi = aflSlice.readByte();
      uint8_t recordToRead = aflSlice.readByte();
      uint8_t endRecord = aflSlice.readByte();
      // number of records included in data authentication
      uint8_t _ = aflSlice.readByte();
      for (; recordToRead <= endRecord; ++recordToRead) {
        Serial.printf("Reading record %02x\n", recordToRead);
        static byte recordbuf[RECORD_BUFSIZ];
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

    CHECK_PRINT_RETURN_OPT("No CDOL found", cdolSlice.len());
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
    // Set CIU_BitFraming register to send 8 bits
    uint16_t addr = 0x633D;
    CHECK_PRINT_RETURN_OPT(
        "Failed to set CIU_BitFraming for ECP",
        sendCommand({{PN532_COMMAND_WRITEREGISTER, (uint8_t)(addr >> 8),
                      (uint8_t)(addr & 0xFF), 0x00}}));

    // Send the ECP frame
    CHECK_PRINT_RETURN_OPT(
        "Failed to send ECP frame",
        sendCommand(
            {{PN532_COMMAND_INCOMMUNICATETHRU, 0x6a, 0x02, 0xc8, 0x01, 0x00,
              0x03, 0x00, 0x06, 0x7f, 0x01, 0x00, 0x00, 0x00, 0x4d, 0xef}}));
  }

  return std::nullopt;
}

void loop() {
  const std::optional<std::span<const uint8_t>> track2DataOpt = getTrack2Data();
  CHECK_RETURN(track2DataOpt);
  const std::span<const uint8_t> track2Data = *track2DataOpt;
  printHex("Track 2 Equivalent Data: ", track2Data);
}