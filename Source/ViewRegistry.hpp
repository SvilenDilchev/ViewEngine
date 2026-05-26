#pragma once

#include "QueryRewriter.hpp"
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

struct ViewEntry {
  std::optional<Expression> cached;
  Expression definition;
  std::unordered_set<std::string> dependencies;
  Signature signature;
};

extern std::unordered_map<std::string, ViewEntry> viewRegistry; // In memory register of Views
extern std::unordered_set<std::string> evaluationStack;         // Guard against runtime cycles

// Indexes for efficient candidate view lookup during rewriting
// Table name -> set of view names that directly touch that table
extern std::unordered_map<std::string, std::unordered_set<std::string>> tableToViews;
// View name -> set of view names that directly depend on it (reverse dependency map)
extern std::unordered_map<std::string, std::unordered_set<std::string>> viewToViews;

// DFS walk to detect cycles in the dependency graph
bool hasCycle(const std::string &newView, const std::string &current,
              std::unordered_set<std::string> &visited);
// Invalidate caches of all views that directly or indirectly depend on the given view
void invalidateDependentCaches(const std::string &invalidatedView,
                               std::unordered_set<std::string> &seen);
// DFS walk on indexes to find all views that use the same tables as the incoming query
void findCandidateViews(const std::unordered_set<std::string> &tables,
                        std::unordered_set<std::string> &candidates);