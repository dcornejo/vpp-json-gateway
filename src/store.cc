// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "src/store.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace vpp_json {
namespace {
[[noreturn]] void StorageFailure() {
  std::cerr << "Fatal journal error; refusing further operations\n";
  std::exit(1);
}
}  // namespace
Store::Store(const std::string& path) {
  if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
    StorageFailure();
  }
  Query("PRAGMA journal_mode=WAL");
  Query("PRAGMA synchronous=FULL");
  Query("PRAGMA foreign_keys=ON");
  Query(
      "CREATE TABLE IF NOT EXISTS sessions (sid TEXT PRIMARY KEY, client TEXT "
      "NOT NULL, registration TEXT NOT NULL, expires INTEGER NOT NULL, "
      "UNIQUE(client,registration))");
  Query(
      "CREATE TABLE IF NOT EXISTS jobs (sid TEXT NOT NULL REFERENCES "
      "sessions(sid) ON DELETE CASCADE, id TEXT NOT NULL, body TEXT NOT NULL, "
      "state TEXT NOT NULL, path TEXT NOT NULL, offset INTEGER NOT NULL "
      "DEFAULT 0, size INTEGER NOT NULL DEFAULT 0, expires INTEGER NOT NULL, "
      "PRIMARY KEY(sid,id))");
}
Store::~Store() { sqlite3_close(db_); }
Store::Rows Store::Query(const std::string& sql,
                         const std::vector<std::string>& args) {
  sqlite3_stmt* statement = nullptr;
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &statement, nullptr) !=
      SQLITE_OK) {
    StorageFailure();
  }
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (sqlite3_bind_text(statement, i + 1, args[i].data(),
                          static_cast<int>(args[i].size()),
                          SQLITE_TRANSIENT) != SQLITE_OK) {
      StorageFailure();
    }
  }
  Rows rows;
  int rc;
  while ((rc = sqlite3_step(statement)) == SQLITE_ROW) {
    std::vector<std::string> row;
    for (int i = 0; i < sqlite3_column_count(statement); ++i) {
      const auto* text = sqlite3_column_text(statement, i);
      row.emplace_back(text ? reinterpret_cast<const char*>(text) : "",
                       sqlite3_column_bytes(statement, i));
    }
    rows.push_back(std::move(row));
  }
  sqlite3_finalize(statement);
  if (rc != SQLITE_DONE) {
    StorageFailure();
  }
  return rows;
}
int64_t Store::Number(const std::string& sql,
                      const std::vector<std::string>& args) {
  const auto rows = Query(sql, args);
  return rows.empty() ? 0 : std::strtoll(rows[0][0].c_str(), nullptr, 10);
}
}  // namespace vpp_json
