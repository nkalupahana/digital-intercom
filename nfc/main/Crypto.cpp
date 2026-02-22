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
mbedtls_ecp_keypair readerKeypair;
mbedtls_ecp_point devicePublicKey;
mbedtls_mpi sharedSecret;
esp_gcm_context gcmCtx;

mbedtls_ecp_group *group = &readerKeypair.MBEDTLS_PRIVATE(grp);

void setup() {
  // TODO: Check if we need this
  if (psa_crypto_init() != PSA_SUCCESS) {
    ESP_LOGE(TAG, "Unable to init crypto");
    errorHang();
  }
  mbedtls_entropy_init(&entropyCtx);
  mbedtls_ecp_keypair_init(&readerKeypair);
  mbedtls_ecp_point_init(&devicePublicKey);
  mbedtls_mpi_init(&sharedSecret);
  mbedtls_gcm_init(&gcmCtx);
}

bool generateNewReaderKeypair() {
  CHECK_CRYPTO_RETURN_BOOL(
      "Failed to generate readerKeypair",
      mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, &readerKeypair,
                          &mbedtls_entropy_func, &entropyCtx));

  return true;
}

std::optional<
    std::pair<std::span<const uint8_t, 32>, std::span<const uint8_t, 32>>>
copyReaderPublicKeyPoints() {
  // The first byte is 0x04 to indicate uncompressed and each each point is 32
  // bytes
  static uint8_t buf[65];
  size_t outputLen;
  CHECK_CRYPTO_RETURN_OPT(
      "Failed to copy readerPublicKeyPoints into buf",
      mbedtls_ecp_point_write_binary(group, &readerKeypair.MBEDTLS_PRIVATE(Q),
                                     MBEDTLS_ECP_PF_UNCOMPRESSED, &outputLen,
                                     buf, sizeof(buf)));
  if (outputLen != sizeof(buf)) {
    ESP_LOGE(TAG, "Unexpected readerPublicKeyPoints len: %d", outputLen);
    return std::nullopt;
  }

  std::span<const uint8_t, 32> xSpan{buf + 1, 32};
  std::span<const uint8_t, 32> ySpan{buf + 1 + 32, 32};
  return std::pair{xSpan, ySpan};

  return std::nullopt;
}

std::optional<std::span<const uint8_t>>
generateEncryptedRequest(std::span<const uint8_t> deviceXY,
                         std::span<const uint8_t> transcript) {
  CHECK_CRYPTO_RETURN_OPT("Failed to read devicePublicKey",
                          mbedtls_ecp_point_read_binary(group, &devicePublicKey,
                                                        deviceXY.data(),
                                                        deviceXY.size()));
  CHECK_CRYPTO_RETURN_OPT("Invalid devicePublicKey",
                          mbedtls_ecp_check_pubkey(group, &devicePublicKey));
  CHECK_CRYPTO_RETURN_OPT(
      "Failed to computed shared secret",
      mbedtls_ecdh_compute_shared(group, &sharedSecret, &devicePublicKey,
                                  &readerKeypair.MBEDTLS_PRIVATE(d),
                                  &mbedtls_entropy_func, &entropyCtx));

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

std::optional<std::span<const uint8_t, 16>>
getIdent(std::span<const uint8_t> encodedDevicePublicKey) {
  static uint8_t hkdfOutput[16];
  static const uint8_t info[] = "BLEIdent";
  const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  CHECK_CRYPTO_RETURN_OPT("Failed to HKDF",
                          mbedtls_hkdf(md, nullptr, 0,
                                       encodedDevicePublicKey.data(),
                                       encodedDevicePublicKey.size(), info, 8,
                                       hkdfOutput, sizeof(hkdfOutput)));
  std::span<uint8_t, sizeof(hkdfOutput)> ident{hkdfOutput};
  printHex("Ident: ", ident);
  return ident;
}
} // namespace Crypto