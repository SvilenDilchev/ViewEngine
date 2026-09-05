#pragma once

#include "CachingProtocol.hpp"
#include "ViewRegistry.hpp"
#include <Expression.hpp>
#include <optional>
#include <unordered_map>
#include <unordered_set>

// Contains the information needed to make a decision about whether to admit a view into the cache
struct AdmissionCandidate {
  boss::Symbol name;
  double size;
  double benefit;
  uint64_t lastUsed = 0;
};

// Global cache for storing evaluated views. This is a simple in-memory cache that maps view names
// to their evaluated expressions.
extern std::unordered_map<Symbol, Expression> viewCache;
extern double viewCacheSize;      // Total size of the view cache in bytes
extern double viewCacheOccupancy; // Current occupancy of the view cache in bytes

// Ordering policy for cache admission. Lowest ranking views are evicted.
enum class EvictionPolicy { Benefit, LRU, Random };
extern EvictionPolicy evictionPolicy;

// Compute the size of a view after evaluating it.
// The result must be a table expression, otherwise an exception is thrown.
double computeSize(Expression const &tableExpr);

// Computes the cost of computing the view from scratch with the IsolatedMeasurement strategy
std::optional<double> isoCost(ViewEntry const &entry);
// Computes the cost of computing the view from scratch with the Standard strategy
std::optional<double> stdCost(ViewEntry const &entry, bool fallbackToIso = false,
                              std::unordered_set<boss::Symbol> *visited = nullptr);
// Takes the minimum of the two costs above.
std::optional<double> trueCost(Symbol const &name, bool fallbackToIso = false,
                               std::unordered_set<boss::Symbol> *visited = nullptr);
// Computes the benefit score of admitting a view into the cache.
// Follows the formula benefit = (trueCost - reuseCost) * importance factor / size
std::optional<double> benefit(boss::Symbol const &name, bool fallbackToIso = false);

// Whether a view could claim enough space to survive admission, given what is
// currently cached or in flight. True when the information to judge is missing, so the decision can
// be deferred to VE2 where more information is available.
bool couldWinAdmission(boss::Symbol const &name, std::optional<double> cost);
// Drops the cached competitor ranking used by couldWinAdmission
// and thus forces it to be rebuilt on the next call.
void invalidateAdmissionSnapshot();

// Depending on cost analysis it decides the execution strategy of the view
ExecutionStrategy selectExecutionStrategy(ViewEntry &entry);
