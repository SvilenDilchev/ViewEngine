#include "MetadataRegistry.hpp"
#include <ExpressionUtilities.hpp>
#include <algorithm>

using boss::utilities::operator""_;

std::unordered_map<boss::Symbol, TableEntry> tableRegistry;
std::unordered_map<boss::Symbol, std::vector<boss::Symbol>> columnRegistry;

bool registerTable(const boss::Symbol &name, const std::string &url, const std::string &loaderPath,
                   bool lazy, const std::vector<boss::Symbol> &columns) {
  if (tableRegistry.count(name))
    return false; // Table already exists

  if (columnRegistry.count(name))
    return false; // Column with the same name as the table already exists
                  // preventing ambiguity in references in queries

  for (auto const &col : columns) {
    if (tableRegistry.count(col))
      return false; // Column name conflicts with an existing table name
  }

  tableRegistry[name] = TableEntry{url, loaderPath, columns, lazy};
  for (const auto &col : columns)
    columnRegistry[col].push_back(name);
  return true;
}

bool dropTable(const boss::Symbol &name) {
  auto it = tableRegistry.find(name);
  if (it == tableRegistry.end())
    return false; // Table does not exist

  for (const auto &col : it->second.columns) {
    auto &tables = columnRegistry[col];
    tables.erase(std::remove(tables.begin(), tables.end(), name), tables.end());
    if (tables.empty())
      columnRegistry.erase(col); // Remove column entry if no tables have it
  }
  tableRegistry.erase(it);
  return true;
}

void clearTables() {
  tableRegistry.clear();
  columnRegistry.clear();
}

boss::Expression listTables() {
  std::vector<std::pair<boss::Symbol, const TableEntry *>> sortedTables;
  sortedTables.reserve(tableRegistry.size());
  for (const auto &[name, entry] : tableRegistry)
    sortedTables.emplace_back(name, &entry);
  std::sort(sortedTables.begin(), sortedTables.end(),
            [](const auto &a, const auto &b) { return a.first.getName() < b.first.getName(); });

  boss::ExpressionArguments nameArgs;
  boss::ExpressionArguments urlArgs;
  boss::ExpressionArguments loaderPathArgs;
  boss::ExpressionArguments columnsArgs;

  for (const auto &[name, entry] : sortedTables) {
    nameArgs.emplace_back(name);
    urlArgs.emplace_back(entry->url);
    loaderPathArgs.emplace_back(entry->loaderPath);
    boss::ExpressionArguments colArgs;
    for (auto const &col : entry->columns)
      colArgs.emplace_back(col);
    columnsArgs.emplace_back(boss::ComplexExpression("List"_, {}, std::move(colArgs), {}));
  }

  boss::ExpressionArguments tableListArgs;
  tableListArgs.emplace_back(boss::ComplexExpression("Name"_, {}, std::move(nameArgs), {}));
  tableListArgs.emplace_back(boss::ComplexExpression("URL"_, {}, std::move(urlArgs), {}));
  tableListArgs.emplace_back(
      boss::ComplexExpression("LoaderPath"_, {}, std::move(loaderPathArgs), {}));
  tableListArgs.emplace_back(boss::ComplexExpression("Columns"_, {}, std::move(columnsArgs), {}));

  return boss::Expression(boss::ComplexExpression("TableList"_, {}, std::move(tableListArgs), {}));
}