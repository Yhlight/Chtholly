#pragma once

#include <sqlite3.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace chtholly::registry_internal {

class Statement {
public:
  Statement(sqlite3 *database, const char *sql, std::string &error);
  ~Statement();
  Statement(const Statement &) = delete;
  Statement &operator=(const Statement &) = delete;

  explicit operator bool() const;
  bool bind(int index, std::string_view value, std::string &error);
  bool bind(int index, std::uint64_t value, std::string &error);
  bool bind(int index, std::int64_t value, std::string &error);
  bool bind(int index, bool value, std::string &error);
  int step();
  std::string text(int column) const;
  std::uint64_t integer(int column) const;
  bool boolean(int column) const;

private:
  sqlite3 *database_ = nullptr;
  sqlite3_stmt *statement_ = nullptr;
};

bool execute(sqlite3 *database, const char *sql, std::string &error);

class Transaction {
public:
  Transaction(sqlite3 *database, std::string &error);
  ~Transaction();

  explicit operator bool() const;
  bool commit(std::string &error);

private:
  sqlite3 *database_ = nullptr;
  bool active_ = false;
};

} // namespace chtholly::registry_internal
