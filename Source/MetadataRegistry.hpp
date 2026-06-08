#pragma once

#include <BOSS.hpp>
#include <Expression.hpp>
#include <string>
#include <unordered_map>
#include <vector>

// Represents the metadata for a table used as a source
// Used for lazy loading of table data
struct TableEntry {
  std::string url;
  std::string loaderPath;
  std::vector<boss::Symbol> columns;
  bool lazy = false; // Whether a the table is lazily loaded by Wisent
                     // and references should be replaced by Gather expressions
};

// table name -> metadata about the table source
extern std::unordered_map<boss::Symbol, TableEntry> tableRegistry;
// column name -> set of tables that own that column
extern std::unordered_map<boss::Symbol, std::vector<boss::Symbol>> columnRegistry;

bool registerTable(const boss::Symbol &name, const std::string &url, const std::string &loaderPath,
                   bool lazy, const std::vector<boss::Symbol> &columns);

// Remove a table and its columns from their registries
bool dropTable(const boss::Symbol &name);

// Clears both table and column registries
void clearTables();

// Returns a list of all registered tables and their metadata
boss::Expression listTables();