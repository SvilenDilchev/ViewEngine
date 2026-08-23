#include "QueryRewriter.hpp"
#include "Cache.hpp"
#include "CachingProtocol.hpp"
#include "MetadataRegistry.hpp"
#include "ViewRegistry.hpp"
#include <Expression.hpp>

#include <algorithm>
#include <ranges>
#include <unordered_map>
#include <unordered_set>

using boss::expressions::CloneReason;
using boss::utilities::operator""_;

static const std::unordered_set<boss::Symbol> sideEffectOperators = {
    "DefineView"_, "DropView"_,    "ClearViews"_,    "RegisterTable"_,
    "DropTable"_,  "ClearTables"_, "SetCacheBudget"_};

static const std::unordered_set<boss::Symbol> rewritableOperators = {
    "Filter"_, "Join"_, "LeftJoin"_, "AntiJoin"_, "Project"_};

// Set of operators that will be compared by serialising the expression and comparing the strings.
static const std::unordered_set<boss::Symbol> opaquePredicateOperators = {
    "Like"_, "Match_Substring"_, "IsValid"_};

// Set of operators that will be compared semantically
static const std::unordered_set<boss::Symbol> domainPredicateOperators = {
    "Equal"_, "NotEqual"_, "Greater"_, "GreaterEqual"_, "Less"_, "LessEqual"_, "Between"_};

// Map for parsing BOSS symbols into JoinType enum values
static const std::unordered_map<boss::Symbol, JoinType> joinTypeMap = {
    {"Join"_, JoinType::INNER}, {"LeftJoin"_, JoinType::LEFT}, {"AntiJoin"_, JoinType::ANTI}};

// Operators whose operands can be reordered without changing meaning, so their serialised
// argument list is sorted to give equivalent predicates (e.g., Equal(a, b) vs Equal(b, a), or
// Or(P1, P2) vs Or(P2, P1)) the same canonical key
static const std::unordered_set<boss::Symbol> commutativePredicateOperators = {
    "Equal"_, "NotEqual"_, "And"_, "Or"_};

// Comparison operators whose operands can be swapped if the operator is flipped to its
// converse (e.g., Greater(a, b) means the same thing as Less(b, a))
static const std::unordered_map<boss::Symbol, boss::Symbol> conversePredicateOperators = {
    {"Greater"_, "Less"_},
    {"Less"_, "Greater"_},
    {"GreaterEqual"_, "LessEqual"_},
    {"LessEqual"_, "GreaterEqual"_}};

static std::string serialiseExpr(const Expression &expr) {
  return std::visit(boss::utilities::overload(
                        [](const ComplexExpression &ce) {
                          std::vector<std::string> argStrs;
                          argStrs.reserve(ce.getDynamicArguments().size());
                          for (const auto &arg : ce.getDynamicArguments())
                            argStrs.push_back(serialiseExpr(arg));

                          std::string headName = ce.getHead().getName();

                          if (commutativePredicateOperators.count(ce.getHead())) {
                            std::sort(argStrs.begin(), argStrs.end());
                          } else if (argStrs.size() == 2) {
                            if (auto it = conversePredicateOperators.find(ce.getHead());
                                it != conversePredicateOperators.end() && argStrs[1] < argStrs[0]) {
                              std::swap(argStrs[0], argStrs[1]);
                              headName = it->second.getName();
                            }
                          }

                          std::string result = headName + "(";
                          bool first = true;
                          for (auto &argStr : argStrs) {
                            if (!first)
                              result += ",";
                            result += argStr;
                            first = false;
                          }
                          return result + ")";
                        },
                        [](const Symbol &s) { return s.getName(); },
                        [](const std::string &s) { return "\"" + s + "\""; },
                        [](const auto &v) { return std::to_string(v); }),
                    expr);
}

// Returns a sorted vector of source names for a given set of sources, used for canonicalising
// join edges for comparison and scoring
static std::vector<std::string>
canonicalSourceKey(const std::unordered_set<boss::Symbol> &sources) {
  std::vector<std::string> names;
  names.reserve(sources.size());
  for (const auto &s : sources)
    names.push_back(s.getName());
  std::sort(names.begin(), names.end());
  return names;
}

// Helper function to extract column symbols
// Used to walk As operator expressions saved in projectedColumns
// and to get the columns referenced in Or expressions that are stored as opaque predicates
static void extractColumnsFromExpr(const Expression &expr, std::unordered_set<boss::Symbol> &out) {
  std::visit(boss::utilities::overload([&](const Symbol &s) { out.insert(s); },
                                       [&](const ComplexExpression &ce) {
                                         for (const auto &arg : ce.getDynamicArguments())
                                           extractColumnsFromExpr(arg, out);
                                       },
                                       [](const auto &) {}),
             expr);
}

void Signature::extractAllReferencedColumns(std::unordered_set<boss::Symbol> &out) const {
  // Column predicate keys are directly column symbols
  for (const auto &[col, _] : columnPredicates)
    out.insert(col);
  for (const auto &[col, _] : columnDomains)
    out.insert(col);
  for (const auto &[col, expr] : projectedColumns) {
    if (expr)
      extractColumnsFromExpr(*expr, out);
    else
      out.insert(col);
  }
  for (const auto &edge : joinEdges) {
    if (edge.leftKeys)
      extractColumnsFromExpr(*edge.leftKeys, out);
    if (edge.rightKeys)
      extractColumnsFromExpr(*edge.rightKeys, out);
  }
}

// Pairwise intersection of two ColumnDomains
static ColumnDomain intersectDomains(const ColumnDomain &a, const ColumnDomain &b) {
  ColumnDomain result;
  result.unrepresentable = a.unrepresentable || b.unrepresentable;

  for (const auto &rangeA : a.ranges) {
    for (const auto &rangeB : b.ranges) {
      Interval out;

      // lower bound
      if (!rangeA.lower) {
        out.lower = rangeB.lower;
        out.lowerInclusive = rangeB.lowerInclusive;
      } else if (!rangeB.lower) {
        out.lower = rangeA.lower;
        out.lowerInclusive = rangeA.lowerInclusive;
      } else if (rangeA.lower->index() != rangeB.lower->index()) {
        result.unrepresentable = true;
        continue;
      } else if (*rangeA.lower > *rangeB.lower) {
        out.lower = rangeA.lower;
        out.lowerInclusive = rangeA.lowerInclusive;
      } else if (*rangeB.lower > *rangeA.lower) {
        out.lower = rangeB.lower;
        out.lowerInclusive = rangeB.lowerInclusive;
      } else {
        out.lower = rangeA.lower;
        out.lowerInclusive = rangeA.lowerInclusive && rangeB.lowerInclusive;
      }

      // upper bound
      if (!rangeA.upper) {
        out.upper = rangeB.upper;
        out.upperInclusive = rangeB.upperInclusive;
      } else if (!rangeB.upper) {
        out.upper = rangeA.upper;
        out.upperInclusive = rangeA.upperInclusive;
      } else if (rangeA.upper->index() != rangeB.upper->index()) {
        result.unrepresentable = true;
        continue;
      } else if (*rangeA.upper < *rangeB.upper) {
        out.upper = rangeA.upper;
        out.upperInclusive = rangeA.upperInclusive;
      } else if (*rangeB.upper < *rangeA.upper) {
        out.upper = rangeB.upper;
        out.upperInclusive = rangeB.upperInclusive;
      } else {
        out.upper = rangeA.upper;
        out.upperInclusive = rangeA.upperInclusive && rangeB.upperInclusive;
      }

      // drop if empty
      if (out.lower && out.upper) {
        if (*out.lower > *out.upper)
          continue;
        if (*out.lower == *out.upper && !(out.lowerInclusive && out.upperInclusive))
          continue;
      }

      result.ranges.push_back(out);
    }
  }

  return result;
}

