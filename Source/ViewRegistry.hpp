#pragma once

#include "CachingProtocol.hpp"
#include "QueryRewriter.hpp"
#include <unordered_map>
#include <unordered_set>

struct ViewEntry {
  Expression definition;
  std::unordered_set<boss::Symbol> dependencies;
  std::unordered_set<boss::Symbol>
      expandedBaseTables; // base tables referenced in the view definition, including those from any
                          // QueryView dependencies
  Signature signature;

  // Cost metrics corresponding to cache entry costs
  double computeCost = 0.0;
  double materialiseCost = 0.0;
  double reuseCost = 0.0;
  double marginalComputeCost = 0.0;

  double size = 0.0; // Size of the materialised view in bytes

  double importanceFactor = 0.0;        // Used to rank views for eviction from cache
  uint64_t importanceLastAgedQuery = 0; // Last query when importance was aged
  uint64_t reuseLastMeasuredQuery = 0;  // Last query when reuseCost was actually measured
  uint64_t lastUsedQuery = 0;           // Last query when the view was hit or (re)built

  CachingDecision cachingDecision = CachingDecision::Defer; // Whether the view should be cached
};

// Full: query rewriter (findRewriting) and caching (viewCache/WithCaches) are active as normal.
// Lite: both are bypassed - QueryView/bare-symbol view references are resolved by directly
// substituting the stored definition (recursively), nothing is cached, and no rewrite candidates
// are searched for. DefineView bookkeeping (dependency graph, cycle detection) is unaffected by
// this mode, since it's registry integrity, not rewriting or caching.
enum class EngineMode { Full, Lite };
extern EngineMode engineMode;

extern std::unordered_map<boss::Symbol, ViewEntry> viewRegistry; // In memory register of Views
extern std::unordered_set<boss::Symbol> evaluationStack;         // Guard against runtime cycles
extern std::unordered_set<boss::Symbol>
    resolvedCacheRefs; // Guard against repeated unevaluated CacheRefs in WithCaches evaluation

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

// Smoothing factor for cost estimates: one sample moves the estimate by at most this fraction,
// so a single measurement outlier (e.g. an allocator stall landing inside a conversion) cannot
// flip a view's benefit sign on its own.
constexpr double COST_EMA_ALPHA = 0.2;
// How long a negative-benefit verdict stands before the view is given one retry (which
// re-measures reuseCost and either heals or re-condemns the estimate).
constexpr uint64_t CONDEMNED_RETRY_QUERIES = 50;

// veTick increments twice per query (VE runs at both ends of the pipeline).
inline uint64_t currentQueryIndex() { return (veTick + 1) / 2; }

// Age the importance factor of a view entry based on how many queries
// have been executed since it was last aged.
void ageEntry(ViewEntry &entry);

// DFS walk to detect cycles in the dependency graph
bool hasCycle(const boss::Symbol &newView, const boss::Symbol &current,
              std::unordered_set<boss::Symbol> &visited);
// Invalidate the cache and stale instrumentation of all views that directly or indirectly
// depend on the given view
void invalidateDependants(const boss::Symbol &invalidatedView,
                          std::unordered_set<boss::Symbol> &seen);

// Recursively compute all the base tables directly and transitively referenced by a view
// definition, including those from any QueryView dependencies
// TODO: consider expanding and caching the full signature of a view at define time
std::unordered_set<boss::Symbol> unionExpanded(const std::unordered_set<boss::Symbol> &baseTables,
                                               const std::unordered_set<boss::Symbol> &dependencies,
                                               std::unordered_set<boss::Symbol> &visited);
std::unordered_set<boss::Symbol> expandBaseTables(const boss::Symbol &viewName,
                                                  std::unordered_set<boss::Symbol> &visited);

// Folds a new cost sample into the stored estimate; a value <= 0.0 means
//  the cost was not measured this pass and leaves the estimate untouched.
void storeCostSample(double &target, const double newValue);
