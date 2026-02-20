#include "NimBLECharacteristic.h"
#include "errors.h"
#include "utils.h"
#include <cstdint>
#include <mbedtls/bignum.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/gcm.h>
#include <mbedtls/hkdf.h>
#include <psa/crypto.h>

int errorCode;

namespace Crypto {
// Generated via tools/ble-server/item-request.js
uint8_t unencryptedRequest[] = {
    0xa2, 0x67, 0x76, 0x65, 0x72, 0x73, 0x69, 0x6f, 0x6e, 0x63, 0x31, 0x2e,
    0x30, 0x6b, 0x64, 0x6f, 0x63, 0x52, 0x65, 0x71, 0x75, 0x65, 0x73, 0x74,
    0x73, 0x81, 0xa1, 0x6c, 0x69, 0x74, 0x65, 0x6d, 0x73, 0x52, 0x65, 0x71,
    0x75, 0x65, 0x73, 0x74, 0xd8, 0x18, 0x58, 0x63, 0xa2, 0x67, 0x64, 0x6f,
    0x63, 0x54, 0x79, 0x70, 0x65, 0x75, 0x6f, 0x72, 0x67, 0x2e, 0x69, 0x73,
    0x6f, 0x2e, 0x31, 0x38, 0x30, 0x31, 0x33, 0x2e, 0x35, 0x2e, 0x31, 0x2e,
    0x6d, 0x44, 0x4c, 0x6a, 0x6e, 0x61, 0x6d, 0x65, 0x53, 0x70, 0x61, 0x63,
    0x65, 0x73, 0xa1, 0x71, 0x6f, 0x72, 0x67, 0x2e, 0x69, 0x73, 0x6f, 0x2e,
    0x31, 0x38, 0x30, 0x31, 0x33, 0x2e, 0x35, 0x2e, 0x31, 0xa3, 0x6a, 0x67,
    0x69, 0x76, 0x65, 0x6e, 0x5f, 0x6e, 0x61, 0x6d, 0x65, 0xf5, 0x6b, 0x66,
    0x61, 0x6d, 0x69, 0x6c, 0x79, 0x5f, 0x6e, 0x61, 0x6d, 0x65, 0xf5, 0x6a,
    0x62, 0x69, 0x72, 0x74, 0x68, 0x5f, 0x64, 0x61, 0x74, 0x65, 0xf5};

bool test(std::span<const uint8_t> deviceXY,
          std::span<const uint8_t> transcript) {
  CHECK_PRINT_RETURN_BOOL("Unable to init crypto",
                          psa_crypto_init() == PSA_SUCCESS);

  // 1. Context definitions
  mbedtls_ecp_keypair keypair;

  // The private key hex string from your snippet
  const char *priv_hex =
      "de3b4b9e5f72dd9b58406ae3091434da48a6f9fd010d88fcb0958e2cebec947c";

  // 2. Initialize the keypair structure
  mbedtls_ecp_keypair_init(&keypair);

  // 3. Load the Curve (secp256r1 is MBEDTLS_ECP_DP_SECP256R1)
  // This is equivalent to: const curve = ecdh.getCurve("secp256r1");
  ASSERT_CODE_PRINT_RETURN_BOOL(
      "Failed to load group",
      mbedtls_ecp_group_load(&keypair.private_grp, MBEDTLS_ECP_DP_SECP256R1));

  // 4. Load the Private Key (d)
  // This parses the hex string directly into the MPI (BigNum) structure
  // Equivalent to: PrivateKey.fromBuffer(...)
  ASSERT_CODE_PRINT_RETURN_BOOL(
      "Failed to load group",
      mbedtls_mpi_read_string(&keypair.private_d, 16, priv_hex));

  // 5. Load the public key
  // ASSERT_CODE_PRINT_RETURN_BOOL(
  //     "Failed to create public key",
  //     mbedtls_ecp_point_read_string(
  //         &keypair.private_Q, 16,
  //         "65b896331da50332029c967c5802943d2068d5f9dc0886953f9c22f756f7cc59",
  //         "0ca8e45d3b2812cd1b9d1047598f5e0b29464c270d0bf7302292010c6782a0f6"));
  ASSERT_CODE_PRINT_RETURN_BOOL(
      "Failed to create public key",
      mbedtls_ecp_point_read_binary(&keypair.private_grp, &keypair.private_Q,
                                    deviceXY.data(), deviceXY.size()));

  // 6. Validate the keys (Optional but good practice)
  ASSERT_CODE_PRINT_RETURN_BOOL(
      "Invalid private key",
      mbedtls_ecp_check_privkey(&keypair.private_grp, &keypair.private_d));

  mbedtls_mpi sharedSecret;
  mbedtls_mpi_init(&sharedSecret);

  mbedtls_entropy_context entropyCtx;
  mbedtls_entropy_init(&entropyCtx);
  ASSERT_CODE_PRINT_RETURN_BOOL(
      "Failed to computed shared secret",
      mbedtls_ecdh_compute_shared(&keypair.private_grp, &sharedSecret,
                                  &keypair.private_Q, &keypair.private_d,
                                  &mbedtls_entropy_func, &entropyCtx));

  // TODO: Use better buffer
  uint8_t sharedSecretBuf[32];
  ASSERT_CODE_PRINT_RETURN_BOOL(
      "Failed to write shared secret to buffer",
      mbedtls_mpi_write_binary(&sharedSecret, sharedSecretBuf,
                               sizeof(sharedSecretBuf)));
  uint8_t readerKey[32];
  const uint8_t readerInfo[] = "SKReader";
  const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  ASSERT_CODE_PRINT_RETURN_BOOL(
      "Failed to HKDF reader key",
      mbedtls_hkdf(md, transcript.data(), transcript.size(), sharedSecretBuf,
                   sizeof(sharedSecretBuf), readerInfo, 8, readerKey,
                   sizeof(readerKey)));
  printHex("Reader Key: ", {readerKey, sizeof(readerKey)});
  uint8_t deviceKey[32];
  const uint8_t deviceInfo[] = "SKDevice";
  ASSERT_CODE_PRINT_RETURN_BOOL(
      "Failed to HKDF device key",
      mbedtls_hkdf(md, transcript.data(), transcript.size(), sharedSecretBuf,
                   sizeof(sharedSecretBuf), deviceInfo, 8, deviceKey,
                   sizeof(deviceKey)));
  printHex("Device Key: ", {deviceKey, sizeof(deviceKey)});

  esp_gcm_context gcmCtx;
  mbedtls_gcm_init(&gcmCtx);
  uint8_t iv[] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
  ASSERT_CODE_PRINT_RETURN_BOOL(
      "Failed to set key",
      mbedtls_gcm_setkey(&gcmCtx, MBEDTLS_CIPHER_ID_AES, readerKey, 256));
  uint8_t encrypted[255];
  uint8_t tag[16];
  uint8_t ad;
  ASSERT_CODE_PRINT_RETURN_BOOL(
      "Failed to encrypt",
      mbedtls_gcm_crypt_and_tag(
          &gcmCtx, MBEDTLS_GCM_ENCRYPT, sizeof(unencryptedRequest), iv,
          sizeof(iv), &ad, 0, unencryptedRequest, encrypted, sizeof(tag), tag));

  printHex("Encrypted Output ", {encrypted, sizeof(encrypted)});

  return true;
  // ... PERFORM ECDH OPERATIONS HERE ...

  // cleanup:
  //   ESP_LOGE(TAG, "CLeaning up");
  //   // 7. Free memory when done
  //   mbedtls_ecp_keypair_free(&keypair);
  //   return true;
}

bool setIdent(NimBLECharacteristic *identCharacteristic,
              std::span<const uint8_t> encodedDevicePublicKey) {
  uint8_t hkdfOutput[16];
  const uint8_t info[] = "BLEIdent";
  const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  ASSERT_CODE_PRINT_RETURN_BOOL(
      "Failed to HKDF", mbedtls_hkdf(md, {}, 0, encodedDevicePublicKey.data(),
                                     encodedDevicePublicKey.size(), info, 8,
                                     hkdfOutput, sizeof(hkdfOutput)));
  printHex("Ident: ", {hkdfOutput, sizeof(hkdfOutput)});
  identCharacteristic->setValue(hkdfOutput, sizeof(hkdfOutput));

  return true;
}
} // namespace Crypto