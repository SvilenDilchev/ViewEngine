#pragma once

#include <Expression.hpp>
#include <ExpressionUtilities.hpp>
#include <Utilities.hpp>

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

using boss::ComplexExpression;
using boss::Expression;
using boss::Symbol;
using boss::utilities::operator""_;
using boss::expressions::CloneReason;

// An ordered list of (view name, expression) pairs representing view results
// that need to be cached once the full pipeline has evaluated them.
//
// Ordering is topological by construction: inner/deeper views are registered
// before outer views that depend on them.
// This guarantees that when processing entries in order 0..N, any CacheRef
// referenced by entry[i] will already be resolved by entry[j < i].
using PendingCacheRegistry = std::vector<std::pair<std::string, Expression>>;

// A mapping from view name to the index of its pending cache in the registry.
using PendingCacheLookup = std::unordered_map<std::string_view, size_t>;

// Default registry and lookup
// Each engine compiled as a separate .so gets its own independent instances.
inline PendingCacheRegistry defaultPendingCacheRegistry;
inline PendingCacheLookup defaultPendingCacheLookup;

// Helper function for handling the WithPendingCaches operator
// - populates the provided registry and lookup with the side-channel expressions to be cached
// - returns only the final expression to be evaluated by the engine
inline Expression unpackWithPendingCaches(Expression &&expr, PendingCacheRegistry &registry,
                                          PendingCacheLookup &lookup) {
  auto [head, statics, dynamics, spans] = std::move(std::get<ComplexExpression>(expr)).decompose();
  // Dynamics layout: Name0, expr0, Name1, expr1, ..., NameN, exprN, finalExpr
  // Total args must be odd: 2N side-channel pairs + 1 final
  auto const numArgs = dynamics.size();
  registry.reserve(numArgs / 2);
  lookup.reserve(numArgs / 2);
  for (size_t i = 0; i + 1 < numArgs - 1; i += 2) {
    auto name = std::get<Symbol>(dynamics[i]).getName();
    registry.emplace_back(std::move(name), std::move(dynamics[i + 1]));
    lookup.emplace(registry.back().first, registry.size() - 1);
  }
  return std::move(dynamics[numArgs - 1]);
}

// Helper function for handling the CacheRef operator
inline std::optional<Expression> resolveCacheRef(ComplexExpression const &expr,
                                                 PendingCacheLookup const &lookup,
                                                 PendingCacheRegistry const &registry) {
  // TODO: cloning is currently unavoidable — the registry must retain ownership
  // so that ViewEngine2 can persist all side-channel results at the end of the pipeline.
  // Worth revisiting whether entries that are guaranteed to not be needed again
  // downstream could instead be moved out of the registry directly.
  auto const &name = std::get<Symbol>(expr.getDynamicArguments()[0]).getName();
  auto it = lookup.find(name);
  if (it == lookup.end())
    return std::nullopt;

  return registry[it->second].second.clone(CloneReason::EVALUATE_CONST_EXPRESSION);
}

// Helper function for re-packing the final expression with the to be cached expressions
inline Expression repackWithPendingCaches(PendingCacheRegistry &&registry, Expression &&finalExpr) {
  boss::ExpressionArguments args;
  args.reserve(registry.size() * 2 + 1);
  std::for_each(std::make_move_iterator(registry.begin()), std::make_move_iterator(registry.end()),
                [&args](auto &&pair) {
                  args.emplace_back(Symbol(std::move(pair.first)));
                  args.emplace_back(std::move(pair.second));
                });
  args.emplace_back(std::move(finalExpr));
  return ComplexExpression{"WithPendingCaches"_, {}, std::move(args), {}};
}