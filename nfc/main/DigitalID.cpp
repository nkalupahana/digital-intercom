#include "Crypto.h"
#include "NFC.h"
#include "NdefMessage.h"
#include "NimBLEAttValue.h"
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
  void onWrite(NimBLECharacteristic *pCharacteristic,
               NimBLEConnInfo &connInfo) override {
    NimBLEAttValue value = pCharacteristic->getValue();
    printHex("Received value to state characteristic: ",
             {value.data(), value.length()});
    NimBLECharacteristicCallbacks::onWrite(pCharacteristic, connInfo);
  };
} stateCharacteristicCallbacks;

uint8_t rbuf[PN532_PACKBUFFSIZ];
// TODO: really, only one buffer is needed, with two spans.
uint8_t encodedDevicePublicKeyBuf[PN532_PACKBUFFSIZ];
uint8_t devicePublicKeyBuf[PN532_PACKBUFFSIZ];
// Generated via tools/ble-server, originally from spec example
// TODO: switch to auto-generation
uint8_t encodedReaderPublicKey[] = {
    0xa4, 0x01, 0x02, 0x20, 0x01, 0x21, 0x58, 0x20, 0x60, 0xe3, 0x39,
    0x23, 0x85, 0x04, 0x1f, 0x51, 0x40, 0x30, 0x51, 0xf2, 0x41, 0x55,
    0x31, 0xcb, 0x56, 0xdd, 0x3f, 0x99, 0x9c, 0x71, 0x68, 0x70, 0x13,
    0xaa, 0xc6, 0x76, 0x8b, 0xc8, 0x18, 0x7e, 0x22, 0x58, 0x20, 0xe5,
    0x8d, 0xeb, 0x8f, 0xdb, 0xe9, 0x07, 0xf7, 0xdd, 0x53, 0x68, 0x24,
    0x55, 0x51, 0xa3, 0x47, 0x96, 0xf7, 0xd2, 0x21, 0x5c, 0x44, 0x0c,
    0x33, 0x9b, 0xb0, 0xf7, 0xb6, 0x7b, 0xec, 0xcd, 0xfa};
// Generated via tools/multipaz-sandbox
uint8_t handoverRequestBuf[] = {
    0x91, 0x02, 0x0A, 0x48, 0x72, 0x15, 0xD1, 0x02, 0x04, 0x61, 0x63, 0x01,
    0x01, 0x30, 0x00, 0x1C, 0x1E, 0x06, 0x0A, 0x69, 0x73, 0x6F, 0x2E, 0x6F,
    0x72, 0x67, 0x3A, 0x31, 0x38, 0x30, 0x31, 0x33, 0x3A, 0x72, 0x65, 0x61,
    0x64, 0x65, 0x72, 0x65, 0x6E, 0x67, 0x61, 0x67, 0x65, 0x6D, 0x65, 0x6E,
    0x74, 0x6D, 0x64, 0x6F, 0x63, 0x72, 0x65, 0x61, 0x64, 0x65, 0x72, 0xA1,
    0x00, 0x63, 0x31, 0x2E, 0x30, 0x5A, 0x20, 0x15, 0x01, 0x61, 0x70, 0x70,
    0x6C, 0x69, 0x63, 0x61, 0x74, 0x69, 0x6F, 0x6E, 0x2F, 0x76, 0x6E, 0x64,
    0x2E, 0x62, 0x6C, 0x75, 0x65, 0x74, 0x6F, 0x6F, 0x74, 0x68, 0x2E, 0x6C,
    0x65, 0x2E, 0x6F, 0x6F, 0x62, 0x30, 0x02, 0x1C, 0x00, 0x11, 0x07, 0xA4,
    0xB2, 0x31, 0xD2, 0x94, 0x69, 0x0B, 0xA9, 0xF9, 0x4F, 0x3C, 0x09, 0x40,
    0x60, 0x18, 0x82};
auto handoverRequestSpan =
    std::span<const uint8_t>(handoverRequestBuf, sizeof(handoverRequestBuf));
