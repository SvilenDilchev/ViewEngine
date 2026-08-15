#pragma once
#include "CachingProtocol.hpp"
#include "ViewRegistry.hpp"
#include <optional>

struct AdaptiveCachingDecision {
  bool shouldCache;
  ExecutionStrategy forcedStrategy;
};

// Computes the cost of computing the view from scratch with the IsolatedMeasurement strategy
std::optional<double> isoCost(ViewEntry const &entry);
// Computes the cost of computing the view from scratch with the Standard strategy
std::optional<double> stdCost(ViewEntry const &entry, bool fallbackToIso = false);
// Takes the minimum of the two costs above.
std::optional<double> trueCost(boss::Symbol const &name, bool fallbackToIso = false);
// Computes the benefit score of admitting a view into the cache.
// Follows the formula benefit = (trueCost - reuseCost) * importance factor / size
std::optional<double> benefit(boss::Symbol const &name, bool fallbackToIso = false);

// Depending on cost analysis it decides the execution strategy of the view and can also make a
// cache admission decision if it has enough information.
AdaptiveCachingDecision resolveAdaptiveCaching(ViewEntry &entry);