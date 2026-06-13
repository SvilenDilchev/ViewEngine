#pragma once

#include <BOSS.hpp>
#include <Expression.hpp>
#include <ExpressionUtilities.hpp>
#include <Utilities.hpp>

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using boss::utilities::operator""_;
using boss::ComplexExpression;
using boss::Expression;
using boss::Symbol;

constexpr double W_TABLE = 0.40;
constexpr double W_JOIN = 0.30;
constexpr double W_PRED = 0.20;
constexpr double W_PROJ = 0.10;

enum class JoinType { INNER, LEFT, ANTI };

// Data structure to represent join relationships between sources in a query, used for rewriting
struct JoinEdge {
  std::unordered_set<boss::Symbol> leftSources;
  std::unordered_set<boss::Symbol> rightSources;
  std::shared_ptr<Expression> leftKeys;
  std::shared_ptr<Expression> rightKeys;
  JoinType joinType; // "Join", "LeftJoin", "AntiJoin"
};

struct Signature {
  // Map of base table name to the predicates that apply to it,
  // which are stored as a serialised string key to the actual expression
  std::unordered_map<boss::Symbol, std::unordered_map<std::string, std::shared_ptr<Expression>>>
      tablePredicates;
  // Map of view names to the predicates that apply to them
  // Views are kept opaque at walking stage and at comparison stage
  // are either expanded or directly compared based on whether the view is materialised
  std::unordered_map<boss::Symbol, std::unordered_map<std::string, std::shared_ptr<Expression>>>
      viewPredicates;
  // Map of projected column names across the entire query
  std::unordered_map<std::string, std::shared_ptr<Expression>> projectedColumns;
  // Join relationships between sources in the query
  std::vector<JoinEdge> joinEdges;
};

// Metadata collected during expression tree walking
// - dependencies: view names referenced via QueryView
// - sideEffect: true if the definition contains prohibited operations
// - signature: tables, predicates, projections, and joins found in the expression
// - referencedTableColums: a map of table names to the set of column names referenced in the
// expression
struct ViewMetadata {
  std::unordered_set<boss::Symbol> dependencies;
  bool sideEffect = false;
  Signature signature;
  std::unordered_map<boss::Symbol, std::unordered_set<boss::Symbol>> referencedTableColumns;
};

// Recursively walks a view definition expression, populating ViewMetadata;
// Sources (tables and views) found in the current subtree are accumulated into
// the provided SourceSets, allowing callers to track which sources belong to
// which side of a join.
void walkView(const Expression &expr, ViewMetadata &metadata);

// Scores a view against a query based on how well the view's signature covers the query's
// signature; Returns -1.0 if view is not usable, otherwise a score >= 0, <= 1.0
double scoreView(const Signature &viewParts, const Signature &queryParts);

// Return the best scoring view for the given query, or nullopt if no rewriting is possible
// Currently only returns a perfect match (score of 1.0)
std::optional<boss::Symbol> findRewriting(const Expression &query, ViewMetadata &queryMetadata);