// Compares two projected column expressions with the same name.
static bool projectedExprsMatch(const std::shared_ptr<Expression> &a,
                                const std::shared_ptr<Expression> &b) {
  if (!a && !b)
    return true; // both are null, so they match
  if (!a || !b)
    return false; // One is aliased and the other is not, so they don't match
  return serialiseExpr(*a) == serialiseExpr(*b); // Compare the serialised expressions for equality
}

// Merge that results in the intersection of the predicates in the two signatures
// Used for And operator handling in Filters and for Join operator handling
void ViewMetadata::intersectMerge(ViewMetadata &&other) {
  dependencies.merge(other.dependencies);
  sideEffect = sideEffect || other.sideEffect;
  signature.baseTables.merge(other.signature.baseTables);

  for (auto &[col, domain] : other.signature.columnDomains) {
    auto it = signature.columnDomains.find(col);
    if (it == signature.columnDomains.end())
      signature.columnDomains.emplace(col, std::move(domain));
    else
      it->second = intersectDomains(it->second, domain);
  }

  for (auto &[col, preds] : other.signature.columnPredicates)
    signature.columnPredicates[col].merge(preds);

  for (auto &[col, expr] : other.signature.projectedColumns) {
    auto it = signature.projectedColumns.find(col);
    if (it != signature.projectedColumns.end()) {
      if (!projectedExprsMatch(it->second, expr)) {
        // Conflicting projections on the same column from different join branches
        sideEffect = true;
        return;
      }
    } else {
      signature.projectedColumns.emplace(col, expr);
    }
  }

  signature.joinEdges.insert(signature.joinEdges.end(),
                             std::make_move_iterator(other.signature.joinEdges.begin()),
                             std::make_move_iterator(other.signature.joinEdges.end()));
}

// Checks if a view join predicate is non-destructive with respect to
// the information that the query is interested in
static bool isSafeUnmatchedJoin(const JoinEdge &viewEdge, const Signature &queryParts) {
  if (viewEdge.joinType != JoinType::LEFT && viewEdge.joinType != JoinType::ANTI)
    return false; // If join is destructive on both sides (e.g., Inner Join)

  for (const auto &src : viewEdge.rightSources) {
    if (queryParts.baseTables.count(src))
      return false; // If query cares about the source on the right (destructive side)

    for (const auto &queryEdge : queryParts.joinEdges)
      if (queryEdge.leftSources.count(src) || queryEdge.rightSources.count(src))
        return false; // If query has a join involving the view source on the right
  }
  return true;
}

// Extract the literal out of an Expression for intervalisation
static std::optional<DomainValue> extractDomainValue(const Expression &expr) {
  return std::visit(
      boss::utilities::overload(
          [](bool b) -> std::optional<DomainValue> { return DomainValue{b}; },
          [](int64_t i) -> std::optional<DomainValue> { return DomainValue{i}; },
          [](int32_t i) -> std::optional<DomainValue> { return DomainValue{int64_t{i}}; },
          [](double d) -> std::optional<DomainValue> { return DomainValue{d}; },
          [](float f) -> std::optional<DomainValue> { return DomainValue{double{f}}; },
          [](const std::string &s) -> std::optional<DomainValue> { return DomainValue{s}; },
          [](const auto &) -> std::optional<DomainValue> { return std::nullopt; }),
      expr);
}

// Reverse of extractDomainValue, used to build comparison expressions from intervals
static Expression domainValueToExpression(const DomainValue &value) {
  return std::visit([](const auto &v) -> Expression { return v; }, value);
}

// Updates the domain ranges by intersercting with a new interval
static void intersectDomain(std::unordered_map<Symbol, ColumnDomain> &domains, const Symbol &col,
                            const Interval &newInterval) {
  auto it = domains.find(col);
  if (it == domains.end()) {
    // First predicate ever seen on this column just becomes the domain.
    domains.emplace(col, ColumnDomain{{newInterval}, false});
    return;
  }

  it->second = intersectDomains(it->second, ColumnDomain{{newInterval}, false});
};

// Updates domain ranges by subtracting a specific point
static void subtractPoint(std::unordered_map<Symbol, ColumnDomain> &domains, const Symbol &col,
                          const DomainValue &point) {
  auto it = domains.find(col);
  if (it == domains.end()) {
    domains.emplace(col, ColumnDomain{{Interval{std::nullopt, point, true, false},
                                       Interval{point, std::nullopt, false, true}},
                                      false});
    return;
  }

  ColumnDomain &existing = it->second;
  std::vector<Interval> result;

  for (const auto &range : existing.ranges) {
    // Type checks
    if (range.lower && range.lower->index() != point.index()) {
      existing.unrepresentable = true;
      continue;
    }
    if (range.upper && range.upper->index() != point.index()) {
      existing.unrepresentable = true;
      continue;
    }

    bool pointBelowRange =
        range.lower && (*range.lower > point || (*range.lower == point && !range.lowerInclusive));
    bool pointAboveRange =
        range.upper && (*range.upper < point || (*range.upper == point && !range.upperInclusive));
    if (pointBelowRange || pointAboveRange) {
      result.push_back(range); // point doesn't touch this range at all
      continue;
    }

    bool pointAtLowerEdge = range.lower && range.lowerInclusive && *range.lower == point;
    bool pointAtUpperEdge = range.upper && range.upperInclusive && *range.upper == point;
    if (pointAtLowerEdge && pointAtUpperEdge) {
      continue; // range was exactly {v}, remove it
    }
    if (pointAtLowerEdge) {
      result.push_back(Interval{point, range.upper, false, range.upperInclusive});
      continue;
    }
    if (pointAtUpperEdge) {
      result.push_back(Interval{range.lower, point, range.lowerInclusive, false});
      continue;
    }

    // Point inside the range, split into two ranges
    result.push_back(Interval{range.lower, point, range.lowerInclusive, false});
    result.push_back(Interval{point, range.upper, false, range.upperInclusive});
  }

  existing.ranges = std::move(result);
}

// Builds an interval from a comparison operator and a literal value, taking into account which
// argument is the column symbol
static Interval buildComparisonInterval(const Symbol &head, const DomainValue &literal,
                                        bool colIsArg0) {
  Interval interval;

  if (head == "Equal"_) {
    interval.lower = literal;
    interval.upper = literal;
    interval.lowerInclusive = true;
    interval.upperInclusive = true;
  } else if (head == "Greater"_) {
    if (colIsArg0) {
      interval.lower = literal;
      interval.lowerInclusive = false;
    } else {
      interval.upper = literal;
      interval.upperInclusive = false;
    }
  } else if (head == "GreaterEqual"_) {
    if (colIsArg0) {
      interval.lower = literal;
      interval.lowerInclusive = true;
    } else {
      interval.upper = literal;
      interval.upperInclusive = true;
    }
  } else if (head == "Less"_) {
    if (colIsArg0) {
      interval.upper = literal;
      interval.upperInclusive = false;
    } else {
      interval.lower = literal;
      interval.lowerInclusive = false;
    }
  } else if (head == "LessEqual"_) {
    if (colIsArg0) {
      interval.upper = literal;
      interval.upperInclusive = true;
    } else {
      interval.lower = literal;
      interval.lowerInclusive = true;
    }
  }

  return interval;
}

