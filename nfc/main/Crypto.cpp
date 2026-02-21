#include "Crypto.h"
#include "errors.h"
#include "utils.h"
#include <NimBLECharacteristic.h>
#include <cstdint>
#include <mbedtls/bignum.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/gcm.h>
#include <mbedtls/hkdf.h>
#include <optional>
#include <psa/crypto.h>

namespace Crypto {
uint8_t encryptedRequestBuf[REQUEST_SIZE];
mbedtls_entropy_context entropyCtx;
mbedtls_ecp_group group;
mbedtls_mpi readerPrivateKey;
mbedtls_ecp_point devicePublicKey;
mbedtls_mpi sharedSecret;
esp_gcm_context gcmCtx;

void setup() {
  // TODO: Check if we need this
  if (psa_crypto_init() != PSA_SUCCESS) {
    ESP_LOGE(TAG, "Unable to init crypto");
    errorHang();
  }
  mbedtls_entropy_init(&entropyCtx);
  mbedtls_ecp_group_init(&group);
  if (int errorCode =
          mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256R1) != 0) {
    ESP_LOGE(TAG, "Failed to load group - Code: %x", -errorCode);
    errorHang();
  }
  mbedtls_mpi_init(&readerPrivateKey);
  mbedtls_ecp_point_init(&devicePublicKey);
  mbedtls_mpi_init(&sharedSecret);
  mbedtls_gcm_init(&gcmCtx);
}

std::optional<std::span<const uint8_t>>
generateEncryptedRequest(std::span<const uint8_t> deviceXY,
                         std::span<const uint8_t> transcript) {
  CHECK_CRYPTO_RETURN_OPT("Failed to generate readerPrivateKey",
                          mbedtls_ecp_gen_privkey(&group, &readerPrivateKey,
                                                  &mbedtls_entropy_func,
                                                  &entropyCtx));
  CHECK_CRYPTO_RETURN_OPT(
      "Failed to read devicePublicKey",
      mbedtls_ecp_point_read_binary(&group, &devicePublicKey, deviceXY.data(),
                                    deviceXY.size()));
  CHECK_CRYPTO_RETURN_OPT("Invalid devicePublicKey",
                          mbedtls_ecp_check_pubkey(&group, &devicePublicKey));
  CHECK_CRYPTO_RETURN_OPT(
      "Failed to computed shared secret",
      mbedtls_ecdh_compute_shared(&group, &sharedSecret, &devicePublicKey,
                                  &readerPrivateKey, &mbedtls_entropy_func,
                                  &entropyCtx));

  static uint8_t sharedSecretBuf[32];
  CHECK_CRYPTO_RETURN_OPT("Failed to write shared secret to buffer",
                          mbedtls_mpi_write_binary(&sharedSecret,
                                                   sharedSecretBuf,
                                                   sizeof(sharedSecretBuf)));
  static uint8_t readerKey[32];
  static const uint8_t readerInfo[] = "SKReader";
  const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  CHECK_CRYPTO_RETURN_OPT("Failed to HKDF reader key",
                          mbedtls_hkdf(md, transcript.data(), transcript.size(),
                                       sharedSecretBuf, sizeof(sharedSecretBuf),
                                       readerInfo, 8, readerKey,
                                       sizeof(readerKey)));
  printHex("Reader Key: ", {readerKey, sizeof(readerKey)});
  static uint8_t deviceKey[32];
  static const uint8_t deviceInfo[] = "SKDevice";
  CHECK_CRYPTO_RETURN_OPT("Failed to HKDF device key",
                          mbedtls_hkdf(md, transcript.data(), transcript.size(),
                                       sharedSecretBuf, sizeof(sharedSecretBuf),
                                       deviceInfo, 8, deviceKey,
                                       sizeof(deviceKey)));
  printHex("Device Key: ", {deviceKey, sizeof(deviceKey)});

  static uint8_t iv[] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
  CHECK_CRYPTO_RETURN_OPT(
      "Failed to set key",
      mbedtls_gcm_setkey(&gcmCtx, MBEDTLS_CIPHER_ID_AES, readerKey, 256));
  uint8_t *tag = encryptedRequestBuf + sizeof(unencryptedRequest);
  CHECK_CRYPTO_RETURN_OPT(
      "Failed to encrypt",
      mbedtls_gcm_crypt_and_tag(&gcmCtx, MBEDTLS_GCM_ENCRYPT,
                                sizeof(unencryptedRequest), iv, sizeof(iv),
                                nullptr, 0, unencryptedRequest,
                                encryptedRequestBuf, TAG_SIZE, tag));

  std::span<const uint8_t> encryptedRequest(encryptedRequestBuf,
                                            sizeof(encryptedRequestBuf));
  printHex("Encrypted Request: ", encryptedRequest);

  return encryptedRequest;
}

bool setIdent(NimBLECharacteristic *identCharacteristic,
              std::span<const uint8_t> encodedDevicePublicKey) {
  static uint8_t hkdfOutput[16];
  static const uint8_t info[] = "BLEIdent";
  const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  CHECK_CRYPTO_RETURN_BOOL("Failed to HKDF",
                           mbedtls_hkdf(md, nullptr, 0,
                                        encodedDevicePublicKey.data(),
                                        encodedDevicePublicKey.size(), info, 8,
                                        hkdfOutput, sizeof(hkdfOutput)));
  printHex("Ident: ", {hkdfOutput, sizeof(hkdfOutput)});
  identCharacteristic->setValue(hkdfOutput, sizeof(hkdfOutput));

  return true;
}
} // namespace Crypto