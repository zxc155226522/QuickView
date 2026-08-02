#pragma once
#include <stdint.h>
typedef int32_t UChar32;
typedef int UErrorCode;
#define U_ZERO_ERROR 0
#define U_FAILURE(status) ((status) != 0)
#define U_SUCCESS(status) ((status) == 0)
#define U8_MAX_LENGTH 4
#define U_IS_UNICODE_CHAR(c) (1)
