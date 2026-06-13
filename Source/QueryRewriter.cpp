#include "QueryRewriter.hpp"
#include "MetadataRegistry.hpp"
#include "ViewRegistry.hpp"
#include <unordered_set>

using boss::expressions::CloneReason;
using boss::utilities::operator""_;

static const std::unordered_set<boss::Symbol> sideEffectOperators = {"DefineView"_, "DropView"_,
                                                                     "ClearViews"_};

static const std::unordered_set<boss::Symbol> rewritableOperators = {
    "Filter"_, "Join"_, "LeftJoin"_, "AntiJoin"_, "Project"_};

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

// Flattens nested And predicates into a single vector of leaf predicates
static void flattenPredicates(Expression &&pred, std::vector<Expression> &out, bool &sideEffect) {
  std::visit(boss::utilities::overload(
                 [&](ComplexExpression &&ce) {
                   auto [head, statics, dynamics, spans] = std::move(ce).decompose();
                   if (head == "And"_) {
                     for (auto &arg : dynamics)
                       flattenPredicates(std::move(arg), out, sideEffect);
                   } else {
                     if (sideEffectOperators.count(head)) {
                       sideEffect = true;
                       return; // Block side effect operators in predicates
                     }
                     out.emplace_back(ComplexExpression(std::move(head), std::move(statics),
                                                        std::move(dynamics), std::move(spans)));
                   }
                 },
                 [&](auto &&other) { out.emplace_back(std::move(other)); }),
             std::move(pred));
}

static void assignPredicateToSources(Expression &&pred, ViewMetadata &metadata) {
  std::vector<Expression> leaves;
  flattenPredicates(std::move(pred), leaves, metadata.sideEffect);
  for (auto &leaf : leaves) {
    walkView(leaf, metadata); // Walk the leaf to find any referenced tables/views
    auto key = serializeExpr(leaf);
    auto shared = std::make_shared<Expression>(std::move(leaf));
    for (const auto &[src, _] : metadata.signature.tablePredicates)
      metadata.signature.tablePredicates[src].emplace(
          key, shared); // Can't move because we need them for every source in the loop
    for (const auto &[src, _] : metadata.signature.viewPredicates)
      metadata.signature.viewPredicates[src].emplace(key, shared);
  }
}

// Checks if a view join predicate is non-destructive with respect to
// the information that the query is interested in
static bool isSafeUnmatchedJoin(const JoinEdge &viewEdge, const Signature &queryParts) {
  if (viewEdge.joinType != JoinType::LEFT && viewEdge.joinType != JoinType::ANTI)
    return false; // If join is destructive on both sides (e.g., Inner Join)

  for (const auto &src : viewEdge.rightSources) {
    if (queryParts.tablePredicates.count(src) || queryParts.viewPredicates.count(src))
      return false; // If query cares about the source on the right (destructive side)

    for (const auto &queryEdge : queryParts.joinEdges)
      if (queryEdge.leftSources.count(src) || queryEdge.rightSources.count(src))
        return false; // If query has a join involving the view source on the right
  }
  return true;
}

// Returns a set of keys of a given map, used for snapshotting sources before walking into a subtree
static std::unordered_set<const boss::Symbol *> snapshotKeys(const auto &map) {
  std::unordered_set<const boss::Symbol *> snap;
  snap.reserve(map.size());
  for (auto &[k, _] : map)
    snap.insert(&k);
  return snap;
}

// Used to collect new sources added to the metadata signature after walking into a subtree, for
// join edge construction
static void collectNewKeys(const auto &map, const std::unordered_set<const boss::Symbol *> &before,
                           std::unordered_set<boss::Symbol> &out) {
  for (auto &[k, _] : map)
    if (!before.count(&k))
      out.insert(k);
}