void walkView(const Expression &expr, ViewMetadata &metadata) {
  std::visit(
      boss::utilities::overload(
          [&](const ComplexExpression &ce) {
            if (metadata.sideEffect)
              return; // Short circuit if we've already detected a side effect

            const auto &head = ce.getHead();
            const auto &dynamics = ce.getDynamicArguments();

            if (head == "Table"_) {
              // A materialized Table's dynamic arguments are the actual data cells (ACE doesn't
              // produce span-based columns, so a real result can be millions of individual
              // Expression args), not query structure - regular queries being rewritten never
              // have an inlined Table whose contents affect the signature (ByName/QueryView/
              // predicates never appear inside one), so recursing into it is always wasted work,
              // cheap for a small literal and very expensive for a real materialized result at
              // the tail of a VE->ACE->VE pipeline. Treat it as an opaque leaf.
              return;
            }

            if (head == "QueryView"_) {
              // Treat malformed QueryView as side effect
              if (dynamics.size() < 2 || dynamics.size() > 4) {
                metadata.sideEffect = true;
                return;
              }

              const auto *viewName = std::get_if<Symbol>(&dynamics[0]);
              if (!viewName) {
                metadata.sideEffect = true;
                return;
              }

              const auto *decisionSym = std::get_if<Symbol>(&dynamics[1]);
              if (!decisionSym || !symbolToCachingDecision(decisionSym)) {
                metadata.sideEffect = true;
                return;
              }

              if (dynamics.size() >= 3 && !std::get_if<Symbol>(&dynamics[2])) {
                metadata.sideEffect = true;
                return;
              }
              if (dynamics.size() == 4 && !std::get_if<Symbol>(&dynamics[3])) {
                metadata.sideEffect = true;
                return;
              }

              metadata.dependencies.insert(*viewName);
              return;
            }

            // Detect side prohibited effects
            if (sideEffectOperators.count(head)) {
              metadata.sideEffect = true;
              return;
            }

            if (head == "ByName"_) {
              if (dynamics.empty()) {
                metadata.sideEffect = true;
                return;
              }

              const auto *tableName = std::get_if<Symbol>(&dynamics[0]);
              if (!tableName) {
                metadata.sideEffect = true; // Treat malformed ByName as side effect
                return;
              }

              metadata.signature.baseTables.insert(*tableName);
              return;
            }

            if (head == "Filter"_) {
              if (dynamics.size() != 2) {
                metadata.sideEffect = true; // Treat malformed Filter as side effect
                return;
              }

              walkView(dynamics[0], metadata); // Walk source
              walkView(dynamics[1], metadata); // Walk predicate
              return;
            }

            if (head == "Project"_) {
              if (dynamics.empty()) {
                metadata.sideEffect = true; // Treat malformed Project as side effect
                return;
              }

              walkView(dynamics[0], metadata);
              metadata.signature.projectedColumns.clear();
              for (size_t i = 1; i < dynamics.size(); ++i) {
                if (const auto *s = std::get_if<Symbol>(&dynamics[i])) {
                  metadata.signature.projectedColumns.emplace(*s, nullptr);
                } else if (const auto *ce = std::get_if<ComplexExpression>(&dynamics[i])) {
                  if (ce->getHead() != "As"_ ||
                      ce->getDynamicArguments().size() != 2) { // Handle As(expr, alias) projections
                    metadata.sideEffect = true;
                    return;
                  }
                  const auto *alias = std::get_if<Symbol>(&ce->getDynamicArguments()[1]);
                  if (!alias) {
                    metadata.sideEffect = true;
                    return;
                  }
                  auto shared = std::make_shared<Expression>(
                      ce->getDynamicArguments()[0].clone(CloneReason::EXPRESSION_WRAPPING));
                  metadata.signature.projectedColumns.emplace(*alias, std::move(shared));
                  walkView(ce->getDynamicArguments()[0], metadata);
                }
              }
              return;
            }

            if (auto it = joinTypeMap.find(head); it != joinTypeMap.end()) {
              if (dynamics.size() < 2) {
                metadata.sideEffect = true;
                return;
              }

              JoinType parsedJoinType = it->second;

              ViewMetadata leftMeta, rightMeta;
              walkView(dynamics[0], leftMeta);
              walkView(dynamics[1], rightMeta);

              std::unordered_set<boss::Symbol> allLefts = leftMeta.signature.baseTables;
              allLefts.insert(leftMeta.dependencies.begin(), leftMeta.dependencies.end());

              std::unordered_set<boss::Symbol> allRights = rightMeta.signature.baseTables;
              allRights.insert(rightMeta.dependencies.begin(), rightMeta.dependencies.end());

              metadata.intersectMerge(std::move(leftMeta));
              // Remove the predicates for the destructive side of an unsafe join and flag the
              // signature as unsafe
              if (parsedJoinType == JoinType::LEFT || parsedJoinType == JoinType::ANTI)
                if (!rightMeta.signature.columnDomains.empty() ||
                    !rightMeta.signature.columnPredicates.empty()) {
                  metadata.signature.hasUnsafeJoinPredicate = true;
                  rightMeta.signature.columnDomains.clear();
                  rightMeta.signature.columnPredicates.clear();
                }
              metadata.intersectMerge(std::move(rightMeta));

              std::vector<std::pair<Expression, Expression>> keyPairs;
              for (size_t i = 2; i < dynamics.size(); ++i) {
                const auto *pred = std::get_if<ComplexExpression>(&dynamics[i]);
                if (pred && pred->getHead() == "Equal"_) {
                  const auto &args = pred->getDynamicArguments();
                  if (args.size() == 2) {
                    keyPairs.emplace_back(args[0].clone(CloneReason::EXPRESSION_WRAPPING),
                                          args[1].clone(CloneReason::EXPRESSION_WRAPPING));
                  }
                } else {
                  // It is a residual filter (or a boolean literal, etc.): assign to tables
                  walkView(dynamics[i], metadata);
                }
              }

              std::sort(keyPairs.begin(), keyPairs.end(), [](const auto &a, const auto &b) {
                auto aFirst = serialiseExpr(a.first);
                auto bFirst = serialiseExpr(b.first);
                if (aFirst != bFirst)
                  return aFirst < bFirst;
                return serialiseExpr(a.second) < serialiseExpr(b.second);
              });

              boss::ExpressionArguments leftKeysArgs, rightKeysArgs;
              for (auto &[leftKey, rightKey] : keyPairs) {
                leftKeysArgs.push_back(std::move(leftKey));
                rightKeysArgs.push_back(std::move(rightKey));
              }

              // TODO: leftKeys/rightKeys are wrapped in a synthetic "Keys(...)"
              // ComplexExpression as a patch to avoid rewriting the serialiser and scorer to
              // handle raw key lists directly. Should perhaps store join keys as a vector of
              // expressions instead of this wrapper.
              Expression leftKeys = ComplexExpression("Keys"_, {}, std::move(leftKeysArgs));
              Expression rightKeys = ComplexExpression("Keys"_, {}, std::move(rightKeysArgs));

              metadata.signature.joinEdges.push_back(
                  {std::move(allLefts), std::move(allRights),
                   std::make_shared<Expression>(std::move(leftKeys)),
                   std::make_shared<Expression>(std::move(rightKeys)), parsedJoinType});
              return;
            }

            if (head == "And"_) {
              if (dynamics.size() < 2) {
                metadata.sideEffect = true; // Malformed And is treated as side effect
                return;
              }

              for (const auto &arg : dynamics)
                walkView(arg, metadata);
              return;
            }

            if (head == "Or"_) {
              if (dynamics.size() < 2) {
                metadata.sideEffect = true; // malformed Or
                return;
              }

              std::vector<ViewMetadata> branches;
              branches.reserve(dynamics.size());

              for (const auto &arg : dynamics) {
                ViewMetadata branchMeta;
                walkView(arg, branchMeta);
                branches.push_back(std::move(branchMeta));
              }

              bool unionisable = true;
              std::optional<boss::Symbol> sharedColumn;
              for (auto &branch : branches) {
                // Merge bookkeeping that's independent of the domain / opaque predicates
                metadata.sideEffect = metadata.sideEffect || branch.sideEffect ||
                                      !branch.signature.joinEdges.empty() ||
                                      !branch.signature.projectedColumns.empty();
                metadata.dependencies.merge(branch.dependencies);
                metadata.signature.baseTables.merge(branch.signature.baseTables);

                // Check if the or can be unionised for must be handled as an opaque predicate.
                // Branches touching different columns can never be unionised into one
                // ColumnDomain - see the DNF TODO on ColumnDomain in QueryRewriter.hpp.
                if (!branch.signature.columnPredicates.empty() ||
                    branch.signature.columnDomains.size() != 1)
                  unionisable = false;
                else if (branch.signature.columnDomains.size() == 1) {
                  if (!sharedColumn)
                    sharedColumn = branch.signature.columnDomains.begin()->first;
                  else if (*sharedColumn != branch.signature.columnDomains.begin()->first)
                    unionisable = false;
                }
              }

              if (metadata.sideEffect)
                return;

              if (unionisable) {
                bool unrepresentable = false;
                std::vector<Interval> allRanges;

                // Collect all ranges from the branches for the shared column
                for (const auto &branch : branches) {
                  const ColumnDomain &domain = branch.signature.columnDomains.at(*sharedColumn);
                  if (domain.unrepresentable) {
                    unrepresentable = true;
                    break;
                  }
                  allRanges.insert(allRanges.end(), domain.ranges.begin(), domain.ranges.end());
                }

                if (unrepresentable) {
                  metadata.signature.columnDomains[*sharedColumn] = ColumnDomain{{}, true};
                  return;
                }
                if (allRanges.empty()) {
                  metadata.signature.columnDomains[*sharedColumn] = ColumnDomain{{}, false};
                  return;
                }

                // Type checks
                std::optional<size_t> typeIndex;
                bool typeClash = false;

                for (const auto &r : allRanges) {
                  for (const auto &bound : {r.lower, r.upper}) {
                    if (!bound)
                      continue;
                    if (!typeIndex)
                      typeIndex = bound->index();
                    else if (*typeIndex != bound->index())
                      typeClash = true;
                  }
                }

                if (typeClash) {
                  metadata.signature.columnDomains[*sharedColumn] = ColumnDomain{{}, true};
                  return;
                }

                // Sort all ranges
                std::sort(allRanges.begin(), allRanges.end(),
                          [](const Interval &x, const Interval &y) {
                            if (!x.lower && !y.lower)
                              return false;
                            if (!x.lower)
                              return true; // x unbounded below, sorts first
                            if (!y.lower)
                              return false;
                            if (*x.lower != *y.lower)
                              return *x.lower < *y.lower;
                            // same lower value: inclusive bound sorts first (covers more)
                            return x.lowerInclusive && !y.lowerInclusive;
                          });

                // Union merge ranges 1 by 1
                std::vector<Interval> merged;
                merged.push_back(allRanges[0]);

                for (size_t i = 1; i < allRanges.size(); ++i) {
                  Interval &last = merged.back();
                  const Interval &cur = allRanges[i];

                  // Do cur and last overlap or touch (adjacent with at least one side inclusive)
                  // Last is unbounded above; current is unbounded below;
                  // Current starts before last ends; or current starts exactly where last ends
                  // and at least one side is inclusive so we don't have a missing point
                  bool overlapsOrTouches =
                      !last.upper || !cur.lower || *cur.lower < *last.upper ||
                      (*cur.lower == *last.upper && (last.upperInclusive || cur.lowerInclusive));

                  // Union them by just adding curr with the gap between them still there
                  if (!overlapsOrTouches) {
                    merged.push_back(cur);
                    continue;
                  }

                  // Extend last's upper bound if cur reaches further
                  if (!cur.upper) {
                    last.upper = std::nullopt;
                    last.upperInclusive = true;
                  } else if (!last.upper) {
                    // last is already unbounded above, stays unbounded
                  } else if (*cur.upper > *last.upper) {
                    last.upper = cur.upper;
                    last.upperInclusive = cur.upperInclusive;
                  } else if (*cur.upper == *last.upper) {
                    last.upperInclusive = last.upperInclusive || cur.upperInclusive;
                  }
                  // else cur.upper < last.upper: cur is fully contained in last, nothing to do
                }

                // Fully unrestricted after union
                // (domain predicate should be removed from the signature)
                if (merged.size() == 1 && !merged[0].lower && !merged[0].upper) {
                  metadata.signature.columnDomains.erase(*sharedColumn);
                } else {
                  metadata.signature.columnDomains[*sharedColumn] =
                      ColumnDomain{std::move(merged), false};
                }
              } else {
                // Fallback to opaque predicate handling for the whole Or expression
                auto shared =
                    std::make_shared<Expression>(ce.clone(CloneReason::EXPRESSION_WRAPPING));
                auto key = serialiseExpr(*shared);

                std::unordered_set<Symbol> refCols;
                extractColumnsFromExpr(*shared, refCols);
                for (const auto &col : refCols)
                  metadata.signature.columnPredicates[col].emplace(key, shared);
              }

              return;
            }

            if (head == "Between"_) {
              if (dynamics.size() != 3) {
                metadata.sideEffect = true;
                return;
              }

              const auto *colSym = std::get_if<Symbol>(&dynamics[0]);
              if (!colSym) {
                metadata.sideEffect = true;
                return;
              }

              auto low = extractDomainValue(dynamics[1]);
              auto high = extractDomainValue(dynamics[2]);
              if (!low || !high) {
                metadata.sideEffect = true; // bounds must be literals we can intervalize
                return;
              }

              Interval interval;
              interval.lower = *low;
              interval.upper = *high;
              interval.lowerInclusive = true;
              interval.upperInclusive = true;

              intersectDomain(metadata.signature.columnDomains, *colSym, interval);
              return;
            }

            if (domainPredicateOperators.count(head)) {
              if (dynamics.size() != 2) {
                metadata.sideEffect = true;
                return;
              }

              bool arg0IsSym = std::holds_alternative<Symbol>(dynamics[0]);
              bool arg1IsSym = std::holds_alternative<Symbol>(dynamics[1]);

              if (!arg0IsSym && !arg1IsSym) {
                metadata.sideEffect = true;
                return;
              }

              // Both symbols, no domain possible, just save as opaque predicate for scoring
              if (arg0IsSym && arg1IsSym) {
                auto colSym0 = std::get<Symbol>(dynamics[0]);
                auto colSym1 = std::get<Symbol>(dynamics[1]);

                // Do not actually mark columns not being in the registry as a side effect;
                // leave this part of expression correctness up to the user, what is
                // actually important is query rewriter correctness which this does not
                // affect. Also this is a symptom of a larger problem, relating to which
                // sources are present in a given subtree for validation, but fixing that
                // requires a significant refactor of what happens at define time and more
                // importantly - redefine time, while being functionally irrelevant if the
                // expression is not actually malformed.

                //  if (columnRegistry.find(colSym0) == columnRegistry.end() ||
                //      columnRegistry.find(colSym1) == columnRegistry.end()) {
                //    metadata.sideEffect = true;
                //    return;
                //  }

                auto shared =
                    std::make_shared<Expression>(ce.clone(CloneReason::EXPRESSION_WRAPPING));
                auto key = serialiseExpr(*shared);

                metadata.signature.columnPredicates[colSym0].emplace(key, shared);
                metadata.signature.columnPredicates[colSym1].emplace(std::move(key),
                                                                     std::move(shared));
                return;
              }

              bool colIsArg0 = arg0IsSym;
              auto colSym = std::get<Symbol>(colIsArg0 ? dynamics[0] : dynamics[1]);
              auto literal = extractDomainValue(colIsArg0 ? dynamics[1] : dynamics[0]);
              if (!literal) {
                metadata.sideEffect = true;
                return;
              }

              if (head == "NotEqual"_) {
                // Subtract the literal from the ranges in the column domain
                subtractPoint(metadata.signature.columnDomains, colSym, *literal);
                return;
              }

              // Build an interval and intersect
              Interval interval = buildComparisonInterval(head, *literal, colIsArg0);
              intersectDomain(metadata.signature.columnDomains, colSym, interval);
              return;
            }

            // Non-domain predicates stored as serialised expressions for scoring
            if (opaquePredicateOperators.count(head)) {
              if (dynamics.empty() || !std::holds_alternative<Symbol>(dynamics[0])) {
                metadata.sideEffect = true;
                return;
              }

              auto colSym = std::get<Symbol>(dynamics[0]);

              // Again skip check, see comment re: the same check in domainPredicateOperators
              //  if (columnRegistry.find(colSym) == columnRegistry.end()) {
              //    metadata.sideEffect = true;
              //    return;
              //  }

              auto shared =
                  std::make_shared<Expression>(ce.clone(CloneReason::EXPRESSION_WRAPPING));
              auto key = serialiseExpr(*shared);

              metadata.signature.columnPredicates[colSym].emplace(std::move(key),
                                                                  std::move(shared));
              return;
            }

            // TODO: handle aggregations
            // For now it's with the passthroughs like ordering operators
            if (head == "GroupBy"_ || head == "OrderBy"_ || head == "Slice"_) {
              if (dynamics.empty()) {
                // Treat malformed passthrough operators as side effect
                metadata.sideEffect = true;
              }
              for (size_t i = 0; i < dynamics.size(); ++i) {
                walkView(dynamics[i], metadata);
              }
              return;
            }

            for (const auto &arg : dynamics) {
              walkView(arg, metadata);
            }
          },
          [&](const Symbol &s) {
            if (viewRegistry.count(s)) {
              metadata.dependencies.insert(s);
              return;
            }
            if (tableRegistry.count(s))
              metadata.signature.baseTables.insert(s);
          },
          [](const auto &) {}),
      expr);
}

