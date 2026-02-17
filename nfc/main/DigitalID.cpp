#include "NFC.h"
#include "NdefMessage.h"
#include "NimBLECharacteristic.h"
#include "NimBLELocalValueAttribute.h"
#include "NimBLEServer.h"
#include "Slice.h"
#include "cbor.h"
#include "errors.h"
#include "utils.h"
#include <NdefRecord.h>
#include <NimBLEDevice.h>
#include <cstdint>
#include <optional>
#include <span>
#include <tlv.h>

namespace DigitalID {
class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *pServer, NimBLEConnInfo &connInfo) override {
    Serial.println("Connected!");
    NimBLEServerCallbacks::onConnect(pServer, connInfo);
  };

  void onDisconnect(NimBLEServer *pServer, NimBLEConnInfo &connInfo,
                    int reason) override {
    Serial.println("Disconnected!");
    NimBLEServerCallbacks::onDisconnect(pServer, connInfo, reason);
  };
} serverCallbacks;

class IdentCharacteristicCallbacks : public NimBLECharacteristicCallbacks {
  void onRead(NimBLECharacteristic *pCharacteristic,
              NimBLEConnInfo &connInfo) override {
    Serial.println("Read!");
    NimBLECharacteristicCallbacks::onRead(pCharacteristic, connInfo);
  };
} identCharacteristicCallbacks;

class StateCharacteristicCallbacks : public NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic *pCharacteristic,
                   NimBLEConnInfo &connInfo, uint16_t subValue) override {
    Serial.println("Subscribed!");
    NimBLECharacteristicCallbacks::onSubscribe(pCharacteristic, connInfo,
                                               subValue);
  };
} stateCharacteristicCallbacks;

uint8_t rbuf[PN532_PACKBUFFSIZ];
uint8_t readerPublicKeyBuf[PN532_PACKBUFFSIZ];
uint8_t sbuf[PN532_PACKBUFFSIZ];
WriteSlice writeSlice(sbuf, PN532_PACKBUFFSIZ);
NimBLEServer *pServer = nullptr;
NimBLEService *pService = nullptr;
NimBLECharacteristic *stateCharacteristic = nullptr;
NimBLECharacteristic *clientToServerCharacteristic = nullptr;
NimBLECharacteristic *serverToClientCharacteristic = nullptr;
NimBLECharacteristic *identCharacteristic = nullptr;
NimBLEAdvertising *pAdvertising = nullptr;

