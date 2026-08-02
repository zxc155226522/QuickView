#pragma once
#include "utypes.h"
#define U8_APPEND_UNSAFE(s, i, c) { \
    if (c <= 0x7F) { (s)[(i)++] = (char)c; } \
    else if (c <= 0x7FF) { (s)[(i)++] = (char)(0xC0 | (c >> 6)); (s)[(i)++] = (char)(0x80 | (c & 0x3F)); } \
    else if (c <= 0xFFFF) { (s)[(i)++] = (char)(0xE0 | (c >> 12)); (s)[(i)++] = (char)(0x80 | ((c >> 6) & 0x3F)); (s)[(i)++] = (char)(0x80 | (c & 0x3F)); } \
    else { (s)[(i)++] = (char)(0xF0 | (c >> 18)); (s)[(i)++] = (char)(0x80 | ((c >> 12) & 0x3F)); (s)[(i)++] = (char)(0x80 | ((c >> 6) & 0x3F)); (s)[(i)++] = (char)(0x80 | (c & 0x3F)); } }
