#pragma once
#include "utypes.h"
struct UConverter {};
inline UConverter* ucnv_open(const char*, UErrorCode* status) { if(status)*status=0; return nullptr; }
inline void ucnv_close(UConverter*) {}
inline UChar32 ucnv_getNextUChar(UConverter*, const char**, const char*, UErrorCode* status) { if(status)*status=0; return 0; }
