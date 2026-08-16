#pragma once

#include "CachingProtocol.hpp"
#include "ViewRegistry.hpp"
#include <Expression.hpp>
#include <optional>
#include <unordered_map>

// Contains the information needed to make a decision about whether to admit a view into the cache
struct AdmissionCandidate {
  boss::Symbol name;
  double size;
  double benefit;
};

// Global cache for storing evaluated views. This is a simple in-memory cache that maps view names
// to their evaluated expressions.
extern std::unordered_map<Symbol, Expression> viewCache;
extern double viewCacheSize;      // Total size of the view cache in bytes
extern double viewCacheOccupancy; // Current occupancy of the view cache in bytes

// Compute the size of a view after evaluating it.
// The result must be a table expression, otherwise an exception is thrown.
double computeSize(Expression const &tableExpr);

// Computes the cost of computing the view from scratch with the IsolatedMeasurement strategy
std::optional<double> isoCost(ViewEntry const &entry);
// Computes the cost of computing the view from scratch with the Standard strategy
std::optional<double> stdCost(ViewEntry const &entry,
                              std::unordered_map<boss::Symbol, std::optional<double>> &memo,
                              bool fallbackToIso = false);
// Takes the minimum of the two costs above.
std::optional<double> trueCost(Symbol const &name,
                               std::unordered_map<boss::Symbol, std::optional<double>> &memo,
                               bool fallbackToIso = false);
// Computes the benefit score of admitting a view into the cache.
// Follows the formula benefit = (trueCost - reuseCost) * importance factor / size
std::optional<double> benefit(boss::Symbol const &name,
                              std::unordered_map<boss::Symbol, std::optional<double>> &memo,
                              bool fallbackToIso = false);

// Depending on cost analysis it decides the execution strategy of the view
ExecutionStrategy
selectExecutionStrategy(ViewEntry &entry,
                        std::unordered_map<boss::Symbol, std::optional<double>> &memo);
