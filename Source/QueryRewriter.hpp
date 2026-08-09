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

// Used in scoring to determine if we need a residual filter for a specific domain
enum class DomainCoverage { NONE, COVERS, EQUAL };

// Variant type for values used in predicates, used for canonicalising expressions
using DomainValue = std::variant<bool, int64_t, double, std::string>;

// nullopt on either lower or upper means unbounded
struct Interval {
  std::optional<DomainValue> lower;
  std::optional<DomainValue> upper;
  bool lowerInclusive = true;
  bool upperInclusive = true;
};

struct ColumnDomain {
  std::vector<Interval> ranges; // sorted by lower bound, non-overlapping
  bool unrepresentable = false; // if there is a type clash while merging predicates, tell the
                                // rewriter to reject for this column
};

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
  // Domains are ranges of values of that column that satisfy a given predicate
  // Predicates like Greater, Less, LessEqual are stored here
  std::unordered_map<boss::Symbol, ColumnDomain> columnDomains;
  // If the destructive side of a left / anti join has predicates then we cannot "correctly"
  // represent these predicates in the signature, so we reject all attempts to rewrite an incoming
  // query using this view, also block any rewriting if the query itself has such a predicate
  bool hasUnsafeJoinPredicate = false;
  // Map of column name to the predicates that apply to it,
  // which are stored as a serialised string key to the actual expression
  // Predicates like Match_Substring, Like, IsValid are stored here
  std::unordered_map<boss::Symbol, std::unordered_map<std::string, std::shared_ptr<Expression>>>
      columnPredicates;
  // Map of projected column names to the expressions that define them across the entire query
  std::unordered_map<boss::Symbol, std::shared_ptr<Expression>> projectedColumns;
  // Join relationships between sources in the query
  std::vector<JoinEdge> joinEdges;

  // Goes through columnPredicates, columnDomains, projectedColumns, and joinEdges to extract the columns needed to
  // evaluate the expression; used for column pruning for the Gather operator
  void extractAllReferencedColumns(std::unordered_set<boss::Symbol> &out) const;
};

// Metadata collected during expression tree walking
// - dependencies: view names referenced via QueryView
// - referencedColumns: columns referenced in the expression, used for pruning Gather outputs,
// populated with extractAllReferencedColumns instead of during the walk
// - sideEffect: true if the definition contains prohibited operations
// - signature: tables, predicates, projections, and joins found in the expression
// - intersectMerge: helper function to combine metadata from subtrees
struct ViewMetadata {
  std::unordered_set<boss::Symbol> dependencies;
  std::unordered_set<boss::Symbol> referencedColumns;
  bool sideEffect = false;
  Signature signature;

  void intersectMerge(ViewMetadata &&other);
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
// If the view does not match perfectly, it wraps it in a residual Filter and/or Project operator
// Doesn't handle any residual join logic for now
std::optional<boss::Expression> findRewriting(const Expression &query, ViewMetadata &queryMetadata,
                                              std::unordered_map<boss::Symbol, ViewMetadata> &cache,
                                              std::unordered_set<boss::Symbol> &seen);