// Expands view sources down to their base tables for accurate comparisons
void expandSignature(ViewMetadata &metadata, std::unordered_map<boss::Symbol, ViewMetadata> &cache,
                     std::unordered_set<boss::Symbol> &seen) {
  for (const auto &viewName : metadata.dependencies) {
    auto regIt = viewRegistry.find(viewName);
    if (regIt == viewRegistry.end() || seen.count(viewName))
      continue;

    seen.insert(viewName);
    if (!cache.count(viewName)) {
      // Cache signatures of views we've expanded to
      // avoid redundant work and infinite recursion on
      // cycles  expandSignature(cache[viewName], cache);
      auto &cached = cache[viewName];
      cached.signature = regIt->second.signature;
      cached.dependencies = regIt->second.dependencies;
      expandSignature(cached, cache, seen);
    }

    auto &viewMeta = cache[viewName];

    // Merge dependencies
    metadata.dependencies.insert(viewMeta.dependencies.begin(), viewMeta.dependencies.end());

    // Merge column predicates
    for (auto &[col, preds] : viewMeta.signature.columnPredicates)
      metadata.signature.columnPredicates[col].merge(preds);

    // Merge column domains with intersect semantics
    for (auto &[col, domain] : viewMeta.signature.columnDomains) {
      auto it = metadata.signature.columnDomains.find(col);
      if (it == metadata.signature.columnDomains.end())
        metadata.signature.columnDomains.emplace(col, domain);
      else
        it->second = intersectDomains(it->second, domain);
    }

    // Merge unsafe join predicate flag
    metadata.signature.hasUnsafeJoinPredicate =
        metadata.signature.hasUnsafeJoinPredicate || viewMeta.signature.hasUnsafeJoinPredicate;

    // Merge base tables
    metadata.signature.baseTables.insert(viewMeta.signature.baseTables.begin(),
                                         viewMeta.signature.baseTables.end());

    // Expand view name in join edges - replace view name with its base tables and remaining
    // dependencies in the sources
    for (auto &edge : metadata.signature.joinEdges) {
      bool wasLeft = edge.leftSources.erase(viewName);
      bool wasRight = edge.rightSources.erase(viewName);
      if (!wasLeft && !wasRight)
        continue;
      for (const auto &table : viewMeta.signature.baseTables) {
        if (wasLeft)
          edge.leftSources.insert(table);
        if (wasRight)
          edge.rightSources.insert(table);
      }
      for (const auto &dep : viewMeta.dependencies) {
        if (wasLeft)
          edge.leftSources.insert(dep);
        if (wasRight)
          edge.rightSources.insert(dep);
      }
    }

    for (auto &edge : viewMeta.signature.joinEdges)
      metadata.signature.joinEdges.push_back(edge);
  }

  // Canonicalise inner join edges so that the left sources are always lexicographically smaller
  // than the right Important for matching semantically equivalent joins that are written in
  // different orders
  for (auto &edge : metadata.signature.joinEdges) {
    if (edge.joinType != JoinType::INNER)
      continue;
    if (canonicalSourceKey(edge.leftSources) > canonicalSourceKey(edge.rightSources)) {
      std::swap(edge.leftSources, edge.rightSources);
      std::swap(edge.leftKeys, edge.rightKeys);
    }
  }
}

