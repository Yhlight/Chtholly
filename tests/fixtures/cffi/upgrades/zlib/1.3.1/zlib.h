#ifndef CH_HIGH_ZLIB_131_H
#define CH_HIGH_ZLIB_131_H
#define ZLIB_VERSION "1.3.1"
#define ZLIB_VERNUM 0x1310
typedef unsigned long uLong;
typedef unsigned long uLongf;
const char *zlibVersion(void);
uLong compressBound(uLong sourceLen);
uLong zlibCompileFlags(void);
#endif