// Expands uncached view sources down to their base tables for accurate comparisons
static void expandSignature(Signature &sig, std::unordered_map<boss::Symbol, Signature> &cache,
                            std::unordered_set<boss::Symbol> &seen) {
  auto it = sig.viewPredicates.begin();
  while (it != sig.viewPredicates.end()) {
    const auto &viewName = it->first;
    auto regIt = viewRegistry.find(viewName);
    if (regIt == viewRegistry.end() || regIt->second.cached.has_value() || seen.count(viewName)) {
      ++it;
      continue;
    }
    seen.insert(viewName);
    if (!cache.count(viewName)) {
      cache[viewName] = regIt->second.signature; // Cache signatures of views we've expanded to
                                                 // avoid redundant work and infinite recursion on
                                                 // cycles  expandSignature(cache[viewName], cache);
      expandSignature(cache[viewName], cache, seen);
    }
    auto &viewSig = cache[viewName];

    // merge tablePredicates
    for (auto &[table, preds] : viewSig.tablePredicates)
      for (auto &[key, expr] : preds)
        sig.tablePredicates[table].emplace(key, expr);

    // merge remaining viewPredicates (cached views referenced inside the expanded view)
    for (auto &[view, preds] : viewSig.viewPredicates)
      for (auto &[key, expr] : preds)
        sig.viewPredicates[view].emplace(key, expr);

    // expand view name in join edges
    for (auto &edge : sig.joinEdges) {
      bool wasLeft = edge.leftSources.erase(viewName);
      bool wasRight = edge.rightSources.erase(viewName);
      if (!wasLeft && !wasRight)
        continue;
      for (const auto &[table, _] : viewSig.tablePredicates) {
        if (wasLeft)
          edge.leftSources.insert(table);
        if (wasRight)
          edge.rightSources.insert(table);
      }
      for (const auto &[view, _] : viewSig.viewPredicates) {
        if (wasLeft)
          edge.leftSources.insert(view);
        if (wasRight)
          edge.rightSources.insert(view);
      }
    }

    for (auto &edge : viewSig.joinEdges)
      sig.joinEdges.push_back(edge);

    it = sig.viewPredicates.erase(it);
  }
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
              metadata.signature.viewPredicates.try_emplace(*viewName);
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

              metadata.signature.tablePredicates.try_emplace(*tableName);
              return;
            }

            if (head == "Filter"_) {
              if (dynamics.size() != 2) {
                metadata.sideEffect = true; // Treat malformed Filter as side effect
                return;
              }

              walkView(dynamics[0], metadata);

              // TODO: predicates are assigned to all sources in the subtree because we
              // lack schema information to determine which table owns which column. This
              // causes false positives in scoring — a view filtering orders on l_price
              // would incorrectly pass condition 2 because the query's predicate map also
              // has l_price on orders after the join. Fix: load table schemas and resolve
              // column-to-table ownership before assignment.
              assignPredicateToSources(dynamics[1].clone(CloneReason::EXPRESSION_WRAPPING),
                                       metadata);
              return;
            }

            if (head == "Project"_) {
              if (dynamics.empty()) {
                metadata.sideEffect = true; // Treat malformed Project as side effect
                return;
              }

              walkView(dynamics[0], metadata);
              for (size_t i = 1; i < dynamics.size(); ++i) {
                walkView(dynamics[i], metadata);
                auto key = serializeExpr(dynamics[i]);
                auto shared = std::make_shared<Expression>(
                    dynamics[i].clone(CloneReason::EXPRESSION_WRAPPING));
                metadata.signature.projectedColumns.emplace(std::move(key), std::move(shared));
              }
              return;
            }

            if (auto it = joinTypeMap.find(head); it != joinTypeMap.end()) {
              if (dynamics.size() < 2) {
                metadata.sideEffect = true; // Treat malformed Joins as side effect
                return;
              }

              JoinType parsedJoinType = it->second;
              std::unordered_set<boss::Symbol> allLefts, allRights;

              auto tablePointersBefore = snapshotKeys(metadata.signature.tablePredicates);
              auto viewPointersBefore = snapshotKeys(metadata.signature.viewPredicates);
              walkView(dynamics[0], metadata);
              collectNewKeys(metadata.signature.tablePredicates, tablePointersBefore, allLefts);
              collectNewKeys(metadata.signature.viewPredicates, viewPointersBefore, allLefts);

              auto tablePointersMid = snapshotKeys(metadata.signature.tablePredicates);
              auto viewPointersMid = snapshotKeys(metadata.signature.viewPredicates);
              walkView(dynamics[1], metadata);
              collectNewKeys(metadata.signature.tablePredicates, tablePointersMid, allRights);
              collectNewKeys(metadata.signature.viewPredicates, viewPointersMid, allRights);

              boss::ExpressionArguments leftKeysArgs, rightKeysArgs;
              for (size_t i = 2; i < dynamics.size(); ++i) {
                const auto *pred = std::get_if<ComplexExpression>(&dynamics[i]);
                if (pred && pred->getHead() == "Equal"_) {
                  const auto &args = pred->getDynamicArguments();
                  // TODO: this assumes equi-join predicates are always Equal(leftKey, rightKey),
                  // instead use schema information to determine which side each key belongs to.
                  if (args.size() == 2) {
                    leftKeysArgs.push_back(args[0].clone(CloneReason::EXPRESSION_WRAPPING));
                    rightKeysArgs.push_back(args[1].clone(CloneReason::EXPRESSION_WRAPPING));
                  }
                } else {
                  // It is a residual filter (or a boolean literal, etc.): assign to tables
                  assignPredicateToSources(dynamics[i].clone(CloneReason::EXPRESSION_WRAPPING),
                                           metadata);
                }
              }

              // TODO: before we just had the keys as dynamics[1] and dynamics[3],
              // this is a patch to not rewrite the serialiser and scorer
              // but we should properly parse the join keys using schema information
              Expression leftKeys = ComplexExpression("Keys"_, {}, std::move(leftKeysArgs));
              Expression rightKeys = ComplexExpression("Keys"_, {}, std::move(rightKeysArgs));

              metadata.signature.joinEdges.push_back(
                  {std::move(allLefts), std::move(allRights),
                   std::make_shared<Expression>(std::move(leftKeys)),
                   std::make_shared<Expression>(std::move(rightKeys)), parsedJoinType});
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
            if (tableRegistry.count(s))
              metadata.signature.tablePredicates.try_emplace(s);

            auto it = columnRegistry.find(s);
            if (it == columnRegistry.end())
              return;
            for (const auto &table : it->second)
              if (metadata.signature.tablePredicates.count(table))
                metadata.referencedTableColumns[table].insert(s);
          },
          [](const auto &) {}),
      expr);
}

