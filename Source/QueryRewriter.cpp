#include "QueryRewriter.hpp"
#include "MetadataRegistry.hpp"
#include "ViewRegistry.hpp"
#include <unordered_map>
#include <unordered_set>

using boss::expressions::CloneReason;
using boss::utilities::operator""_;

static const std::unordered_set<boss::Symbol> sideEffectOperators = {"DefineView"_, "DropView"_,
                                                                     "ClearViews"_};

static const std::unordered_set<boss::Symbol> rewritableOperators = {
    "Filter"_, "Join"_, "LeftJoin"_, "AntiJoin"_, "Project"_};

// Used to canonicalise predicates for comparisons and scoring
// Greater(a, 5) and Less(5, a) are equivalent
static const std::unordered_set<boss::Symbol> strictComparisonOperators = {
    "Between"_, "Like"_, "Match_Substring"_, "IsValid"_};

static const std::unordered_map<boss::Symbol, boss::Symbol> flipComparisonOperators = {
    {"Equal"_, "Equal"_},  {"NotEqual"_, "NotEqual"_},
    {"Greater"_, "Less"_}, {"GreaterEqual"_, "LessEqual"_},
    {"Less"_, "Greater"_}, {"LessEqual"_, "GreaterEqual"_}};

// Map for parsing BOSS symbols into JoinType enum values
static const std::unordered_map<boss::Symbol, JoinType> joinTypeMap = {
    {"Join"_, JoinType::INNER}, {"LeftJoin"_, JoinType::LEFT}, {"AntiJoin"_, JoinType::ANTI}};

