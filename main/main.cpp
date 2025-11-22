// #define MIFAREDEBUG
// #define PN532DEBUG
#include "Slice.h"
#include "errors.h"

#include <Adafruit_PN532.h>

#include <initializer_list>
#include <span>
#include <tlv.h>

#define PN532_SCK (18)
#define PN532_MOSI (23)
#define PN532_SS (5)
#define PN532_MISO (19)

Adafruit_PN532 nfc(PN532_SCK, PN532_MISO, PN532_MOSI, PN532_SS);

#define PN532_PACKBUFFSIZ 255
#define RECORD_BUFSIZ 300
byte sbuf[PN532_PACKBUFFSIZ];
byte sbuf2[PN532_PACKBUFFSIZ];
byte rbuf[PN532_PACKBUFFSIZ];
byte tlvbuf[PN532_PACKBUFFSIZ];
byte cdolbuf[PN532_PACKBUFFSIZ];
byte recordbuf[RECORD_BUFSIZ];
WriteSlice recordSlice(recordbuf, RECORD_BUFSIZ);
WriteSlice writeSlice(sbuf, PN532_PACKBUFFSIZ);

byte cdolBuf[64];
WriteSlice cdolSlice(cdolBuf, sizeof(cdolBuf));

TLVS tlvs;

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
  nfc.begin();

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

void printHex(const char *pre, const byte *data, size_t len) {
  Serial.print(pre);
  for (int i = 0; i < len; i++) {
    Serial.print(data[i], HEX);
  }
  Serial.println();
}

std::optional<ReadSlice> exchangeData(const char *pre, WriteSlice &writeSlice,
                                      byte *rbuf) {
  printHex(pre, writeSlice.data(), writeSlice.len());

  uint8_t responseLength = PN532_PACKBUFFSIZ;
  CHECK_PRINT_RETURN_VAL("Failed to get response for InDataExchange",
                         nfc.inDataExchange(writeSlice.data(), writeSlice.len(),
                                            rbuf, &responseLength),
                         std::nullopt);

  printHex("Received data: ", rbuf, responseLength);

  ReadSlice readSlice(rbuf, responseLength);
  uint8_t sw2 = readSlice.readByteFromEnd();
  uint8_t sw1 = readSlice.readByteFromEnd();
  uint16_t sw = (sw1 << 8) | sw2;
  CHECK_PRINT_RETURN_VAL("Unexpected status word: %02x\n", sw == 0x9000,
                         std::nullopt, sw);

  return readSlice;
}

std::optional<ReadSlice> exchangeDataICT(const char *pre,
                                         WriteSlice &writeSlice, byte *rbuf) {
  printHex(pre, writeSlice.data(), writeSlice.len());

  sbuf2[0] = PN532_COMMAND_INCOMMUNICATETHRU;
  memcpy(&sbuf2[1], writeSlice.data(), writeSlice.len());

  if (!nfc.sendCommandCheckAck(sbuf2, writeSlice.len() + 1)) {
    Serial.println("Failed to send command!");
    return std::nullopt;
  }

  if (!nfc.waitready(1000)) {
    Serial.println("Response never received for APDU...");
    return std::nullopt;
  }

  nfc.readdata(rbuf, PN532_PACKBUFFSIZ);

  ReadSlice readSlice(rbuf, PN532_PACKBUFFSIZ);
  if (!readSlice.windowToPN532Response()) {
    Serial.println("Failed to window to PN532 response");
    return std::nullopt;
  }

  if (uint8_t cmd =
          readSlice.readByte() != PN532_COMMAND_INCOMMUNICATETHRU + 1) {
    Serial.printf("ERROR: Expected INCOMMUNICATETHRU response, got %02X\n",
                  cmd);
    return std::nullopt;
  }
  if (uint8_t status = readSlice.readByte() != 0x00) {
    Serial.printf("ERROR: Invalid status from INCOMMUNICATETHRU: %02X\n",
                  status);
    return std::nullopt;
  }

  return readSlice;
}

