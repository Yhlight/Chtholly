#include "RegistryDatabaseInternal.h"

#include <limits>

namespace chtholly::registry_internal {

Statement::Statement(sqlite3 *database, const char *sql, std::string &error)
    : database_(database) {
  if (sqlite3_prepare_v2(database, sql, -1, &statement_, nullptr) != SQLITE_OK)
    error = "failed to prepare registry database statement: " +
            std::string(sqlite3_errmsg(database));
}

Statement::~Statement() { sqlite3_finalize(statement_); }

Statement::operator bool() const { return statement_ != nullptr; }

bool Statement::bind(int index, std::string_view value, std::string &error) {
  if (sqlite3_bind_text(statement_, index, value.data(),
                        static_cast<int>(value.size()), SQLITE_TRANSIENT) !=
      SQLITE_OK) {
    error = "failed to bind registry database text: " +
            std::string(sqlite3_errmsg(database_));
    return false;
  }
  return true;
}

bool Statement::bind(int index, std::uint64_t value, std::string &error) {
  if (value > static_cast<std::uint64_t>(
                  (std::numeric_limits<sqlite3_int64>::max)()) ||
      sqlite3_bind_int64(statement_, index,
                         static_cast<sqlite3_int64>(value)) != SQLITE_OK) {
    error = "failed to bind registry database integer";
    return false;
  }
  return true;
}

bool Statement::bind(int index, std::int64_t value, std::string &error) {
  if (sqlite3_bind_int64(statement_, index,
                         static_cast<sqlite3_int64>(value)) != SQLITE_OK) {
    error = "failed to bind registry database integer";
    return false;
  }
  return true;
}

bool Statement::bind(int index, bool value, std::string &error) {
  if (sqlite3_bind_int(statement_, index, value ? 1 : 0) != SQLITE_OK) {
    error = "failed to bind registry database boolean";
    return false;
  }
  return true;
}

int Statement::step() { return sqlite3_step(statement_); }

std::string Statement::text(int column) const {
  const auto *value = sqlite3_column_text(statement_, column);
  const auto size = sqlite3_column_bytes(statement_, column);
  return value == nullptr
             ? std::string{}
             : std::string(reinterpret_cast<const char *>(value),
                           static_cast<std::size_t>(size));
}

std::uint64_t Statement::integer(int column) const {
  return static_cast<std::uint64_t>(sqlite3_column_int64(statement_, column));
}

bool Statement::boolean(int column) const {
  return sqlite3_column_int(statement_, column) != 0;
}

bool execute(sqlite3 *database, const char *sql, std::string &error) {
  char *message = nullptr;
  const auto result = sqlite3_exec(database, sql, nullptr, nullptr, &message);
  if (result == SQLITE_OK)
    return true;
  error = "registry database operation failed: " +
          std::string(message == nullptr ? sqlite3_errmsg(database) : message);
  sqlite3_free(message);
  return false;
}

Transaction::Transaction(sqlite3 *database, std::string &error)
    : database_(database) {
  active_ = execute(database_, "BEGIN IMMEDIATE", error);
}

Transaction::~Transaction() {
  if (active_) {
    std::string ignored;
    execute(database_, "ROLLBACK", ignored);
  }
}

Transaction::operator bool() const { return active_; }

bool Transaction::commit(std::string &error) {
  if (!active_ || !execute(database_, "COMMIT", error))
    return false;
  active_ = false;
  return true;
}

} // namespace chtholly::registry_internal
