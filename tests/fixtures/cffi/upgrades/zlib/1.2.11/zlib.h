#ifndef CH_HIGH_ZLIB_1211_H
#define CH_HIGH_ZLIB_1211_H
#define ZLIB_VERSION "1.2.11"
#define ZLIB_VERNUM 0x12b0
typedef unsigned long uLong;
typedef unsigned long uLongf;
const char *zlibVersion(void);
uLong compressBound(uLong sourceLen);
#endif
