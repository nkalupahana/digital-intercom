#pragma once
#include <esp_log.h>

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

#define ASSERT_CODE_PRINT_RETURN_VAL(error_string, code, val, ...)             \
  do {                                                                         \
    int errorCode = code;                                                      \
    if (code != 0) {                                                           \
      ESP_LOGE(TAG, error_string " - Code: %x" __VA_OPT__(, ) __VA_ARGS__,     \
               -errorCode);                                                    \
      return val;                                                              \
    }                                                                          \
  } while (0)

#define ASSERT_CODE_PRINT_RETURN_BOOL(error_string, code, ...)                 \
  ASSERT_CODE_PRINT_RETURN_VAL(error_string, code, false, __VA_ARGS__)

#define ASSERT_CODE_PRINT_RETURN_OPT(error_string, code, ...)                  \
  ASSERT_CODE_PRINT_RETURN_VAL(error_string, code, std::nullopt, __VA_ARGS__)

#define CHECK_PRINT_RETURN_BOOL(error_string, code, ...)                       \
  CHECK_PRINT_RETURN_VAL(error_string, code, false, __VA_ARGS__)

#define CHECK_PRINT_RETURN_OPT(error_string, code, ...)                        \
  CHECK_PRINT_RETURN_VAL(error_string, code, std::nullopt, __VA_ARGS__)

#define CHECK_PRINT_RETURN(error_string, code, ...)                            \
  CHECK_PRINT_RETURN_VAL(error_string, code, , __VA_ARGS__)
