// #define MIFAREDEBUG
// #define PN532DEBUG
#include "Adafruit_PN532.h"

#include "Slice.h"
#include "tlv.h"
#include <initializer_list>

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

TLVS tlvs;
TLVS cdol_tlvs;

void setup() {
  Serial.begin(115200);
  delay(5000);

  Serial.println("HELLO! version 2");
  nfc.begin();

  uint32_t versiondata = nfc.getFirmwareVersion();
  if (!versiondata) {
    Serial.println("Didn't find PN53x board");
    while (1)
      ; // halt
  }

  // Got ok data, print it out!
  Serial.print("Found chip PN5");
  Serial.println((versiondata >> 24) & 0xFF, HEX);
  Serial.print("Firmware ver. ");
  Serial.print((versiondata >> 16) & 0xFF, DEC);
  Serial.print('.');
  Serial.println((versiondata >> 8) & 0xFF, DEC);

  sbuf[0] = PN532_COMMAND_RFCONFIGURATION;
  sbuf[1] = 0x05;
  sbuf[2] = 0xFF;
  sbuf[3] = 0x01;
  sbuf[4] = 0x00;

  if (!nfc.sendCommandCheckAck(sbuf, 5)) {
    Serial.println("Failed to do NFC RF configuration");
  }
}

void printHex(const char *pre, const byte *data, size_t len) {
  Serial.print(pre);

  for (int i = 0; i < len; i++) {
    Serial.printf("%02X", data[i]);
  }
  Serial.println();
}

uint8_t exchangeData(const char *pre, WriteSlice &writeSlice, byte *rbuf) {
  printHex(pre, writeSlice.data(), writeSlice.len());

  uint8_t responseLength = PN532_PACKBUFFSIZ;
  if (!nfc.inDataExchange(writeSlice.data(), writeSlice.len(), rbuf,
                          &responseLength)) {
    Serial.println("Failed to get response in data exchange");
    return 0;
  }

  printHex("Received data: ", rbuf, responseLength);

  if (responseLength < 2) {
    Serial.println("WARNING: Received response with less than length 2!");
    return 0;
  }

  if (rbuf[responseLength - 2] != 0x90) {
    Serial.printf("SW1: Expected 0x90, got %02x\n", rbuf[responseLength - 2]);
    return 0;
  }

  if (rbuf[responseLength - 1] != 0x00) {
    Serial.printf("SW2: Expected 0x00, got %02x\n", rbuf[responseLength - 1]);
    return 0;
  }

  return responseLength - 2;
}

