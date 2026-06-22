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
  // Set of base tables the expression references
  std::unordered_set<boss::Symbol> baseTables;
  // Map of column name to the predicates that apply to it,
  // which are stored as a serialised string key to the actual expression
  std::unordered_map<boss::Symbol, std::unordered_map<std::string, std::shared_ptr<Expression>>>
      columnPredicates;
  // Map of projected column names to the expressions that define them across the entire query
  std::unordered_map<boss::Symbol, std::shared_ptr<Expression>> projectedColumns;
  // Join relationships between sources in the query
  std::vector<JoinEdge> joinEdges;

  // Goes through columnPredicates, projectedColumns, and joinEdges to extract the columns needed to
  // evaluate the expression; used for column pruning for the Gather operator
  void extractAllReferencedColumns(std::unordered_set<boss::Symbol> &out) const;
};

// Metadata collected during expression tree walking
// - dependencies: view names referenced via QueryView
// - referencedColumns: columns referenced in the expression, used for pruning Gather outputs,
// populated with extractAllReferencedColumns instead of during the walk
// - sideEffect: true if the definition contains prohibited operations
// - signature: tables, predicates, projections, and joins found in the expression
// - merge: helper function to combine metadata from subtrees
struct ViewMetadata {
  std::unordered_set<boss::Symbol> dependencies;
  std::unordered_set<boss::Symbol> referencedColumns;
  bool sideEffect = false;
  Signature signature;

  void merge(ViewMetadata &&other);
};

// Recursively walks a view definition expression, populating ViewMetadata;
void walkView(const Expression &expr, ViewMetadata &metadata);

// Recursively resolve view dependencies, expanding the signature of a view definition
void expandSignature(ViewMetadata &metadata, std::unordered_map<boss::Symbol, ViewMetadata> &cache,
                     std::unordered_set<boss::Symbol> &seen);

// Scores a view against a query based on how well the view's signature covers the query's
// signature; Returns -1.0 if view is not usable, otherwise a score >= 0, <= 1.0
double scoreView(const Signature &viewParts, const Signature &queryParts);

// Return the best scoring view for the given query, or nullopt if no rewriting is possible
// Currently only returns a perfect match (score of 1.0)
std::optional<boss::Symbol> findRewriting(const Expression &query, ViewMetadata &queryMetadata,
                                          std::unordered_map<boss::Symbol, ViewMetadata> &cache,
                                          std::unordered_set<boss::Symbol> &seen);