// Returns the coverage of the query domain by the view domain
static DomainCoverage domainCoverage(const ColumnDomain &viewDomain,
                                     const ColumnDomain &queryDomain) {
  if (viewDomain.unrepresentable || queryDomain.unrepresentable)
    return DomainCoverage::NONE;

  if (queryDomain.ranges.empty())
    return DomainCoverage::NONE;

  bool allExact = true;

  for (const auto &qRange : queryDomain.ranges) {
    bool contained = false;
    bool exact = false;

    for (const auto &vRange : viewDomain.ranges) {
      bool lowerOk =
          !vRange.lower ||
          (qRange.lower &&
           (*qRange.lower > *vRange.lower ||
            (*qRange.lower == *vRange.lower && (!qRange.lowerInclusive || vRange.lowerInclusive))));
      bool upperOk =
          !vRange.upper ||
          (qRange.upper &&
           (*qRange.upper < *vRange.upper ||
            (*qRange.upper == *vRange.upper && (!qRange.upperInclusive || vRange.upperInclusive))));

      if (lowerOk && upperOk) {
        contained = true;
        bool lowerSame = (!vRange.lower && !qRange.lower) ||
                         (vRange.lower && qRange.lower && *vRange.lower == *qRange.lower &&
                          vRange.lowerInclusive == qRange.lowerInclusive);
        bool upperSame = (!vRange.upper && !qRange.upper) ||
                         (vRange.upper && qRange.upper && *vRange.upper == *qRange.upper &&
                          vRange.upperInclusive == qRange.upperInclusive);
        if (lowerSame && upperSame)
          exact = true;
        break;
      }
    }

    if (!contained)
      return DomainCoverage::NONE;
    if (!exact)
      allExact = false;
  }

  return allExact ? DomainCoverage::EQUAL : DomainCoverage::COVERS;
}

