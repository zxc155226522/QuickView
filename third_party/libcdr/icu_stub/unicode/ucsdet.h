#pragma once
#include "utypes.h"
struct UCharsetDetector {};
struct UCharsetMatch {};
inline UCharsetDetector* ucsdet_open(UErrorCode* status) { if(status)*status=0; return nullptr; }
inline void ucsdet_enableInputFilter(UCharsetDetector*, bool) {}
inline void ucsdet_setText(UCharsetDetector*, const char*, unsigned int, UErrorCode* status) { if(status)*status=0; }
inline const UCharsetMatch* ucsdet_detect(UCharsetDetector*, UErrorCode* status) { if(status)*status=0; return nullptr; }
inline const char* ucsdet_getName(const UCharsetMatch*, UErrorCode* status) { if(status)*status=0; return ""; }
inline int32_t ucsdet_getConfidence(const UCharsetMatch*, UErrorCode* status) { if(status)*status=0; return 0; }
inline void ucsdet_close(UCharsetDetector*) {}