void sendAndReceive(uint8_t *buf, size_t bufLen) {
  if (!nfc.sendCommandCheckAck(buf, bufLen)) {
    Serial.println("Failed to send command");
  }

  if (!nfc.waitready(1000)) {
    Serial.println("Response never received for command");
    return;
  }

  nfc.readdata(rbuf, PN532_PACKBUFFSIZ);

  ReadSlice readSlice(rbuf, PN532_PACKBUFFSIZ);
  if (!readSlice.windowToPN532Response()) {
    Serial.println("Failed to window to PN532 response");
    return;
  }
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
  bool success = nfc.inListPassiveTarget();

  if (success) {
    Serial.println("Found something!");
    uint8_t responseLength = 0;

    // SELECT PPSE
    const char *cmd = "2PAY.SYS.DDF01";
    writeSlice.reset();
    success = writeSlice.appendApduCommand(0x00, 0xA4, 0x04, 0x00, cmd);
    if (!success)
      return;

    responseLength = exchangeData("Sending SELECT PPSE: ", writeSlice, tlvbuf);
    if (responseLength == 0)
      return;

    // SELECT AID
    tlvs.decodeTLVs(tlvbuf, responseLength);
    TLVNode *aid = tlvs.findTLV(0x4F);
    if (aid == nullptr) {
      Serial.println("Failed to get AID from card!");
      return;
    }

    writeSlice.reset();
    success = writeSlice.appendApduCommand(
        0x00, 0xA4, 0x04, 0x00, aid->getValue(), aid->getValueLength());
    if (!success)
      return;

    responseLength = exchangeData("Sending SELECT AID: ", writeSlice, tlvbuf);
    if (responseLength == 0)
      return;

    // GPO
    tlvs.reset();
    tlvs.decodeTLVs(tlvbuf, responseLength);
    TLVNode *pdolData = tlvs.findTLV(0x9F38);

    writeSlice.reset();
    if (pdolData == nullptr) {
      Serial.println("Failed to find PDOL!");
      success =
          writeSlice.appendApduCommand(0x80, 0xA8, 0x00, 0x00, {0x83, 0x00});
    } else {
      ReadSlice pdol(pdolData->getValue(), pdolData->getValueLength());
      success = writeSlice.appendApduCommand(
          0x80, 0xA8, 0x00, 0x00, [&pdol](WriteSlice &slice) {
            return slice.appendTLV(0x83, [&pdol](WriteSlice &slice) {
              return slice.appendFromDol(pdol);
            });
          });
    }
    if (!success)
      return;

    responseLength = exchangeData("Sending GPO: ", writeSlice, tlvbuf);
    if (responseLength == 0)
      return;

    // Get Track 2 data
    tlvs.reset();
    tlvs.decodeTLVs(tlvbuf, responseLength);
    TLVNode *cardData = tlvs.findTLV(0x57);
    if (cardData == nullptr) {
      Serial.println("Failed to get Track 2 Equivalent Data from card!");
    } else {
      printHex("Received Track 2 Equivalent data: ", cardData->getValue(),
               cardData->getValueLength());
    }

    // Read records
    TLVNode *aflData = tlvs.findTLV(0x94);
    if (aflData == nullptr) {
      Serial.println("No AFL data found");
      return;
    }

    const uint8_t *aflDataPtr = aflData->getValue();
    uint32_t remainingLength = aflData->getValueLength();
    // TLVNode *fddaData = nullptr;

    // Figure out the last sent block number
    writeSlice.reset();
    writeSlice.append({0xB2});
    std::optional<ReadSlice> responseOpt =
        exchangeDataICT("R(NACK)", writeSlice, rbuf);
    if (!responseOpt) {
      Serial.println("Failed to R(NACK)!");
      return;
    }
    ReadSlice response = responseOpt.value();
    printHex("R(NACK) response: ", response.data(), response.len());
    uint8_t blockNum = ((response.readByte() & 0xA0) != 0) ? 0x1 : 0x0;
    printf("Last block num: %02x\n", blockNum);

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
        if (!writeSlice.append({pcb, 0x00, 0xB2, recordToRead, p2, 0x00})) {
          Serial.println("Failed to append!");
          return;
        }
        // TODO: Take in buffer directly
        responseOpt = exchangeDataICT("Read Record: ", writeSlice, rbuf);
        while (true) {
          if (!responseOpt) {
            Serial.println("Failed to Read Record!");
            return;
          }
          response = responseOpt.value();
          uint8_t responsePcb = response.readByte();
          if (!recordSlice.append(response.data(), response.len())) {
            Serial.println("Failed to append to recordSlice!");
            return;
          }

          // Handle chaining
          if ((responsePcb & 0x10) == 0) {
            break;
          }
          // Send R(ACK)
          writeSlice.reset();
          blockNum ^= 1;
          pcb = 0xA2 | blockNum;
          writeSlice.append({pcb});
          responseOpt = exchangeDataICT("R(ACK): ", writeSlice, rbuf);
        }
        printHex("Full Read Record response: ", recordbuf, recordSlice.len());

        // if (fddaData == nullptr && responseLength > 0) {
        //   Serial.println("Looking for fddaData...");
        //   cdol_tlvs.reset();
        //   memcpy(cdolbuf, rbuf, responseLength);
        //   cdol_tlvs.decodeTLVs(cdolbuf, responseLength);
        //   fddaData = cdol_tlvs.findTLV(0x9F69);
        // }

        recordToRead += 1;
      }

      remainingLength -= 4;
      aflDataPtr += 4;
    }

    // if (fddaData == nullptr) {
    //   Serial.println("Failed to find fDDA data (CARD)!");
    //   return;
    // }

    // printHex("FDDA data: ", fddaData->getValue(),
    // fddaData->getValueLength());

    // fDDA
    // writeSlice.reset();
    // // writeSlice.append({
    // //   0x00, 0x88, 0x00, 0x00, 28,
    // //   0x01, 0x02, 0x03, 0x04, // UN,
    // //   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // $0.00 authorized
    // //   0x08, 0x40 // USD
    // // });
    // // writeSlice.append(fddaData->getValue(), fddaData->getValueLength());
    // writeSlice.append({0x00, 0x88, 0x00, 0x00});
    // writeSlice.append({0x04, 0x01, 0x01, 0x03, 0x04});
    // writeSlice.append({0x00});

    // responseLength =
    //     exchangeData("Sending INTERNAL AUTHENTICATE: ", writeSlice, rbuf);
    // if (responseLength == 0) {
    //   Serial.println("Failed!");
    // }
  } else {
    // ECP
    writeSlice.reset();
    writeSlice.append({PN532_COMMAND_WRITEREGISTER, (0x633d >> 8) & 0xFF,
                       0x633d & 0xFF, 0x00});

    if (!nfc.sendCommandCheckAck(writeSlice.data(), writeSlice.len())) {
      Serial.println("Failed to writeregister for ECP!");
    }

    writeSlice.reset();
    writeSlice.append({PN532_COMMAND_INCOMMUNICATETHRU, 0x6a, 0x02, 0xc8, 0x01,
                       0x00, 0x03, 0x00, 0x06, 0x7f, 0x01, 0x00, 0x00, 0x00,
                       0x4d, 0xef});

    if (!nfc.sendCommandCheckAck(writeSlice.data(), writeSlice.len())) {
      Serial.println("Failed to incommunicatethrough for ECP!");
    }
  }
}