double scoreView(const Signature &viewParts, const Signature &queryParts) {
  // Condition 1: view must share at least one table with the query
  size_t commonTables = 0;
  for (const auto &[table, _] : viewParts.tablePredicates)
    if (queryParts.tablePredicates.count(table))
      ++commonTables;

  if (commonTables == 0)
    return -1.0;

  double tableCoverage = (double)commonTables / (double)queryParts.tablePredicates.size();

  // Condition 2: the view must have weaker predicates than the query on shared tables
  size_t totalQueryPreds = 0;
  size_t coveredQueryPreds = 0;

  for (const auto &[table, viewPreds] : viewParts.tablePredicates) {
    auto qIt = queryParts.tablePredicates.find(table);
    // View applies predicates on a table the query doesn't reference;
    // any filtering here silently restricts the result the query sees.
    if (qIt == queryParts.tablePredicates.end()) {
      if (!viewPreds.empty())
        return -1.0;
      continue;
    }

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

  for (const auto &[table, queryPreds] : queryParts.tablePredicates)
    if (!viewParts.tablePredicates.count(table))
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
      // TODO: join matching is purely syntactic — semantically equivalent joins
      // (e.g. different key ordering or aliased column names) are not detected.
      // Needs semantic join key comparison.
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
    size_t matched = 0;
    for (const auto &[key, _] : queryParts.projectedColumns)
      if (viewParts.projectedColumns.count(key))
        ++matched;
    if (matched < queryParts.projectedColumns.size())
      return -1.0; // View does not cover all projected columns the query needs

    // Score is how many more columns the view projects on top of what the query needs,
    // as a fraction of the query's projections
    projectionCoverage =
        (double)queryParts.projectedColumns.size() / (double)viewParts.projectedColumns.size();
  } else if (!viewParts.projectedColumns.empty() && queryParts.projectedColumns.empty()) {
    // TODO: without schema information we cannot verify the view's projections cover
    // everything the query needs — pessimistically reject until schema is available.
    return -1.0;
  } else if (viewParts.projectedColumns.empty() && !queryParts.projectedColumns.empty()) {
    // View returns all base columns but query needs specific projected columns.
    // Without schema info we cannot verify coverage — pessimistically reject.
    return -1.0;
  }

  return W_TABLE * tableCoverage + W_PRED * predicateCoverage + W_JOIN * joinCoverage +
         W_PROJ * projectionCoverage;
}

std::optional<boss::Symbol> findRewriting(const Expression &query, ViewMetadata &queryMetadata) {
  // Only attempt rewriting if the top-level operator is one we support rewriting for
  auto *ce = std::get_if<ComplexExpression>(&query);
  if (!ce || !rewritableOperators.count(ce->getHead()))
    return std::nullopt;

  if (queryMetadata.sideEffect)
    return std::nullopt;

  // TODO: explore having a cross-query cache (goes together with the tableToViews TODO)
  // Cache for expanded view signatures
  std::unordered_map<boss::Symbol, Signature> cache;
  // Track seen views to avoid merging the same view signature multiple times
  std::unordered_set<boss::Symbol> seen;
  // Expand query signature in place - resolves uncached view references to their base tables
  expandSignature(queryMetadata.signature, cache, seen);

  // TODO: look into building a complete reverse index of base table -> top level views that
  // reference it at define time to nuke the search space instead of scanning all views
  for (const auto &[name, entry] : viewRegistry) {
    Signature viewSig = entry.signature;
    seen.clear();
    expandSignature(viewSig, cache, seen);

    double score = scoreView(viewSig, queryMetadata.signature);
    // TODO: consider non-perfect matches and extract remainder to apply on top of the view
    if (score >= 1.0)
      return name;
  }
  return std::nullopt;
}