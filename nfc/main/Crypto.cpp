#include "errors.h"
#include "mbedtls/bignum.h"
#include "mbedtls/ecp.h"
#include "mbedtls/entropy.h"
#include "utils.h"
#include <mbedtls/ecdh.h>
#include <psa/crypto.h>

int errorCode;

namespace Crypto {

bool test() {
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
  ret = mbedtls_ecp_group_load(&keypair.private_grp, MBEDTLS_ECP_DP_SECP256R1);
  if (ret != 0) {
    ESP_LOGE(TAG, "Failed to load group: -0x%04x", -ret);
    return false;
  }

  // 4. Load the Private Key (d)
  // This parses the hex string directly into the MPI (BigNum) structure
  // Equivalent to: PrivateKey.fromBuffer(...)
  ret = mbedtls_mpi_read_string(&keypair.private_d, 16, priv_hex);
  if (ret != 0) {
    ESP_LOGE(TAG, "Failed to parse private key: -0x%04x", -ret);
    return false;
  }
  int errorCode = mbedtls_ecp_point_read_string(
      &keypair.private_Q, 16,
      "65b896331da50332029c967c5802943d2068d5f9dc0886953f9c22f756f7cc59",
      "0ca8e45d3b2812cd1b9d1047598f5e0b29464c270d0bf7302292010c6782a0f6");
  if (errorCode != 0) {
    ESP_LOGE(TAG, "Failed to create %d", errorCode);
    return false;
  }

  // 6. Validate the keys (Optional but good practice)
  ret = mbedtls_ecp_check_privkey(&keypair.private_grp, &keypair.private_d);
  if (ret == 0) {
    ESP_LOGI(TAG, "Private key loaded and validated successfully on secp256r1");
  } else {
    ESP_LOGE(TAG, "Private key is invalid for this curve");
  }

  mbedtls_mpi sharedSecret;
  mbedtls_mpi_init(&sharedSecret);

  mbedtls_entropy_context entropyCtx;
  mbedtls_entropy_init(&entropyCtx);
  errorCode = mbedtls_ecdh_compute_shared(
      &keypair.private_grp, &sharedSecret, &keypair.private_Q,
      &keypair.private_d, &mbedtls_entropy_func, &entropyCtx);
  if (errorCode != 0) {
    ESP_LOGE(TAG, "Failed to compute shared %d\n", errorCode);
    return false;
  }

  mbedtls_mpi_write_file("Shared ", &sharedSecret, 16, NULL);

  return true;
  // ... PERFORM ECDH OPERATIONS HERE ...

  // cleanup:
  //   ESP_LOGE(TAG, "CLeaning up");
  //   // 7. Free memory when done
  //   mbedtls_ecp_keypair_free(&keypair);
  //   return true;
}
} // namespace Crypto