#include "provider.h"

int chtholly_sqlite_version(void) { return sqlite3_libversion_number(); }

sqlite3 *chtholly_sqlite_open_memory(void) {
  sqlite3 *database = 0;
  if (sqlite3_open(":memory:", &database) != SQLITE_OK) {
    if (database)
      sqlite3_close(database);
    return 0;
  }
  return database;
}

int chtholly_sqlite_close(sqlite3 *database) {
  return sqlite3_close(database);
}

int chtholly_sqlite_open_invalid(void) {
  sqlite3 *database = 0;
  const int status = sqlite3_open(
      "/chtholly/path/that/does/not/exist/database.sqlite", &database);
  if (database)
    sqlite3_close(database);
  return status;
}
