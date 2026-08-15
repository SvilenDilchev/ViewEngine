// ViewEngine Caching Protocol
//
// If views are to be cached and cached results reused by the engine pipeline, every engine in the
// pipeline must handle the WithCaches top-level operator, the Pending and Borrowed wrapper
// operators, and the CacheRef operator used to refer to cached views in the expression.
//
// Views can be cached by passing true as the second argument to QueryView. If a single QueryView
// call wants to cache its result, the expression gets wrapped in the WithCaches operators, and
// inversely if no views are to be cached, the expression is left unchanged.

#pragma once

#include <Expression.hpp>
#include <ExpressionUtilities.hpp>
#include <Utilities.hpp>

#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

using boss::ComplexExpression;
using boss::Expression;
using boss::Symbol;

enum class CacheEntryType {
  Borrowed, // Cached value moved out of view registry and moved back in at pipeline end
  Pending,  // View definition to be cached after evaluation of the full pipeline
};

enum class ExecutionStrategy {
  Standard,            // Instrumentation already trustworthy - engine may use
                       // whatever normal/optimized execution it has (fusion,
                       // pipelining, reuse of cached dependencies, etc.)
  IsolatedMeasurement, // Profiling data missing - engines should evaluate expression independently
                       // of any cached dependencies, so we get an accurate measurement of the cost
                       // of this expression alone.
};

// Optional optimisation guideline for engines to follow while connecting their own caching logic to
// this protocol. Designed to help engines identify when it is safe to use their own cached results
// for the incoming BOSS expression, instead of reevaluating it. However, engines can always
// choose to just reevaluate everything as normal. An integrity check is a comparison between the
// incoming expression and the engine's own cached expression for the same view
//
// 2 modes of integrity checks are supported as options for QueryView calls These options can mean
// different things to different engines but are designed with BOSS Table expressions in mind.
//
// Structural - more lightweight but incorrect when there are operators that modify values inline
//              while preserving the overall structure of the BOSS table expression
//              O(num_columns): schema + row count + null counts per column.
//              100% correct for structural operators (Filter, Project,
//              GroupBy, Join, ...)
// Content - hash over the actual content of the expression
//           O(n): xxHash3_128 over all column data.
//           Practically 100% correct for everything.  Recommended for
//           views ending in aggregations where the result is small and hashing is cheap
enum class IntegrityCheckMode {
  Structural,
  Content,
};

struct CacheEntry {
  Expression value;
  CacheEntryType type;
  std::optional<IntegrityCheckMode>
      integrityMode; // guideline for engines, not a strict requirement

  ExecutionStrategy executionStrategy = ExecutionStrategy::Standard;

  // Running cost totals of wall time sprent producing or reusing this entry, accumulated by each
  // engine that touches it during the pipeline pass. Each engine is responsible to accurately
  // separate the time spent computing the entry vs. the time spent materialising it as an
  // intermediate result if it chooses to do so. These costs are used for benefit calculations to
  // decide whether to cache the entry in the view registry or not.
  double computeCost = 0.0;
  double materialiseCost = 0.0;
  double reuseCost = 0.0;
  double marginalComputeCost = 0.0; // Cost of computing on top of its materialised dependencies
};

// Registry mapping view names to cache entries. The registry is not about persistance, but
// per-query tracking, and should be cleared after each query. Entries represent expressions wrapped
// with Pending/Borrowed and referenced by CacheRef within the the WithCaches expression, which has
// the following structure:
// (WithCaches
//   (Borrowed|Pending viewName1 viewDef1 [integrityMode1])
//   (Borrowed|Pending viewName2 viewDef2 [integrityMode2])
//   ...
//   finalExpr)
using CacheRegistry = std::unordered_map<boss::Symbol, CacheEntry>;

// Default registry
// Each engine compiled as a separate .so gets its own independent instance.
inline CacheRegistry defaultCacheRegistry;