// Extracts a numeric value out of a DomainValue for width computation; bool/string values have
// no meaningful numeric width, so those report nullopt and callers fall back to a flat credit
static std::optional<double> numericValue(const DomainValue &value) {
  if (const auto *i = std::get_if<int64_t>(&value))
    return static_cast<double>(*i);
  if (const auto *d = std::get_if<double>(&value))
    return *d;
  return std::nullopt;
}

// Width of a single closed numeric interval; nullopt if either bound is missing (unbounded) or
// non-numeric - callers fall back to a flat credit in that case
static std::optional<double> intervalWidth(const Interval &interval) {
  if (!interval.lower || !interval.upper)
    return std::nullopt;
  auto lower = numericValue(*interval.lower);
  auto upper = numericValue(*interval.upper);
  if (!lower || !upper)
    return std::nullopt;
  return *upper - *lower;
}

// Credit for a non-exact (COVERS, not EQUAL) domain match: how much of the possible values in the
// view's domain are actually required by the query's domain. Only reached if the view does indeed
// cover the query's full domain.
static double domainCoverageCredit(const ColumnDomain *viewDomain,
                                   const ColumnDomain &queryDomain) {
  if (!viewDomain)
    return MIN_DOMAIN_PARTIAL_CREDIT;

  if (viewDomain->ranges.size() == 1 && queryDomain.ranges.size() == 1) {
    auto queryWidth = intervalWidth(queryDomain.ranges[0]);
    auto viewWidth = intervalWidth(viewDomain->ranges[0]);
    if (queryWidth && viewWidth && *viewWidth > 0.0)
      return std::clamp(*queryWidth / *viewWidth, MIN_DOMAIN_PARTIAL_CREDIT, 1.0);
  }

  return FALLBACK_DOMAIN_PARTIAL_CREDIT;
}

double scoreView(const Signature &viewParts, const Signature &queryParts) {
  // Condition 0: flags that directly block the rewriting process
  if (viewParts.hasUnsafeJoinPredicate || queryParts.hasUnsafeJoinPredicate)
    return -1.0;

  // Condition 1: view must cover every table the query needs. There's no residual Join
  // construction in findRewriting, only residual Filter/Project, so a view missing a table the
  // query joins in can't be patched up afterwards - the join (and its row-filtering/duplicating
  // effect) would just silently vanish from the rewritten query.
  size_t commonTables = 0;
  for (const auto &table : viewParts.baseTables)
    if (queryParts.baseTables.count(table))
      ++commonTables;

  if (commonTables != queryParts.baseTables.size())
    return -1.0;

  double tableCoverage = queryParts.baseTables.empty()
                             ? 1.0
                             : (double)commonTables / (double)queryParts.baseTables.size();

  // A non-exact predicate match on `col` needs a residual filter re-applied on top of the view's
  // output; if the view's projection drops that column, there's no way to re-filter on it, so the
  // view can't be used to answer that predicate at all.
  auto columnSurvivesProjection = [&](const boss::Symbol &col) {
    return viewParts.projectedColumns.empty() || viewParts.projectedColumns.count(col) > 0;
  };

  // Condition 2: the view must have weaker predicates than the query on shared tables
  double totalQueryPreds = 0;
  double coveredQueryPreds = 0;

  // 2a: opaque predicates (Like/Match_Substring/IsValid/col-vs-col), compared syntactically
  for (const auto &[col, viewPreds] : viewParts.columnPredicates) {
    auto qIt = queryParts.columnPredicates.find(col);
    // View applies an opaque predicate (Like/Match_Substring/IsValid/col-vs-col) on a column
    // the query doesn't; these have no domain representation, so we can't reason about them yet
    // and any mismatch is a hard rejection
    if (qIt == queryParts.columnPredicates.end())
      return -1.0;

    const auto &queryPreds = qIt->second;
    for (const auto &[key, _] : viewPreds)
      // Syntactic check of opaque predicates, can't reason for partial coverage
      if (!queryPreds.count(key))
        return -1.0;

    totalQueryPreds += queryPreds.size();
    bool needsResidual = false;
    for (const auto &[key, _] : queryPreds) {
      if (viewPreds.count(key))
        ++coveredQueryPreds;
      else
        needsResidual = true; // query has an opaque predicate on `col` the view doesn't already
                              // apply, so it'll need a residual filter referencing `col`
    }
    if (needsResidual && !columnSurvivesProjection(col))
      return -1.0; // the view projects `col` out, so that residual filter can't be re-applied
  }

  for (const auto &[col, queryPreds] : queryParts.columnPredicates)
    if (!viewParts.columnPredicates.count(col)) {
      // View has no opaque predicate on `col` at all, so the query's would need to be applied
      // in full as a residual filter; reject if the view doesn't expose the column for it
      if (!columnSurvivesProjection(col))
        return -1.0;
      totalQueryPreds += queryPreds.size();
    }

  // 2b: domain predicates (Equal/NotEqual/Greater/GreaterEqual/Less/LessEqual/Between), compared
  // semantically and scored for partial coverage
  for (const auto &[col, viewDomain] : viewParts.columnDomains) {
    auto qIt = queryParts.columnDomains.find(col);
    if (qIt == queryParts.columnDomains.end())
      return -1.0; // View has a domain predicate on a column the query doesn't

    const auto &queryDomain = qIt->second;
    DomainCoverage coverage = domainCoverage(viewDomain, queryDomain);
    if (coverage == DomainCoverage::NONE)
      return -1.0; // View's domain predicate is stronger than the query's, can't use it

    if (coverage != DomainCoverage::EQUAL && !columnSurvivesProjection(col))
      return -1.0; // View's predicate is weaker than the query's and needs tightening via a
                   // residual filter, but the view projects this column out

    coveredQueryPreds +=
        (coverage == DomainCoverage::EQUAL) ? 1.0 : domainCoverageCredit(&viewDomain, queryDomain);
  }

  totalQueryPreds += queryParts.columnDomains.size();
  for (const auto &[col, queryDomain] : queryParts.columnDomains)
    if (!viewParts.columnDomains.count(col)) {
      if (!columnSurvivesProjection(col))
        return -1.0; // View is unfiltered on this column and would need a residual filter for
                     // it, but the view projects this column out
      // View doesn't have a domain predicate on this column at all: usable but did no narrowing
      // work for it, so this is floored below any real (even wide) predicate's credit
      coveredQueryPreds += domainCoverageCredit(nullptr, queryDomain);
    }

  double predicateCoverage = (totalQueryPreds > 0) ? coveredQueryPreds / totalQueryPreds : 1.0;

  // Condition 3: the join predicates of the view are non-destructive to the query's join
  // predicates
  std::unordered_set<size_t> matchedQueryJoinIndices;
  for (const auto &viewEdge : viewParts.joinEdges) {
    auto viewLeftKey = serialiseExpr(*viewEdge.leftKeys);
    auto viewRightKey = serialiseExpr(*viewEdge.rightKeys);
    bool matched = false;
    for (size_t i = 0; i < queryParts.joinEdges.size(); ++i) {
      const auto &queryEdge = queryParts.joinEdges[i];
      // TODO: join matching still doesn't detect joins that are semantically equivalent via
      // column aliasing (e.g. Equal(A.customer_id, ...) vs Equal(A.cust_id, ...) referring to
      // the same underlying column under a different name). Needs schema-aware alias resolution.
      if (viewEdge.joinType == queryEdge.joinType &&
          viewEdge.leftSources == queryEdge.leftSources &&
          viewEdge.rightSources == queryEdge.rightSources &&
          viewLeftKey == serialiseExpr(*queryEdge.leftKeys) &&
          viewRightKey == serialiseExpr(*queryEdge.rightKeys)) {
        matchedQueryJoinIndices.insert(i);
        matched = true;
        break;
      }
    }
    if (!matched && !isSafeUnmatchedJoin(viewEdge, queryParts))
      return -1.0;
  }

  double joinCoverage = queryParts.joinEdges.empty() ? 1.0
                                                     : (double)matchedQueryJoinIndices.size() /
                                                           (double)queryParts.joinEdges.size();

  // Same reasoning as Condition 1: a join the query needs that the view doesn't already have
  // can't be synthesised as a residual afterwards, so the view can't be used at all.
  if (joinCoverage < 1.0)
    return -1.0;

  // Condition 4: the view must project at least all the columns the query projects
  double projectionCoverage = 1.0;
  if (!viewParts.projectedColumns.empty() && !queryParts.projectedColumns.empty()) {
    for (const auto &[col, queryExpr] : queryParts.projectedColumns) {
      auto it = viewParts.projectedColumns.find(col);
      if (it == viewParts.projectedColumns.end())
        // TODO: check if any view projection has the same underlying expression but a different
        // name — if so, a rename Project on top of the view could satisfy the query (partial
        // match)
        return -1.0;
      if (!projectedExprsMatch(it->second, queryExpr))
        return -1.0;
    }
    // Score is how many more columns the view projects on top of what the query needs,
    // as a fraction of the query's projections
    projectionCoverage =
        (double)queryParts.projectedColumns.size() / (double)viewParts.projectedColumns.size();
  } else if (!viewParts.projectedColumns.empty() && queryParts.projectedColumns.empty()) {
    std::unordered_set<boss::Symbol> queryColumns;
    for (const auto &table : queryParts.baseTables) {
      const auto &entry = tableRegistry.find(table);
      if (entry == tableRegistry.end())
        return -1.0; // Pessimistically reject if we cannot find the table in the registry
      for (const auto &col : entry->second.columns) {
        const auto it = viewParts.projectedColumns.find(col);
        if (it == viewParts.projectedColumns.end())
          return -1.0; // View doesn't project a column the query needs
        if (it->second)
          return -1.0; // If matched against column is computed, reject as the query wants a base
                       // column (no projections)
        queryColumns.insert(col);
      }
    }
    projectionCoverage = (double)queryColumns.size() / (double)viewParts.projectedColumns.size();
  } else if (viewParts.projectedColumns.empty() && !queryParts.projectedColumns.empty()) {
    // View returns all base columns but query needs specific projected columns.
    std::unordered_set<boss::Symbol> viewColumns;
    for (const auto &table : viewParts.baseTables) {
      const auto &entry = tableRegistry.find(table);
      if (entry == tableRegistry.end())
        return -1.0; // Pessimistically reject if we cannot find the table in the registry
      viewColumns.insert(entry->second.columns.begin(), entry->second.columns.end());
    }
    for (const auto &[col, expr] : queryParts.projectedColumns) {
      if (expr)
        return -1.0; // TODO: Have to check if the expression can be computed (partial matching)
                     // from the base columns, as there is no projected column to match against.
      if (!viewColumns.count(col))
        return -1.0; // View doesn't project a column the query needs
    }

    projectionCoverage = (double)queryParts.projectedColumns.size() / (double)viewColumns.size();
  }

  return W_TABLE * tableCoverage + W_PRED * predicateCoverage + W_JOIN * joinCoverage +
         W_PROJ * projectionCoverage;
}