void loop() {
  bool foundCard = nfc.inListPassiveTarget();

  if (foundCard) {
    Serial.println("Found something!");
    std::optional<ReadSlice> readSliceOpt;
    ReadSlice readSlice{nullptr, 0};

    // SELECT PPSE
    const char *cmd = "2PAY.SYS.DDF01";
    writeSlice.reset();
    CHECK_RETURN(writeSlice.appendApduCommand(0x00, 0xA4, 0x04, 0x00, cmd));

    readSliceOpt = exchangeData("Sending SELECT PPSE: ", writeSlice, tlvbuf);
    CHECK_RETURN(readSliceOpt.has_value());

    // SELECT AID
    tlvs.decodeTLVs(tlvbuf, readSliceOpt.value().len());
    TLVNode *aid = tlvs.findTLV(0x4F);
    if (aid == nullptr) {
      Serial.println("Failed to get AID from card!");
      return;
    }

    writeSlice.reset();
    CHECK_RETURN(writeSlice.appendApduCommand(
        0x00, 0xA4, 0x04, 0x00, aid->getValue(), aid->getValueLength()));

    readSliceOpt = exchangeData("Sending SELECT AID: ", writeSlice, tlvbuf);
    CHECK_RETURN(readSliceOpt.has_value());

    // GPO
    tlvs.reset();
    tlvs.decodeTLVs(tlvbuf, readSliceOpt.value().len());
    TLVNode *pdolData = tlvs.findTLV(0x9F38);

    writeSlice.reset();
    if (pdolData == nullptr) {
      Serial.println("Failed to find PDOL. Using empty DOL");
      CHECK_RETURN(
          writeSlice.appendApduCommand(0x80, 0xA8, 0x00, 0x00, {0x83, 0x00}));
    } else {
      ReadSlice pdol(pdolData->getValue(), pdolData->getValueLength());
      CHECK_RETURN(writeSlice.appendApduCommand(
          0x80, 0xA8, 0x00, 0x00, [&pdol](WriteSlice &slice) {
            return slice.appendTLV(0x83, [&pdol](WriteSlice &slice) {
              return slice.appendFromDol(pdol);
            });
          }));
    }

    readSliceOpt = exchangeData("Sending GPO: ", writeSlice, tlvbuf);
    CHECK_RETURN(readSliceOpt.has_value());

    // Get Track 2 data
    tlvs.reset();
    tlvs.decodeTLVs(tlvbuf, readSliceOpt.value().len());
    TLVNode *cardData = tlvs.findTLV(0x57);
    if (cardData == nullptr) {
      Serial.println("Failed to get Track 2 Equivalent Data from card!");
    } else {
      printHex("Received Track 2 Equivalent data: ", cardData->getValue(),
               cardData->getValueLength());
    }

    // Read records
    TLVNode *aflData = tlvs.findTLV(0x94);
    CHECK_PRINT_RETURN("No AFL data found", aflData);

    const uint8_t *aflDataPtr = aflData->getValue();
    uint32_t remainingLength = aflData->getValueLength();

    // Figure out the last sent block number
    writeSlice.reset();
    writeSlice.append({0xB2});
    readSliceOpt = exchangeDataICT("R(NACK)", writeSlice, rbuf);
    CHECK_RETURN(readSliceOpt.has_value());

    uint8_t blockNum =
        ((readSliceOpt.value().readByte() & 0xA0) != 0) ? 0x1 : 0x0;

    cdolSlice.reset();
    while (remainingLength > 0) {
      uint8_t recordToRead = aflDataPtr[1];
      uint8_t endRecord = aflDataPtr[2];
      while (recordToRead <= endRecord) {
        Serial.printf("Reading record %02x\n", recordToRead);
        recordSlice.reset();

        writeSlice.reset();
        blockNum ^= 1;
        uint8_t pcb = 0x2 | blockNum;
        uint8_t p2 = (aflDataPtr[0] & 0xF8) | 0x4;
        // TODO: add appendApduCommand that takes no data
        CHECK_PRINT_RETURN(
            "Failed to build read record command",
            writeSlice.append({pcb, 0x00, 0xB2, recordToRead, p2, 0x00}));

        // TODO: Take in buffer directly
        readSliceOpt = exchangeDataICT("Read Record: ", writeSlice, rbuf);
        while (true) {
          CHECK_RETURN(readSliceOpt);
          readSlice = readSliceOpt.value();

          uint8_t responsePcb = readSlice.readByte();
          CHECK_PRINT_RETURN(
              "Failed to buffer record data",
              recordSlice.append(readSlice.data(), readSlice.len()));

          // If no more data, break
          if ((responsePcb & 0x10) == 0) {
            break;
          }
          // Send R(ACK) to get more data
          writeSlice.reset();
          blockNum ^= 1;
          pcb = 0xA2 | blockNum;
          writeSlice.append({pcb});
          readSliceOpt = exchangeDataICT("R(ACK): ", writeSlice, rbuf);
        }
        printHex("Full Read Record response: ", recordbuf, recordSlice.len());
        tlvs.reset();
        tlvs.decodeTLVs(recordbuf, recordSlice.len());
        if (TLVNode *cdolNode = tlvs.findTLV(0x8C)) {
          CHECK_PRINT_RETURN("Found duplicate CDOL tags", cdolSlice.len() == 0);
          cdolSlice.append(cdolNode->getValue(), cdolNode->getValueLength());
        }

        recordToRead += 1;
      }

      remainingLength -= 4;
      aflDataPtr += 4;
    }

    CHECK_PRINT_RETURN("No CDOL found", cdolSlice.len());

    printHex("CDOL data: ", cdolSlice.data(), cdolSlice.len());
    writeSlice.reset();
    bool builtAC = writeSlice.appendApduCommand(
        0x80, 0xAE, 0x50, 0x00, [](WriteSlice &slice) {
          ReadSlice cdolReadSlice{cdolSlice.data(), cdolSlice.len()};
          return slice.appendFromDol(cdolReadSlice);
        });
    CHECK_PRINT_RETURN("Failed to build GENERATE AC command", builtAC);

    readSliceOpt = exchangeData("Sending GENERATE AC: ", writeSlice, rbuf);
    CHECK_PRINT_RETURN("Failed to send ", readSliceOpt.has_value());
  } else {
    // Set CIU_BitFraming register to send 8 bits
    uint16_t addr = 0x633D;
    CHECK_PRINT_RETURN(
        "Failed to set CIU_BitFraming for ECP",
        sendCommand({{PN532_COMMAND_WRITEREGISTER, (uint8_t)(addr >> 8),
                      (uint8_t)(addr & 0xFF), 0x00}}));

    // Send the ECP frame
    CHECK_PRINT_RETURN(
        "Failed to send ECP frame",
        sendCommand(
            {{PN532_COMMAND_INCOMMUNICATETHRU, 0x6a, 0x02, 0xc8, 0x01, 0x00,
              0x03, 0x00, 0x06, 0x7f, 0x01, 0x00, 0x00, 0x00, 0x4d, 0xef}}));
  }
}