uint8_t sessionTranscriptBuf[PN532_PACKBUFFSIZ * 3];
constexpr size_t COORD_LENGTH = 32;
uint8_t deviceXYPubKeyEncodedBuf[COORD_LENGTH * 2 + 1];
std::span<uint8_t> devicePubKeyX{deviceXYPubKeyEncodedBuf + 1, COORD_LENGTH};
std::span<uint8_t> devicePubKeyY{deviceXYPubKeyEncodedBuf + 1 + COORD_LENGTH,
                                 COORD_LENGTH};
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
  deviceXYPubKeyEncodedBuf[0] = 0x04; // Uncompressed public key
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

  // Get public key out of device engagement
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
  bool keyFound = false;
  while (!cbor_value_at_end(&value)) {
    if (cbor_value_is_unsigned_integer(&value)) {
      uint64_t key;
      CHECK_PRINT_RETURN_OPT("Failed to get key",
                             cbor_value_get_uint64(&value, &key) ==
                                 CborNoError);
      if (key == 1) {
        keyFound = true;
        break;
      }
    }
    CHECK_PRINT_RETURN_OPT("Failed to advance",
                           cbor_value_advance(&value) == CborNoError);
  }
  CHECK_PRINT_RETURN_OPT("Key 1 not found", keyFound);
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
  CHECK_PRINT_RETURN_OPT("Failed to advance to tagged device public key",
                         cbor_value_advance(&value) == CborNoError);
  CHECK_PRINT_RETURN_OPT("Tagged device public key is not tagged",
                         cbor_value_is_tag(&value));
  CborTag devicePublicKeyTag;
  CHECK_PRINT_RETURN_OPT("Failed to get device public key tag",
                         cbor_value_get_tag(&value, &devicePublicKeyTag) ==
                             CborNoError);
  CHECK_PRINT_RETURN_OPT("device public key tag is not 24",
                         devicePublicKeyTag == 24);
  CHECK_PRINT_RETURN_OPT("Failed to advance to device public key",
                         cbor_value_advance(&value) == CborNoError);
  CHECK_PRINT_RETURN_OPT("device public key is not byte string",
                         cbor_value_is_byte_string(&value));
  size_t devicePublicKeyLength = PN532_PACKBUFFSIZ;
  CHECK_PRINT_RETURN_OPT("Failed to get device public key",
                         cbor_value_copy_byte_string(&value, devicePublicKeyBuf,
                                                     &devicePublicKeyLength,
                                                     &value) == CborNoError);
  std::span<const uint8_t> devicePublicKeySpan(devicePublicKeyBuf,
                                               devicePublicKeyLength);
  printHex("device public key: ", devicePublicKeySpan);

  // Write extracted public key back to CBOR, with tag
  // This could definintely be improved.
  CborEncoder encoder;
  cbor_encoder_init(&encoder, encodedDevicePublicKeyBuf, PN532_PACKBUFFSIZ, 0);
  CHECK_PRINT_RETURN_OPT("Failed to encode device public key",
                         cbor_encode_tag(&encoder, devicePublicKeyTag) ==
                             CborNoError);
  CHECK_PRINT_RETURN_OPT(
      "Failed to encode device public key",
      cbor_encode_byte_string(&encoder, devicePublicKeySpan.data(),
                              devicePublicKeySpan.size()) == CborNoError);
  std::span<const uint8_t> encodedDevicePublicKeySpan(
      encodedDevicePublicKeyBuf,
      cbor_encoder_get_buffer_size(&encoder, encodedDevicePublicKeyBuf));
  printHex("Encoded device public key: ", encodedDevicePublicKeySpan);

  Crypto::setIdent(identCharacteristic, encodedDevicePublicKeySpan);

  // Extract x and y from the device public key
  CborParser keyParser;
  CborValue keyValue;
  std::optional<std::span<const uint8_t>> xSpanOpt = std::nullopt;
  std::optional<std::span<const uint8_t>> ySpanOpt = std::nullopt;
  CHECK_PRINT_RETURN_OPT(
      "CBOR parser failed to initialize",
      cbor_parser_init(devicePublicKeySpan.data(), devicePublicKeySpan.size(),
                       0, &keyParser, &keyValue) == CborNoError);
  CHECK_PRINT_RETURN_OPT("CBOR value is not map", cbor_value_is_map(&keyValue));
  CHECK_PRINT_RETURN_OPT("Failed to enter map",
                         cbor_value_enter_container(&keyValue, &keyValue) ==
                             CborNoError);
  // Find -2 (x) and -3 (y)
  while (!cbor_value_at_end(&keyValue) &&
         (!xSpanOpt.has_value() || !ySpanOpt.has_value())) {
    if (cbor_value_is_negative_integer(&keyValue)) {
      int key;
      CHECK_PRINT_RETURN_OPT("Failed to get key",
                             cbor_value_get_int(&keyValue, &key) ==
                                 CborNoError);
      if (key == -2) {
        CHECK_PRINT_RETURN_OPT("Failed to advance to x",
                               cbor_value_advance(&keyValue) == CborNoError);
        CHECK_PRINT_RETURN_OPT("x is not byte string",
                               cbor_value_is_byte_string(&keyValue));
        size_t xLength = devicePubKeyX.size();
        CHECK_PRINT_RETURN_OPT(
            "Failed to get x",
            cbor_value_copy_byte_string(&keyValue, devicePubKeyX.data(),
                                        &xLength, NULL) == CborNoError);
        CHECK_PRINT_RETURN_OPT("x length is not 32", xLength == COORD_LENGTH);
        xSpanOpt = devicePubKeyX;
      }

      if (key == -3) {
        CHECK_PRINT_RETURN_OPT("Failed to advance to y",
                               cbor_value_advance(&keyValue) == CborNoError);
        CHECK_PRINT_RETURN_OPT("y is not byte string",
                               cbor_value_is_byte_string(&keyValue));
        size_t yLength = devicePubKeyY.size();

        CHECK_PRINT_RETURN_OPT(
            "Failed to get y",
            cbor_value_copy_byte_string(&keyValue, devicePubKeyY.data(),
                                        &yLength, NULL) == CborNoError);
        CHECK_PRINT_RETURN_OPT("y length is not 32", yLength == COORD_LENGTH);
        ySpanOpt = devicePubKeyY;
      }
    }

    CHECK_PRINT_RETURN_OPT("Failed to advance",
                           cbor_value_advance(&keyValue) == CborNoError);
  }
  CHECK_PRINT_RETURN_OPT("Failed to get x or y",
                         xSpanOpt.has_value() && ySpanOpt.has_value());
  /// Transcript
  CborEncoder transcriptEncoder;
  cbor_encoder_init(&transcriptEncoder, sessionTranscriptBuf + 5,
                    sizeof(sessionTranscriptBuf) - 5,
                    0); // TODO: clean up offsets
  CborEncoder arrayEncoder;
  CHECK_PRINT_RETURN_OPT("Failed to create array",
                         cbor_encoder_create_array(&transcriptEncoder,
                                                   &arrayEncoder,
                                                   3) == CborNoError);

  // 1: Device engagement
  CHECK_PRINT_RETURN_OPT("Failed to add tag",
                         cbor_encode_tag(&arrayEncoder, 24) == CborNoError);
  CHECK_PRINT_RETURN_OPT(
      "Failed to add device engagement",
      cbor_encode_byte_string(&arrayEncoder, encodedDeviceEngagementSpan.data(),
                              encodedDeviceEngagementSpan.size()) ==
          CborNoError);
  // 2: Reader public key
  CHECK_PRINT_RETURN_OPT("Failed to add tag",
                         cbor_encode_tag(&arrayEncoder, 24) == CborNoError);
  CHECK_PRINT_RETURN_OPT(
      "Failed to add reader public key",
      cbor_encode_byte_string(&arrayEncoder, encodedReaderPublicKey,
                              sizeof(encodedReaderPublicKey)) == CborNoError);
  // 3: Handover array
  CborEncoder handoverArrayEncoder;
  CHECK_PRINT_RETURN_OPT("Failed to add handover array",
                         cbor_encoder_create_array(&arrayEncoder,
                                                   &handoverArrayEncoder,
                                                   2) == CborNoError);
  // 3a: Handover select (incorrectly named handover response, TODO fix)
  CHECK_PRINT_RETURN_OPT("Failed to add handover select",
                         cbor_encode_byte_string(
                             &handoverArrayEncoder, handoverResponseSpan.data(),
                             handoverResponseSpan.size()) == CborNoError);
  // 3b: Handover request
  // CHECK_PRINT_RETURN_OPT("Failed to add handover select",
  //                        cbor_encode_byte_string(
  //                            &handoverArrayEncoder,
  //                            handoverResponseSpan.data(),
  //                            handoverResponseSpan.size()) == CborNoError);
  CHECK_PRINT_RETURN_OPT(
      "Failed to add handover request",
      cbor_encode_byte_string(&handoverArrayEncoder, handoverRequestSpan.data(),
                              handoverRequestSpan.size()) == CborNoError);
  // Close containers
  CHECK_PRINT_RETURN_OPT(
      "Failed to close handover array",
      cbor_encoder_close_container(&arrayEncoder, &handoverArrayEncoder) ==
          CborNoError);
  CHECK_PRINT_RETURN_OPT("Failed to close array",
                         cbor_encoder_close_container(
                             &transcriptEncoder, &arrayEncoder) == CborNoError);
  // Add tag
  sessionTranscriptBuf[0] = 0xD8;
  sessionTranscriptBuf[1] = 0x18;
  // Interpret as bytes of 0xXXXX length
  sessionTranscriptBuf[2] = 0x59;
  size_t binaryLength = cbor_encoder_get_buffer_size(&transcriptEncoder,
                                                     sessionTranscriptBuf + 5);
  sessionTranscriptBuf[3] = binaryLength >> 8;
  sessionTranscriptBuf[4] = binaryLength & 0xFF;

  std::span<const uint8_t> transcriptSpan(sessionTranscriptBuf,
                                          binaryLength + 5);
  printHex("Transcript: ", transcriptSpan);

  // TODO: also need to return full handover response to be used by the
  // caller for encryption stuff
  if (pAdvertising) {
    pAdvertising->start();
  } else {
    ESP_LOGE(TAG, "Tried to start advertising, but advertising is not "
                  "initialized");
  }

  printHex("Device XY: ", {deviceXYPubKeyEncodedBuf});
  // Crypto::test({deviceXYPubKeyEncodedBuf});

  return std::nullopt;
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