std::optional<std::span<const uint8_t>> readNdefFile(bool isCC) {
  std::optional<ReadSlice> readSliceOpt;
  ReadSlice readSlice{nullptr, 0};
  uint8_t binaryLength = 0;
  uint8_t retries = 0;

  while (binaryLength == 0 && retries < 5) {
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
    binaryLength = readSlice.span()[1];

    if (binaryLength == 0) {
      ESP_LOGI(TAG, "No data from NDEF file, retrying...");
      ++retries;
      delay(100);
    }
  }

  if (retries == 5) {
    ESP_LOGE(TAG, "Did not get any data from NDEF file");
    return std::nullopt;
  }

  if (!isCC)
    binaryLength += 2;

  // Read data
  writeSlice.reset();
  CHECK_RETURN_OPT(writeSlice.append({{0x00, 0xB0, 0x00, 0x00, binaryLength}}));
  readSliceOpt = NFC::exchangeData("Sending read: ", writeSlice.span(), rbuf);
  CHECK_RETURN_OPT(readSliceOpt);
  readSlice = *readSliceOpt;

  return readSlice.span().subspan(2, readSlice.span().size() - 2);
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

std::optional<std::span<const uint8_t>> performHandoff() {
  std::optional<ReadSlice> readSliceOpt;
  ReadSlice readSlice{nullptr, 0};
  uint8_t ndefPayloadBuf[PN532_PACKBUFFSIZ];

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
  auto ccDataSpan = std::span<const uint8_t>(*ccData);
  // Based on spec, CC data should be at least 13 bytes
  // (15 bytes - 2 bytes for length at beginning)
  CHECK_PRINT_RETURN_OPT("CC data is not at least 13 bytes",
                         ccDataSpan.size() >= 13);
  auto ccTlvSpan = ccDataSpan.subspan(5, ccDataSpan.size() - 5);
  printHex("CC TLV: ", ccTlvSpan);
  TLVS tlvs;
  tlvs.decodeTLVs(ccTlvSpan.data(), ccTlvSpan.size());
  TLVNode *fileControlTag = tlvs.findTLV(0x04);
  CHECK_PRINT_RETURN_OPT("Failed to get file control tag from CC data",
                         fileControlTag != nullptr);
  std::span<const uint8_t> fileControlValue{fileControlTag->getValue(),
                                            fileControlTag->getValueLength()};
  CHECK_PRINT_RETURN_OPT("File control tag value is not at least length 2",
                         fileControlValue.size() >= 2);

  // SELECT NDEF File
  writeSlice.reset();
  CHECK_RETURN_OPT(writeSlice.appendApduCommand(
      0x00, 0xA4, 0x00, 0x0C, {{fileControlValue[0], fileControlValue[1]}}));
  readSliceOpt =
      NFC::exchangeData("Sending NDEF Select File: ", writeSlice.span(), rbuf);
  CHECK_RETURN_OPT(readSliceOpt);
  readSlice = *readSliceOpt;

  // Read NDEF record. Should contain an NDEF record of type Tp (service
  // parameter) with data including "urn:nfc:sn:handover". We may want to
  // parse and verify this more robustly.
  auto initialNdefData = readNdefFile(false);
  CHECK_RETURN_OPT(initialNdefData);

  auto initialNdefDataSpan = std::span<const uint8_t>(*initialNdefData);
  auto initialNdefMessage =
      NdefMessage(initialNdefDataSpan.data(), initialNdefDataSpan.size());
  CHECK_PRINT_RETURN_OPT(
      "Initial NDEF message does not contain one NDEF record",
      initialNdefMessage.getRecordCount() == 1);
  auto serviceParameterRecord = initialNdefMessage.getRecord(0);
  CHECK_PRINT_RETURN_OPT("Record is not a service parameter",
                         serviceParameterRecord.getType() == "Tp");
  CHECK_PRINT_RETURN_OPT("Service parameter payload length is 0",
                         serviceParameterRecord.getPayloadLength() > 0);
  serviceParameterRecord.getPayload(ndefPayloadBuf);
  auto payloadSpan = std::span<const uint8_t>(
      ndefPayloadBuf, serviceParameterRecord.getPayloadLength());
  auto payloadHandoverSearch =
      std::ranges::search(payloadSpan, std::string_view("urn:nfc:sn:handover"));
  CHECK_PRINT_RETURN_OPT(
      "Handover service not in the service parameter record payload",
      payloadHandoverSearch.size() > 0);

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

  // From NDEF Exchange Protocol 1.0: 4.3 TNEP Status Message
  // If the NFC Tag Device has received a Service Select Message with a known
  // Service, it will return a TNEP Status Message to confirm a successful
  // Service selection.
  // https://github.com/openwallet-foundation/multipaz/blob/main/multipaz/src/commonMain/kotlin/org/multipaz/mdoc/nfc/MdocNfcEngagementHelper.kt#L224
  auto serviceSelectedResponseSpan =
      std::span<const uint8_t>(*serviceSelectedResponse);

  auto statusMessage = NdefMessage(serviceSelectedResponseSpan.data(),
                                   serviceSelectedResponseSpan.size());

  CHECK_PRINT_RETURN_OPT(
      "Service selected response does not contain one NDEF record",
      statusMessage.getRecordCount() == 1);
  auto statusRecord = statusMessage.getRecord(0);
  CHECK_PRINT_RETURN_OPT("Record is not a TNEP Status Message",
                         statusRecord.getType() == "Te");
  CHECK_PRINT_RETURN_OPT("Status record payload length is not 1",
                         statusRecord.getPayloadLength() == 1);
  byte statusCode = 0xFF;
  statusRecord.getPayload(&statusCode);
  CHECK_PRINT_RETURN_OPT("Status code is not 0x00", statusCode == 0x00);

  // Send handover request
  // Generated via tools/multipaz-sandbox
  auto handoverRequestSpan = std::span<const uint8_t>(
      {0x91, 0x02, 0x0A, 0x48, 0x72, 0x15, 0xD1, 0x02, 0x04, 0x61, 0x63, 0x01,
       0x01, 0x30, 0x00, 0x1C, 0x1E, 0x06, 0x0A, 0x69, 0x73, 0x6F, 0x2E, 0x6F,
       0x72, 0x67, 0x3A, 0x31, 0x38, 0x30, 0x31, 0x33, 0x3A, 0x72, 0x65, 0x61,
       0x64, 0x65, 0x72, 0x65, 0x6E, 0x67, 0x61, 0x67, 0x65, 0x6D, 0x65, 0x6E,
       0x74, 0x6D, 0x64, 0x6F, 0x63, 0x72, 0x65, 0x61, 0x64, 0x65, 0x72, 0xA1,
       0x00, 0x63, 0x31, 0x2E, 0x30, 0x5A, 0x20, 0x15, 0x01, 0x61, 0x70, 0x70,
       0x6C, 0x69, 0x63, 0x61, 0x74, 0x69, 0x6F, 0x6E, 0x2F, 0x76, 0x6E, 0x64,
       0x2E, 0x62, 0x6C, 0x75, 0x65, 0x74, 0x6F, 0x6F, 0x74, 0x68, 0x2E, 0x6C,
       0x65, 0x2E, 0x6F, 0x6F, 0x62, 0x30, 0x02, 0x1C, 0x00, 0x11, 0x07, 0xA4,
       0xB2, 0x31, 0xD2, 0x94, 0x69, 0x0B, 0xA9, 0xF9, 0x4F, 0x3C, 0x09, 0x40,
       0x60, 0x18, 0x82});

  // Write to file: length + message
  writeSlice.reset();
  CHECK_RETURN_OPT(writeSlice.append(
      {{0x00, 0xD6, 0x00, 0x00,
        static_cast<uint8_t>(handoverRequestSpan.size() + 2), 0,
        static_cast<uint8_t>(handoverRequestSpan.size())}}));
  writeSlice.append(handoverRequestSpan);
  readSliceOpt =
      NFC::exchangeData("Writing Handover Request: ", writeSlice.span(), rbuf);
  CHECK_RETURN_OPT(readSliceOpt);
  readSlice = *readSliceOpt;

  auto handoverResponse = readNdefFile(false);
  CHECK_RETURN_OPT(handoverResponse);
  printHex("Handover Response: ", *handoverResponse);

  auto handoverResponseSpan = std::span<const uint8_t>(*handoverResponse);
  auto handoverResponseMessage =
      NdefMessage(handoverResponseSpan.data(), handoverResponseSpan.size());
  std::optional<std::span<const uint8_t>> encodedDeviceEngagementOpt =
      std::nullopt;
  for (int i = 0; i < handoverResponseMessage.getRecordCount(); i++) {
    auto record = handoverResponseMessage.getRecord(i);
    if (record.getType() == "iso.org:18013:deviceengagement" &&
        record.getId() == "mdoc") {
      record.getPayload(ndefPayloadBuf);
      encodedDeviceEngagementOpt =
          std::span<const uint8_t>(ndefPayloadBuf, record.getPayloadLength());
    }
  }
  CHECK_RETURN_OPT(encodedDeviceEngagementOpt);
  auto encodedDeviceEngagementSpan = *encodedDeviceEngagementOpt;
  printHex("Encoded device engagement: ", encodedDeviceEngagementSpan);

  CborParser parser;
  CborValue value;
  CHECK_PRINT_RETURN_OPT("CBOR parser fialed to initialize",
                         cbor_parser_init(encodedDeviceEngagementSpan.data(),
                                          encodedDeviceEngagementSpan.size(), 0,
                                          &parser, &value) == CborNoError);
  CHECK_PRINT_RETURN_OPT("CBOR value is not map", cbor_value_is_map(&value));
  CHECK_PRINT_RETURN_OPT("Failed to enter map",
                         cbor_value_enter_container(&value, &value) ==
                             CborNoError);
  std::optional<CborValue> keyOpt = std::nullopt;
  while (!cbor_value_at_end(&value)) {
    if (cbor_value_is_unsigned_integer(&value)) {
      uint64_t key;
      CHECK_PRINT_RETURN_OPT("Failed to get key",
                             cbor_value_get_uint64(&value, &key) ==
                                 CborNoError);
      if (key == 1) {
        keyOpt = value;
        break;
      }
    }
    CHECK_PRINT_RETURN_OPT("Failed to advance",
                           cbor_value_advance(&value) == CborNoError);
  }
  CHECK_RETURN_OPT(keyOpt);
  CHECK_PRINT_RETURN_OPT("Failed to advance to data",
                         cbor_value_advance(&value) == CborNoError);
  CHECK_PRINT_RETURN_OPT("Data is not array", cbor_value_is_array(&value));
  CHECK_PRINT_RETURN_OPT("Failed to enter array",
                         cbor_value_enter_container(&value, &value) ==
                             CborNoError);
  CHECK_PRINT_RETURN_OPT("First value is not uint",
                         cbor_value_is_unsigned_integer(&value));
  uint64_t cipherSuiteIdentifier;
  CHECK_PRINT_RETURN_OPT(
      "Failed to get cipher suite identifier",
      cbor_value_get_uint64(&value, &cipherSuiteIdentifier) == CborNoError);
  CHECK_PRINT_RETURN_OPT("Cipher suite identifier is not 1, which is the only "
                         "identifier we know about",
                         cipherSuiteIdentifier == 1);
  CHECK_PRINT_RETURN_OPT("Failed to advance to tagged reader public key",
                         cbor_value_advance(&value) == CborNoError);
  CHECK_PRINT_RETURN_OPT("Tagged reader public key is not tagged",
                         cbor_value_is_tag(&value));
  CborTag readerPublicKeyTag;
  CHECK_PRINT_RETURN_OPT("Failed to get reader public key tag",
                         cbor_value_get_tag(&value, &readerPublicKeyTag) ==
                             CborNoError);
  CHECK_PRINT_RETURN_OPT("Reader public key tag is not 24",
                         readerPublicKeyTag == 24);
  CHECK_PRINT_RETURN_OPT("Failed to advance to reader public key",
                         cbor_value_advance(&value) == CborNoError);
  CHECK_PRINT_RETURN_OPT("Reader public key is not byte string",
                         cbor_value_is_byte_string(&value));
  size_t readerPublicKeyLength = PN532_PACKBUFFSIZ;
  CHECK_PRINT_RETURN_OPT("Failed to get reader public key",
                         cbor_value_copy_byte_string(&value, readerPublicKeyBuf,
                                                     &readerPublicKeyLength,
                                                     &value) == CborNoError);
  std::span<const uint8_t> readerPublicKeySpan(readerPublicKeyBuf,
                                               readerPublicKeyLength);
  printHex("Reader public key: ", readerPublicKeySpan);

  // TODO: also need to return full handover response to be used by the caller
  // for encryption stuff
  if (pAdvertising) {
    pAdvertising->start();
  } else {
    ESP_LOGE(TAG,
             "Tried to start advertising, but advertising is not initialized");
  }
  return readerPublicKeySpan;
}

void setupBLEServer() {
  NimBLEDevice::init("Digital Intercom");
  pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(&serverCallbacks);
  pService = pServer->createService("82186040-093c-4ff9-a90b-6994d231b2a4");
  stateCharacteristic = pService->createCharacteristic(
      "00000005-A123-48CE-896B-4C76973373E6",
      NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::WRITE_NR);
  stateCharacteristic->setCallbacks(&stateCharacteristicCallbacks);
  clientToServerCharacteristic = pService->createCharacteristic(
      "00000006-A123-48CE-896B-4C76973373E6", NIMBLE_PROPERTY::WRITE_NR);
  serverToClientCharacteristic = pService->createCharacteristic(
      "00000007-A123-48CE-896B-4C76973373E6", NIMBLE_PROPERTY::NOTIFY);
  identCharacteristic = pService->createCharacteristic(
      "00000008-A123-48CE-896B-4C76973373E6", NIMBLE_PROPERTY::READ);
  identCharacteristic->setCallbacks(&identCharacteristicCallbacks);

  pService->start();
  identCharacteristic->setValue("Uninitialized");

  pAdvertising = pServer->getAdvertising();
  pAdvertising->clearData();
  pAdvertising->addServiceUUID(pService->getUUID());
  pAdvertising->enableScanResponse(true);
  pAdvertising->setName("Digital Intercom");
}

} // namespace DigitalID