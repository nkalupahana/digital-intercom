#pragma once

#define CHECK_RETURN_VAL(code, retVal)                                         \
  do {                                                                         \
    if (!(code)) {                                                             \
      return retVal;                                                           \
    }                                                                          \
  } while (0)

#define CHECK_RETURN_BOOL(code) CHECK_RETURN_VAL(code, false)

#define CHECK_PRINT_RETURN_VAL(error_string, code, retVal, ...)                \
  do {                                                                         \
    if (!(code)) {                                                             \
      Serial.printf(error_string __VA_OPT__(, ) __VA_ARGS__);                  \
      return retVal;                                                           \
    }                                                                          \
  } while (0)

#define CHECK_PRINT_RETURN_BOOL(error_string, code, ...)                       \
  CHECK_PRINT_RETURN_VAL(error_string, code, false, __VA_ARGS__)

#define CHECK_PRINT_RETURN(error_string, code, ...)                            \
  CHECK_PRINT_RETURN_VAL(error_string, code, __VA_ARGS__)