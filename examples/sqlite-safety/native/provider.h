#pragma once

#include <sqlite3.h>

int chtholly_sqlite_version(void);
sqlite3 *chtholly_sqlite_open_memory(void);
int chtholly_sqlite_close(sqlite3 *database);
int chtholly_sqlite_open_invalid(void);
