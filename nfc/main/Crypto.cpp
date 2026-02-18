#include "errors.h"
#include "mbedtls/bignum.h"
#include "utils.h"
#include <mbedtls/bignum.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/hkdf.h>
#include <psa/crypto.h>

int errorCode;

namespace Crypto {

bool test(std::span<const uint8_t> deviceXY) {
  CHECK_PRINT_RETURN_BOOL("Unable to init crypto",
                          psa_crypto_init() == PSA_SUCCESS);

  // 1. Context definitions
  mbedtls_ecp_keypair keypair;
  int ret;

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
  uint8_t hkdfOutput[32];
  const uint8_t salt[] = {
      216, 24,  89,  3,   25,  131, 216, 24,  120, 192, 65,  52,  48,  48,  54,
      51,  51,  49,  50,  69,  51,  49,  48,  49,  56,  50,  48,  49,  68,  56,
      49,  56,  53,  56,  52,  66,  65,  52,  48,  49,  48,  50,  50,  48,  48,
      49,  50,  49,  53,  56,  50,  48,  54,  53,  66,  56,  57,  54,  51,  51,
      49,  68,  65,  53,  48,  51,  51,  50,  48,  50,  57,  67,  57,  54,  55,
      67,  53,  56,  48,  50,  57,  52,  51,  68,  50,  48,  54,  56,  68,  53,
      70,  57,  68,  67,  48,  56,  56,  54,  57,  53,  51,  70,  57,  67,  50,
      50,  70,  55,  53,  54,  70,  55,  67,  67,  53,  57,  50,  50,  53,  56,
      50,  48,  48,  67,  65,  56,  69,  52,  53,  68,  51,  66,  50,  56,  49,
      50,  67,  68,  49,  66,  57,  68,  49,  48,  52,  55,  53,  57,  56,  70,
      53,  69,  48,  66,  50,  57,  52,  54,  52,  67,  50,  55,  48,  68,  48,
      66,  70,  55,  51,  48,  50,  50,  57,  50,  48,  49,  48,  67,  54,  55,
      56,  50,  65,  48,  70,  54,  48,  53,  56,  48,  48,  54,  65,  50,  48,
      51,  70,  53,  48,  52,  70,  53,  216, 24,  88,  75,  164, 1,   2,   32,
      1,   33,  88,  32,  96,  227, 57,  35,  133, 4,   31,  81,  64,  48,  81,
      242, 65,  85,  49,  203, 86,  221, 63,  153, 156, 113, 104, 112, 19,  170,
      198, 118, 139, 200, 24,  126, 34,  88,  32,  229, 141, 235, 143, 219, 233,
      7,   247, 221, 83,  104, 36,  85,  81,  163, 71,  150, 247, 210, 33,  92,
      68,  12,  51,  155, 176, 247, 182, 123, 236, 205, 250, 130, 121, 1,   132,
      57,  49,  48,  50,  48,  70,  52,  56,  55,  51,  49,  53,  68,  49,  48,
      50,  48,  57,  54,  49,  54,  51,  48,  49,  48,  49,  51,  48,  48,  49,
      48,  52,  54,  68,  54,  52,  54,  70,  54,  51,  49,  65,  50,  48,  48,
      51,  48,  49,  54,  49,  55,  48,  55,  48,  54,  67,  54,  57,  54,  51,
      54,  49,  55,  52,  54,  57,  54,  70,  54,  69,  50,  70,  55,  54,  54,
      69,  54,  52,  50,  69,  54,  50,  54,  67,  55,  53,  54,  53,  55,  52,
      54,  70,  54,  70,  55,  52,  54,  56,  50,  69,  54,  67,  54,  53,  50,
      69,  54,  70,  54,  70,  54,  50,  51,  48,  48,  50,  49,  67,  48,  49,
      53,  67,  49,  69,  54,  48,  48,  52,  54,  57,  55,  51,  54,  70,  50,
      69,  54,  70,  55,  50,  54,  55,  51,  65,  51,  49,  51,  56,  51,  48,
      51,  49,  51,  51,  51,  65,  54,  52,  54,  53,  55,  54,  54,  57,  54,
      51,  54,  53,  54,  53,  54,  69,  54,  55,  54,  49,  54,  55,  54,  53,
      54,  68,  54,  53,  54,  69,  55,  52,  54,  68,  54,  52,  54,  70,  54,
      51,  65,  52,  48,  48,  54,  51,  51,  49,  50,  69,  51,  49,  48,  49,
      56,  50,  48,  49,  68,  56,  49,  56,  53,  56,  52,  66,  65,  52,  48,
      49,  48,  50,  50,  48,  48,  49,  50,  49,  53,  56,  50,  48,  54,  53,
      66,  56,  57,  54,  51,  51,  49,  68,  65,  53,  48,  51,  51,  50,  48,
      50,  57,  67,  57,  54,  55,  67,  53,  56,  48,  50,  57,  52,  51,  68,
      50,  48,  54,  56,  68,  53,  70,  57,  68,  67,  48,  56,  56,  54,  57,
      53,  51,  70,  57,  67,  50,  50,  70,  55,  53,  54,  70,  55,  67,  67,
      53,  57,  50,  50,  53,  56,  50,  48,  48,  67,  65,  56,  69,  52,  53,
      68,  51,  66,  50,  56,  49,  50,  67,  68,  49,  66,  57,  68,  49,  48,
      52,  55,  53,  57,  56,  70,  53,  69,  48,  66,  50,  57,  52,  54,  52,
      67,  50,  55,  48,  68,  48,  66,  70,  55,  51,  48,  50,  50,  57,  50,
      48,  49,  48,  67,  54,  55,  56,  50,  65,  48,  70,  54,  48,  53,  56,
      48,  48,  54,  65,  50,  48,  51,  70,  53,  48,  52,  70,  53,  88,  123,
      145, 2,   10,  72,  114, 21,  209, 2,   4,   97,  99,  1,   1,   48,  0,
      28,  30,  6,   10,  105, 115, 111, 46,  111, 114, 103, 58,  49,  56,  48,
      49,  51,  58,  114, 101, 97,  100, 101, 114, 101, 110, 103, 97,  103, 101,
      109, 101, 110, 116, 109, 100, 111, 99,  114, 101, 97,  100, 101, 114, 161,
      0,   99,  49,  46,  48,  90,  32,  21,  1,   97,  112, 112, 108, 105, 99,
      97,  116, 105, 111, 110, 47,  118, 110, 100, 46,  98,  108, 117, 101, 116,
      111, 111, 116, 104, 46,  108, 101, 46,  111, 111, 98,  48,  2,   28,  0,
      17,  7,   164, 178, 49,  210, 148, 105, 11,  169, 249, 79,  60,  9,   64,
      96,  24,  130};
  const uint8_t info[] = "SKReader";
  const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  ASSERT_CODE_PRINT_RETURN_BOOL(
      "Failed to HKDF", mbedtls_hkdf(md, salt, sizeof(salt), sharedSecretBuf,
                                     sizeof(sharedSecretBuf), info, 8,
                                     hkdfOutput, sizeof(hkdfOutput)));

  mbedtls_mpi_write_file("Shared ", &sharedSecret, 16, NULL);
  printHex("HKDF Output ", {hkdfOutput, sizeof(hkdfOutput)});

  return true;
  // ... PERFORM ECDH OPERATIONS HERE ...

  // cleanup:
  //   ESP_LOGE(TAG, "CLeaning up");
  //   // 7. Free memory when done
  //   mbedtls_ecp_keypair_free(&keypair);
  //   return true;
}
} // namespace Crypto