static Expression intervalToExpression(const Symbol &col, const Interval &range) {
  bool hasLower = range.lower.has_value();
  bool hasUpper = range.upper.has_value();

  if (hasLower && !hasUpper) {
    boss::ExpressionArguments args;
    args.push_back(Symbol(col));
    args.push_back(domainValueToExpression(*range.lower));
    return ComplexExpression(range.lowerInclusive ? "GreaterEqual"_ : "Greater"_, {},
                             std::move(args), {});
  }

  if (!hasLower && hasUpper) {
    boss::ExpressionArguments args;
    args.push_back(Symbol(col));
    args.push_back(domainValueToExpression(*range.upper));
    return ComplexExpression(range.upperInclusive ? "LessEqual"_ : "Less"_, {}, std::move(args),
                             {});
  }

  // From here on we know both lower and upper bounds exist
  if (*range.lower == *range.upper) {
    boss::ExpressionArguments args;
    args.push_back(Symbol(col));
    args.push_back(domainValueToExpression(*range.lower));
    return ComplexExpression("Equal"_, {}, std::move(args), {});
  }

  // Utilise between operator if both bounds are inclusive, otherwise fall back to And(Greater/Less)
  if (range.lowerInclusive && range.upperInclusive) {
    boss::ExpressionArguments args;
    args.push_back(Symbol(col));
    args.push_back(domainValueToExpression(*range.lower));
    args.push_back(domainValueToExpression(*range.upper));
    return ComplexExpression("Between"_, {}, std::move(args), {});
  }

  boss::ExpressionArguments lowerArgs, upperArgs, andArgs;
  lowerArgs.push_back(Symbol(col));
  lowerArgs.push_back(domainValueToExpression(*range.lower));
  andArgs.push_back(ComplexExpression(range.lowerInclusive ? "GreaterEqual"_ : "Greater"_, {},
                                      std::move(lowerArgs), {}));

  upperArgs.push_back(Symbol(col));
  upperArgs.push_back(domainValueToExpression(*range.upper));
  andArgs.push_back(ComplexExpression(range.upperInclusive ? "LessEqual"_ : "Less"_, {},
                                      std::move(upperArgs), {}));

  return ComplexExpression("And"_, {}, std::move(andArgs), {});
}

// Convert a ColumnDomain into an Expression that represents the same predicate
// Used to build the residual filter
static std::optional<Expression> domainToExpression(const Symbol &col, const ColumnDomain &domain) {
  boss::ExpressionArguments pieces;
  for (const auto &range : domain.ranges) {
    if (!range.lower && !range.upper)
      continue; // fully unbounded range - no constraint to express, skip it

    pieces.push_back(intervalToExpression(col, range));
  }

  if (pieces.empty())
    return std::nullopt; // shouldn't in practice happen

  if (pieces.size() == 1)
    return std::move(pieces[0]);

  return ComplexExpression("Or"_, {}, std::move(pieces), {});
}

std::optional<Expression> computeResidualFilter(const Signature &viewParts,
                                                const Signature &queryParts) {
  std::unordered_map<std::string, std::shared_ptr<Expression>> missing;

  for (const auto &[col, queryPreds] : queryParts.columnPredicates) {
    auto vIt = viewParts.columnPredicates.find(col);
    bool colCovered = (vIt != viewParts.columnPredicates.end());

    for (const auto &[key, expr] : queryPreds) {
      if (!colCovered || !vIt->second.count(key))
        missing.emplace(key, expr);
    }
  }

  for (const auto &[col, queryDomain] : queryParts.columnDomains) {
    auto vIt = viewParts.columnDomains.find(col);

    DomainCoverage coverage = (vIt != viewParts.columnDomains.end())
                                  ? domainCoverage(vIt->second, queryDomain)
                                  : DomainCoverage::NONE;

    if (coverage != DomainCoverage::EQUAL) {
      if (auto expr = domainToExpression(col, queryDomain))
        missing.emplace(serialiseExpr(*expr), std::make_shared<Expression>(std::move(*expr)));
    }
  }

  if (missing.empty())
    return std::nullopt;

  boss::ExpressionArguments residualArgs;
  for (const auto &[key, expr] : missing)
    residualArgs.push_back(expr->clone(CloneReason::EXPRESSION_WRAPPING));

  if (residualArgs.size() == 1)
    return std::move(residualArgs[0]);

  return ComplexExpression("And"_, {}, std::move(residualArgs), {});
}

