#include "fixtures/cffi/fixture.h"

struct sqlite3;
extern int sqlite3_open(const char *, struct sqlite3 **);
extern int sqlite3_close(struct sqlite3 *);

typedef unsigned long c_zlib_ulongf;
extern int uncompress(unsigned char *, c_zlib_ulongf *, const unsigned char *,
                      unsigned long);

extern int curl_easy_perform(void *);

int c_sqlite_open_invalid(void) {
  struct sqlite3 *database = NULL;
  const int status = sqlite3_open(
      "/chtholly/path/that/does/not/exist/database.sqlite", &database);
  if (database)
    sqlite3_close(database);
  return status;
}

int c_zlib_corrupt(void) {
  unsigned char output[32] = {};
  c_zlib_ulongf output_size = sizeof(output);
  static const unsigned char corrupt[] = {0x78, 0x9c, 0x00, 0x01, 0x02, 0x03};
  return uncompress(output, &output_size, corrupt, sizeof(corrupt));
}

int c_curl_perform_invalid(void) { return curl_easy_perform(NULL); }
