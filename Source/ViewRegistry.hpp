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

  double computeCost = 0.0;
  double materialiseCost = 0.0;
  double reuseCost = 0.0;

  double size = 0.0; // Size of the materialised view in bytes

  double importanceFactor = 0.0;       // Used to rank views for eviction from cache
  uint64_t importanceLastAgedTick = 0; // Last tick (global query count) when importance was aged
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

// Global call count of the view engine. Incremented on each top-level evaluate() call.
// This means it's incremented twice per processed query at the start and end of a pipeline
// that has the view engine at both ends.
extern uint64_t veTick;
constexpr double IMPORTANCE_DECAY_FACTOR = 0.98;

// Age the importance factor of a view entry based on how many queries
// have been executed since it was last aged.
void ageEntry(ViewEntry &entry);

// DFS walk to detect cycles in the dependency graph
bool hasCycle(const boss::Symbol &newView, const boss::Symbol &current,
              std::unordered_set<boss::Symbol> &visited);
// Invalidate caches of all views that directly or indirectly depend on the given view
void invalidateDependentCaches(const boss::Symbol &invalidatedView,
                               std::unordered_set<boss::Symbol> &seen);

// Compute the size of a view after evaluating it.
// The result must be a table expression, otherwise an exception is thrown.
double computeSize(Expression const &tableExpr);