std::optional<boss::ExpressionArguments> computeResidualProjection(const Signature &viewParts,
                                                                   const Signature &queryParts) {
  if (viewParts.projectedColumns.empty() && queryParts.projectedColumns.empty())
    return std::nullopt;

  if (!viewParts.projectedColumns.empty() && !queryParts.projectedColumns.empty()) {
    if (viewParts.projectedColumns.size() == queryParts.projectedColumns.size())
      return std::nullopt;

    boss::ExpressionArguments residualArgs;
    for (const auto &[col, _] : queryParts.projectedColumns)
      residualArgs.push_back(Symbol(col));

    return std::move(residualArgs);
  }

  if (!viewParts.projectedColumns.empty() && queryParts.projectedColumns.empty()) {
    std::unordered_set<boss::Symbol> seenColumns;
    std::vector<boss::Symbol> orderedColumns;
    for (const auto &table : queryParts.baseTables) {
      const auto &entry = tableRegistry.find(table);
      if (entry == tableRegistry.end())
        return std::nullopt; // Shouldn't happen, but still bail safely
      for (const auto &col : entry->second.columns)
        if (seenColumns.insert(col).second)
          orderedColumns.push_back(col);
    }

    if (orderedColumns.size() == viewParts.projectedColumns.size())
      return std::nullopt; // View projects exactly the base columns,
                           // no residual projection needed

    boss::ExpressionArguments residualArgs;
    for (const auto &col : orderedColumns)
      residualArgs.push_back(Symbol(col));

    return std::move(residualArgs);
  }

  // if (viewParts.projectedColumns.empty() && !queryParts.projectedColumns.empty())
  boss::ExpressionArguments cols;
  for (const auto &[col, expr] : queryParts.projectedColumns)
    cols.push_back(Symbol(col));
  return std::move(cols);
}

// Returns true if `candidate` should replace current `best`.
static bool candidateViewRewriteComparator(boss::Symbol const &bestName, double bestScore,
                                           std::optional<double> bestCost,
                                           boss::Symbol const &candName, double candScore,
                                           std::optional<double> candCost) {
  bool cachedBest = viewCache.count(bestName) > 0;
  bool cachedCand = viewCache.count(candName) > 0;

  bool hasCostBest = bestCost.has_value();
  bool hasCostCand = candCost.has_value();

  if (!cachedBest && !cachedCand) {
    if (!hasCostBest && !hasCostCand)
      return candScore > bestScore; // if neither has cost info, nor is cached, select higher score
    if (!hasCostBest || !hasCostCand)
      return hasCostBest; // if only one has cost info, select the other one to gather its info
    // both have cost info — fall through to cost comparison
  } else if (cachedBest != cachedCand) {
    // if the score gap is large enough, select the higher score even if it's uncached
    double scoreGap = candScore - bestScore;
    if (std::abs(scoreGap) >= SCORE_GAP_OVERRIDES_CACHE_PREFERENCE)
      return scoreGap > 0;

    if (!hasCostBest || !hasCostCand)
      return cachedCand; // only one is cached, prefer the cached one
    // both cached-or-not with cost info — fall through to cost comparison
  }

  // Both have cost info and have the same cached status, so compare their effective costs and
  // select the cheaper one
  auto &bestEntry = viewRegistry.at(bestName);
  auto &candEntry = viewRegistry.at(candName);
  double effBest = cachedBest ? bestEntry.reuseCost : *bestCost;
  double effCand = cachedCand ? candEntry.reuseCost : *candCost;
  return effCand < effBest; // same cached status: lower effective cost wins
}

// Intersection of the views given by running a set of tables through the tableToViews index.
static std::unordered_set<boss::Symbol>
candidateViewsForTables(const std::unordered_set<boss::Symbol> &baseTables) {
  if (baseTables.empty()) {
    auto names = std::views::keys(viewRegistry);
    return std::unordered_set<boss::Symbol>(names.begin(), names.end());
  }

  std::unordered_set<boss::Symbol> candidates;
  bool first = true;
  for (const auto &table : baseTables) {
    auto it = tableToViews.find(table);
    if (it == tableToViews.end())
      return {};

    if (first) {
      candidates = it->second;
      first = false;
      continue;
    }

    std::erase_if(candidates, [&](const boss::Symbol &name) { return !it->second.count(name); });
  }

  return candidates;
}

std::optional<Expression> findRewriting(const Expression &query, ViewMetadata &queryMetadata,
                                        std::unordered_map<boss::Symbol, ViewMetadata> &cache,
                                        std::unordered_set<boss::Symbol> &seen) {
  auto *ce = std::get_if<ComplexExpression>(&query);
  if (!ce || !rewritableOperators.count(ce->getHead()))
    return std::nullopt;

  if (queryMetadata.sideEffect)
    return std::nullopt;

  std::optional<ViewMetadata> bestMatch;
  std::optional<boss::Symbol> bestName;
  std::optional<double> bestCost;
  double bestScore = -1.0;

  for (const auto &name : candidateViewsForTables(queryMetadata.signature.baseTables)) {
    auto &entry = viewRegistry.at(name);
    ViewMetadata viewMeta;
    viewMeta.signature = entry.signature;
    viewMeta.dependencies = entry.dependencies;
    seen.clear();
    expandSignature(viewMeta, cache, seen);

    double score = scoreView(viewMeta.signature, queryMetadata.signature);

    if (score > -1.0) {
      ageEntry(entry);
      entry.importanceFactor += score;
    }

    if (score >= 1.0 && viewCache.count(name)) {
      boss::ExpressionArguments args;
      args.push_back(Symbol(name));
      args.push_back("Defer"_);
      auto strategy = selectExecutionStrategy(entry);
      args.push_back(strategy == ExecutionStrategy::IsolatedMeasurement ? "IsolatedMeasurement"_
                                                                        : "Standard"_);
      Expression queryView = ComplexExpression("QueryView"_, {}, std::move(args), {});
      return queryView;
    }

    if (score <= -1.0)
      continue; // outright reject

    auto candCost = trueCost(name, true);
    if (!bestName ||
        candidateViewRewriteComparator(*bestName, bestScore, bestCost, name, score, candCost)) {
      bestName = name;
      bestScore = score;
      bestMatch = std::move(viewMeta);
      bestCost = candCost;
    }
  }

  // TODO: if the view already contains a filter or a project, then the residuals are currently
  // added on top where they could be merged into the existing ones
  if (bestMatch && bestName) {
    boss::ExpressionArguments args;
    args.push_back(Symbol(*bestName));
    args.push_back("Defer"_);
    auto strategy = selectExecutionStrategy(viewRegistry.at(*bestName));
    args.push_back(strategy == ExecutionStrategy::IsolatedMeasurement ? "IsolatedMeasurement"_
                                                                      : "Standard"_);
    Expression rewritten = ComplexExpression("QueryView"_, {}, std::move(args), {});

    if (auto residualFilter =
            computeResidualFilter(bestMatch->signature, queryMetadata.signature)) {
      boss::ExpressionArguments filterArgs;
      filterArgs.push_back(std::move(rewritten));
      filterArgs.push_back(std::move(*residualFilter));
      rewritten = ComplexExpression("Filter"_, {}, std::move(filterArgs), {});
    }

    if (auto residualArgs =
            computeResidualProjection(bestMatch->signature, queryMetadata.signature)) {
      boss::ExpressionArguments projectArgs;
      projectArgs.push_back(std::move(rewritten));
      for (auto &a : *residualArgs)
        projectArgs.push_back(std::move(a));
      rewritten = ComplexExpression("Project"_, {}, std::move(projectArgs), {});
    }

    return rewritten;
  }

  return std::nullopt;
}