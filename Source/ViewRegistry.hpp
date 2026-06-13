#pragma once

#include "QueryRewriter.hpp"
#include <optional>
#include <unordered_map>
#include <unordered_set>

struct ViewEntry {
  std::optional<Expression> cached;
  Expression definition;
  std::unordered_set<boss::Symbol> dependencies;
  Signature signature;
};

extern std::unordered_map<boss::Symbol, ViewEntry> viewRegistry; // In memory register of Views
extern std::unordered_set<boss::Symbol> evaluationStack;         // Guard against runtime cycles

// Indexes for efficient candidate view lookup during rewriting
// Table name -> set of view names that directly touch that table
// Currently not used in query rewriter, but not completely deprecated as a start for future
// optimisations based on minicon or GQR
extern std::unordered_map<boss::Symbol, std::unordered_set<boss::Symbol>> tableToViews;
// View name -> set of view names that directly depend on it (reverse dependency map)
extern std::unordered_map<boss::Symbol, std::unordered_set<boss::Symbol>> viewToViews;

// DFS walk to detect cycles in the dependency graph
bool hasCycle(const boss::Symbol &newView, const boss::Symbol &current,
              std::unordered_set<boss::Symbol> &visited);
// Invalidate caches of all views that directly or indirectly depend on the given view
void invalidateDependentCaches(const boss::Symbol &invalidatedView,
                               std::unordered_set<boss::Symbol> &seen);