// TODO: canonicalise serialised predicates (e.g., Equal(a, b) vs Equal(b, a))
static std::string serializeExpr(const Expression &expr) {
  return std::visit(boss::utilities::overload(
                        [](const ComplexExpression &ce) {
                          std::string result = ce.getHead().getName() + "(";
                          bool first = true;
                          for (const auto &arg : ce.getDynamicArguments()) {
                            if (!first)
                              result += ",";
                            result += serializeExpr(arg);
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
static void extractColumnsFromExpr(const Expression &expr, std::unordered_set<boss::Symbol> &out) {
  std::visit(boss::utilities::overload(
                 [&](const Symbol &s) {
                   if (columnRegistry.find(s) != columnRegistry.end())
                     out.insert(s);
                 },
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
  for (const auto &[col, expr] : projectedColumns) {
    if (expr)
      extractColumnsFromExpr(*expr, out);
    else
      out.insert(col);
  }
}

void ViewMetadata::merge(ViewMetadata &&other) {
  dependencies.merge(other.dependencies);
  sideEffect = sideEffect || other.sideEffect;
  signature.baseTables.merge(other.signature.baseTables);
  for (auto &[col, preds] : other.signature.columnPredicates)
    signature.columnPredicates[col].merge(preds);
  for (auto &[col, expr] : other.signature.projectedColumns) {
    auto it = signature.projectedColumns.find(col);
    if (it != signature.projectedColumns.end()) {
      if (serializeExpr(*it->second) != serializeExpr(*expr)) {
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

void walkView(const Expression &expr, ViewMetadata &metadata) {
  std::visit(
      boss::utilities::overload(
          [&](const ComplexExpression &ce) {
            if (metadata.sideEffect)
              return; // Short circuit if we've already detected a side effect

            const auto &head = ce.getHead();
            const auto &dynamics = ce.getDynamicArguments();

            if (head == "QueryView"_) {
              // Treat malformed QueryView as side effect
              if (dynamics.empty() || dynamics.size() > 3) {
                metadata.sideEffect = true;
                return;
              }

              const auto *viewName = std::get_if<Symbol>(&dynamics[0]);
              if (!viewName) {
                metadata.sideEffect = true;
                return;
              }

              if (dynamics.size() >= 2 && !std::get_if<bool>(&dynamics[1])) {
                metadata.sideEffect = true;
                return;
              }
              if (dynamics.size() == 3 && !std::get_if<Symbol>(&dynamics[2])) {
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

              metadata.merge(std::move(leftMeta));
              metadata.merge(std::move(rightMeta));

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
                auto aFirst = serializeExpr(a.first);
                auto bFirst = serializeExpr(b.first);
                if (aFirst != bFirst)
                  return aFirst < bFirst;
                return serializeExpr(a.second) < serializeExpr(b.second);
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

            // Predicate conjunction operators just recurse down into the predicates
            if (head == "And"_) {
              if (dynamics.size() < 2) {
                metadata.sideEffect = true; // Malformed And is treated as side effect
                return;
              }

              for (const auto &arg : dynamics)
                walkView(arg, metadata);
              return;
            }

            if (flipComparisonOperators.count(head)) {
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

              // Both symbols — intra-table column comparison, no flip needed
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
                auto key = serializeExpr(*shared);

                metadata.signature.columnPredicates[colSym0].emplace(key, shared);
                metadata.signature.columnPredicates[colSym1].emplace(std::move(key),
                                                                     std::move(shared));
                return;
              }

              // One symbol, one literal — canonicalise so column is always on the left
              bool needsFlip = arg1IsSym && !arg0IsSym;

              auto colSym = std::get<Symbol>(needsFlip ? dynamics[1] : dynamics[0]);

              // Again skip check, see comment about malformation checks above
              //  if (columnRegistry.find(colSym) == columnRegistry.end()) {
              //    metadata.sideEffect = true;
              //    return;
              //  }

              if (needsFlip) {
                boss::Symbol canonicalHead = flipComparisonOperators.at(head);
                boss::ExpressionArguments canonArgs;
                canonArgs.push_back(dynamics[1].clone(CloneReason::EXPRESSION_WRAPPING));
                canonArgs.push_back(dynamics[0].clone(CloneReason::EXPRESSION_WRAPPING));
                ComplexExpression canonicalExpr(canonicalHead, {}, std::move(canonArgs), {});
                auto shared = std::make_shared<Expression>(std::move(canonicalExpr));
                auto key = serializeExpr(*shared);
                metadata.signature.columnPredicates[colSym].emplace(std::move(key),
                                                                    std::move(shared));
              } else {
                auto shared =
                    std::make_shared<Expression>(ce.clone(CloneReason::EXPRESSION_WRAPPING));
                auto key = serializeExpr(*shared);
                metadata.signature.columnPredicates[colSym].emplace(std::move(key),
                                                                    std::move(shared));
              }
              return;
            }

            // Strict First-Argument Operators
            if (strictComparisonOperators.count(head)) {
              if (dynamics.empty() || !std::holds_alternative<Symbol>(dynamics[0])) {
                metadata.sideEffect = true;
                return;
              }

              auto colSym = std::get<Symbol>(dynamics[0]);

              // Again skip check, see comment re: the same check in flipComparisonOperators
              //  if (columnRegistry.find(colSym) == columnRegistry.end()) {
              //    metadata.sideEffect = true;
              //    return;
              //  }

              auto shared =
                  std::make_shared<Expression>(ce.clone(CloneReason::EXPRESSION_WRAPPING));
              auto key = serializeExpr(*shared);

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

double scoreView(const Signature &viewParts, const Signature &queryParts) {
  // Condition 1: view must share at least one table with the query
  size_t commonTables = 0;
  for (const auto &table : viewParts.baseTables)
    if (queryParts.baseTables.count(table))
      ++commonTables;

  if (commonTables == 0)
    return -1.0;

  double tableCoverage = (double)commonTables / (double)queryParts.baseTables.size();

  // Condition 2: the view must have weaker predicates than the query on shared tables
  size_t totalQueryPreds = 0;
  size_t coveredQueryPreds = 0;

  for (const auto &[col, viewPreds] : viewParts.columnPredicates) {
    auto qIt = queryParts.columnPredicates.find(col);
    // View applies predicates on a column the query doesn't;
    // any filtering here silently restricts the result the query sees.
    // TODO: Semantic reasoning needed with knowledge of the actual data
    // if a predicate does not affect the data at all (e.g. Greater(l_price, -1) when l_price is
    // always positive) then this is not a coverage failure
    if (qIt == queryParts.columnPredicates.end())
      return -1.0;

    const auto &queryPreds = qIt->second;
    for (const auto &[key, _] : viewPreds)
      // TODO: this is a syntactic check only — semantically weaker predicates in the view
      // (e.g. Greater(l_price, 50) when query has Greater(l_price, 100)) are incorrectly
      // rejected. Needs semantic predicate strength comparison.
      if (!queryPreds.count(key))
        return -1.0;

    // TODO: when semantic predicate comparison is implemented, we should give partial credit
    // for weaker view predicates that still cover some of the query predicates, update our return
    // tell us exactly how much more should be added on top of the query
    totalQueryPreds += queryPreds.size();
    for (const auto &[key, _] : queryPreds)
      if (viewPreds.count(key))
        ++coveredQueryPreds;
  }

  for (const auto &[col, queryPreds] : queryParts.columnPredicates)
    if (!viewParts.columnPredicates.count(col))
      totalQueryPreds += queryPreds.size();

  double predicateCoverage =
      (totalQueryPreds > 0) ? (double)coveredQueryPreds / (double)totalQueryPreds : 1.0;

  // Condition 3: the join predicates of the view are non-destructive to the query's join
  // predicates
  std::unordered_set<size_t> matchedQueryJoinIndices;
  for (const auto &viewEdge : viewParts.joinEdges) {
    auto viewLeftKey = serializeExpr(*viewEdge.leftKeys);
    auto viewRightKey = serializeExpr(*viewEdge.rightKeys);
    bool matched = false;
    for (size_t i = 0; i < queryParts.joinEdges.size(); ++i) {
      const auto &queryEdge = queryParts.joinEdges[i];
      // TODO: join matching still doesn't detect joins that are semantically equivalent via
      // column aliasing (e.g. Equal(A.customer_id, ...) vs Equal(A.cust_id, ...) referring to
      // the same underlying column under a different name). Needs schema-aware alias resolution.
      if (viewEdge.joinType == queryEdge.joinType &&
          viewEdge.leftSources == queryEdge.leftSources &&
          viewEdge.rightSources == queryEdge.rightSources &&
          viewLeftKey == serializeExpr(*queryEdge.leftKeys) &&
          viewRightKey == serializeExpr(*queryEdge.rightKeys)) {
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

  // Condition 4: the view must project at least all the columns the query projects
  double projectionCoverage = 1.0;
  if (!viewParts.projectedColumns.empty() && !queryParts.projectedColumns.empty()) {
    for (const auto &[col, queryExpr] : queryParts.projectedColumns) {
      auto it = viewParts.projectedColumns.find(col);
      if (it == viewParts.projectedColumns.end()) {
        // TODO: check if any view projection has the same underlying expression but a different
        // name — if so, a rename Project on top of the view could satisfy the query (partial match)
        return -1.0;
      }
      if (it->second && queryExpr && serializeExpr(*it->second) != serializeExpr(*queryExpr))
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
      else if (!viewColumns.count(col))
        return -1.0; // View doesn't project a column the query needs
    }

    projectionCoverage = (double)queryParts.projectedColumns.size() / (double)viewColumns.size();
  }

  return W_TABLE * tableCoverage + W_PRED * predicateCoverage + W_JOIN * joinCoverage +
         W_PROJ * projectionCoverage;
}

std::optional<boss::Symbol> findRewriting(const Expression &query, ViewMetadata &queryMetadata,
                                          std::unordered_map<boss::Symbol, ViewMetadata> &cache,
                                          std::unordered_set<boss::Symbol> &seen) {
  auto *ce = std::get_if<ComplexExpression>(&query);
  if (!ce || !rewritableOperators.count(ce->getHead()))
    return std::nullopt;

  if (queryMetadata.sideEffect)
    return std::nullopt;

  // TODO: look into building a complete reverse index of base table -> top level views that
  // reference it at define time to nuke the search space instead of scanning all views
  for (const auto &[name, entry] : viewRegistry) {
    ViewMetadata viewMeta;
    viewMeta.signature = entry.signature;
    viewMeta.dependencies = entry.dependencies;
    seen.clear();
    expandSignature(viewMeta, cache, seen);

    double score = scoreView(viewMeta.signature, queryMetadata.signature);
    // TODO: consider non-perfect matches and extract remainder to apply on top of the view
    if (score >= 1.0)
      return name;
  }
  return std::nullopt;
}