// Helper functions to convert symbols to enums for integrity check mode and execution strategy
inline std::optional<IntegrityCheckMode> symbolToIntegrityCheckMode(Symbol const &s) {
  using boss::utilities::operator""_;
  if (s == "Structural"_)
    return IntegrityCheckMode::Structural;
  if (s == "Content"_)
    return IntegrityCheckMode::Content;
  return std::nullopt;
}

inline std::optional<ExecutionStrategy> symbolToExecutionStrategy(Symbol const &s) {
  using boss::utilities::operator""_;
  if (s == "Standard"_)
    return ExecutionStrategy::Standard;
  if (s == "IsolatedMeasurement"_)
    return ExecutionStrategy::IsolatedMeasurement;
  return std::nullopt;
}

// Parses a WithCaches expression, populates registry with Borrowed
// and Pending entries, and returns the actual query (last arg).
inline Expression unpackWithCaches(Expression &&expr, CacheRegistry &registry) {
  using boss::utilities::operator""_;

  auto [head, statics, dynamics, spans] = std::move(std::get<ComplexExpression>(expr)).decompose();

  auto const numArgs = dynamics.size();
  registry.reserve(numArgs - 1);

  for (size_t i = 0; i + 1 < numArgs; ++i) {
    auto [wHead, wStatics, wDynamics, wSpans] =
        std::move(std::get<ComplexExpression>(dynamics[i])).decompose();

    CacheEntryType type = wHead == "Borrowed"_ ? CacheEntryType::Borrowed : CacheEntryType::Pending;

    auto name = std::get<Symbol>(std::move(wDynamics[0]));
    auto value = std::move(wDynamics[1]);
    auto executionStrategy = symbolToExecutionStrategy(std::get<Symbol>(wDynamics[2]))
                                 .value_or(ExecutionStrategy::Standard);
    auto computeCost = std::get<double>(wDynamics[3]);
    auto materialiseCost = std::get<double>(wDynamics[4]);
    auto reuseCost = std::get<double>(wDynamics[5]);
    auto marginalComputeCost = std::get<double>(wDynamics[6]);
    std::optional<IntegrityCheckMode> mode = std::nullopt;
    if (type == CacheEntryType::Borrowed && wDynamics.size() >= 8)
      mode = symbolToIntegrityCheckMode(std::get<Symbol>(wDynamics[7]))
                 .value_or(IntegrityCheckMode::Content);

    registry[std::move(name)] = CacheEntry{
        std::move(value),   type, mode, executionStrategy, computeCost, materialiseCost, reuseCost,
        marginalComputeCost};
  }

  return std::move(dynamics[numArgs - 1]);
}

inline Expression repackWithCaches(CacheRegistry &&registry, Expression &&finalExpr) {
  using boss::utilities::operator""_;

  boss::ExpressionArguments args;
  args.reserve(registry.size() + 1);

  for (auto &&[name, entry] : registry) {
    boss::ExpressionArguments wrapperArgs;
    auto const &mode = entry.integrityMode;
    wrapperArgs.reserve(mode ? 8u : 7u);
    wrapperArgs.emplace_back(name);
    wrapperArgs.emplace_back(std::move(entry.value));
    wrapperArgs.emplace_back(entry.executionStrategy == ExecutionStrategy::IsolatedMeasurement
                                 ? "IsolatedMeasurement"_
                                 : "Standard"_);
    wrapperArgs.emplace_back(entry.computeCost);
    wrapperArgs.emplace_back(entry.materialiseCost);
    wrapperArgs.emplace_back(entry.reuseCost);
    wrapperArgs.emplace_back(entry.marginalComputeCost);
    if (mode) {
      wrapperArgs.emplace_back(*mode == IntegrityCheckMode::Structural ? "Structural"_
                                                                       : "Content"_);
    }

    Symbol const wrapperHead = entry.type == CacheEntryType::Borrowed ? "Borrowed"_ : "Pending"_;
    args.emplace_back(ComplexExpression{wrapperHead, {}, std::move(wrapperArgs), {}});
  }

  args.emplace_back(std::move(finalExpr));
  return ComplexExpression{"WithCaches"_, {}, std::move(args), {}};
}