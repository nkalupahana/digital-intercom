#pragma once
#include <cbor.h>
#include <esp_log.h>
#include <mbedtls/error.h>

#define CHECK_RETURN_VAL(code, retVal)                                         \
  do {                                                                         \
    if (!(code)) {                                                             \
      return retVal;                                                           \
    }                                                                          \
  } while (0)

#define CHECK_RETURN(code) CHECK_RETURN_VAL(code, )

#define CHECK_RETURN_BOOL(code) CHECK_RETURN_VAL(code, false)

#define CHECK_RETURN_OPT(code) CHECK_RETURN_VAL(code, std::nullopt)

#define CHECK_PRINT_RETURN_VAL(error_string, code, retVal, ...)                \
  do {                                                                         \
    if (!(code)) {                                                             \
      ESP_LOGE(TAG, error_string __VA_OPT__(, ) __VA_ARGS__);                  \
      return retVal;                                                           \
    }                                                                          \
  } while (0)

#define CHECK_PRINT_RETURN_BOOL(error_string, code, ...)                       \
  CHECK_PRINT_RETURN_VAL(error_string, code, false, __VA_ARGS__)

#define CHECK_PRINT_RETURN_OPT(error_string, code, ...)                        \
  CHECK_PRINT_RETURN_VAL(error_string, code, std::nullopt, __VA_ARGS__)

#define CHECK_PRINT_RETURN(error_string, code, ...)                            \
  CHECK_PRINT_RETURN_VAL(error_string, code, , __VA_ARGS__)

#define CHECK_CRYPTO_RETURN_VAL(error_string, code, val, ...)                  \
  do {                                                                         \
    int error = code;                                                          \
    if (error != 0) {                                                          \
      const char *highLevel = mbedtls_high_level_strerr(error);                \
      const char *lowLevel = mbedtls_high_level_strerr(error);                 \
      ESP_LOGE(TAG,                                                            \
               error_string " - Error: %s : %s" __VA_OPT__(, ) __VA_ARGS__,    \
               highLevel ? highLevel : "<MISSING>",                            \
               lowLevel ? lowLevel : "<MISSING>");                             \
      return val;                                                              \
    }                                                                          \
  } while (0)

#define CHECK_CRYPTO_RETURN_BOOL(error_string, code, ...)                      \
  CHECK_CRYPTO_RETURN_VAL(error_string, code, false, __VA_ARGS__)

#define CHECK_CRYPTO_RETURN_OPT(error_string, code, ...)                       \
  CHECK_CRYPTO_RETURN_VAL(error_string, code, std::nullopt, __VA_ARGS__)

#define CHECK_CBOR_RETURN_VAL(error_string, code, val, ...)                    \
  do {                                                                         \
    CborError error = code;                                                    \
    if (error != CborNoError) {                                                \
      ESP_LOGE(TAG, error_string " - Error: %s" __VA_OPT__(, ) __VA_ARGS__,    \
               cbor_error_string(error));                                      \
      return val;                                                              \
    }                                                                          \
  } while (0)

#define CHECK_CBOR_RETURN_BOOL(error_string, code, ...)                        \
  CHECK_CBOR_RETURN_VAL(error_string, code, false, __VA_ARGS__)

#define CHECK_CBOR_RETURN_OPT(error_string, code, ...)                         \
  CHECK_CBOR_RETURN_VAL(error_string, code, std::nullopt, __VA_ARGS__)
