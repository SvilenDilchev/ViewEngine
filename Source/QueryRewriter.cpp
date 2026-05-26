#include "QueryRewriter.hpp"
#include "ViewRegistry.hpp"
#include <unordered_set>

using boss::expressions::CloneReason;

static std::unordered_set<std::string> sideEffectOperators = {"DefineView", "DropView",
                                                              "ClearViews", "CacheView"};

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
                     if (sideEffectOperators.count(head.getName())) {
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

static void assignPredicateToSources(Expression &&pred, ViewMetadata &metadata,
                                     SourceSets &sources) {
  std::vector<Expression> leaves;
  flattenPredicates(std::move(pred), leaves, metadata.sideEffect);
  for (auto &leaf : leaves) {
    auto key = serializeExpr(leaf);
    auto shared = std::make_shared<Expression>(std::move(leaf));
    for (const auto &src : sources.first)
      metadata.signature.tablePredicates[src].emplace(
          key, shared); // Can't move because we need them for every source in the loop
    for (const auto &src : sources.second)
      metadata.signature.viewPredicates[src].emplace(key, shared);
  }
}

// Checks if a view join predicate is non-destructive with respect to
// the information that the query is interested in
static bool isSafeUnmatchedJoin(const JoinEdge &viewEdge, const Signature &queryParts) {
  if (viewEdge.joinType != "LeftJoin" && viewEdge.joinType != "AntiJoin")
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

void walkView(const Expression &expr, ViewMetadata &metadata, SourceSets &sources) {
  std::visit(boss::utilities::overload(
                 [&](const ComplexExpression &ce) {
                   if (metadata.sideEffect)
                     return; // Short circuit if we've already detected a side effect

                   const auto &head = ce.getHead();
                   const auto &dynamics = ce.getDynamicArguments();

                   if (head == "QueryView"_) {
                     if (dynamics.empty() || !std::get_if<Symbol>(&dynamics[0])) {
                       metadata.sideEffect = true; // Treat malformed QueryView as side effect
                       return;
                     }
                     const auto &viewName = std::get_if<Symbol>(&dynamics[0])->getName();
                     metadata.dependencies.insert(viewName);
                     metadata.signature.viewPredicates.try_emplace(viewName);
                     sources.second.insert(viewName);
                     return;
                   }

                   // Detect side prohibited effects
                   if (sideEffectOperators.count(head.getName())) {
                     metadata.sideEffect = true;
                     return;
                   }

                   if (head == "ByName"_) {
                     if (dynamics.empty() || !std::get_if<Symbol>(&dynamics[0])) {
                       metadata.sideEffect = true; // Treat malformed ByName as side effect since
                       return;
                     }
                     const auto &viewName = std::get_if<Symbol>(&dynamics[0])->getName();
                     metadata.signature.tablePredicates.try_emplace(viewName);
                     sources.first.insert(viewName);
                     return;
                   }

                   if (head == "Filter"_) {
                     if (dynamics.size() != 2) {
                       metadata.sideEffect = true; // Treat malformed Filter as side effect
                       return;
                     }

                     walkView(dynamics[0], metadata, sources);

                     // TODO: predicates are assigned to all sources in the subtree because we
                     // lack schema information to determine which table owns which column. This
                     // causes false positives in scoring — a view filtering orders on l_price
                     // would incorrectly pass condition 2 because the query's predicate map also
                     // has l_price on orders after the join. Fix: load table schemas and resolve
                     // column-to-table ownership before assignment.
                     assignPredicateToSources(dynamics[1].clone(CloneReason::EXPRESSION_WRAPPING),
                                              metadata, sources);
                     return;
                   }

                   if (head == "Project"_) {
                     if (dynamics.empty()) {
                       metadata.sideEffect = true; // Treat malformed Project as side effect
                       return;
                     }

                     walkView(dynamics[0], metadata, sources);
                     for (size_t i = 1; i < dynamics.size(); ++i) {
                       auto key = serializeExpr(dynamics[i]);
                       auto shared = std::make_shared<Expression>(
                           dynamics[i].clone(CloneReason::EXPRESSION_WRAPPING));
                       metadata.signature.projectedColumns.emplace(std::move(key),
                                                                   std::move(shared));
                     }
                     return;
                   }

                   if (head == "Join"_ || head == "LeftJoin"_ || head == "AntiJoin"_) {
                     if (dynamics.size() < 4) {
                       metadata.sideEffect = true; // Treat malformed Joins as side effect
                       return;
                     }

                     SourceSets leftSources, rightSources;
                     walkView(dynamics[0], metadata, leftSources);
                     walkView(dynamics[2], metadata, rightSources);

                     std::unordered_set<std::string> allLefts = leftSources.first;
                     allLefts.insert(leftSources.second.begin(), leftSources.second.end());
                     std::unordered_set<std::string> allRights = rightSources.first;
                     allRights.insert(rightSources.second.begin(), rightSources.second.end());

                     metadata.signature.joinEdges.push_back(
                         {std::move(allLefts), std::move(allRights),
                          dynamics[1].clone(CloneReason::EXPRESSION_WRAPPING),
                          dynamics[3].clone(CloneReason::EXPRESSION_WRAPPING), head.getName()});

                     sources.first.merge(leftSources.first);
                     sources.first.merge(rightSources.first);
                     sources.second.merge(leftSources.second);
                     sources.second.merge(rightSources.second);
                     return;
                   }

                   // TODO: handle aggregations
                   // For now it's with the passthroughs like ordering operators
                   if (head == "GroupBy"_ || head == "OrderBy"_ || head == "Slice"_) {
                     if (dynamics.empty()) {
                       // Treat malformed passthrough operators as side effect
                       metadata.sideEffect = true;
                     }
                     walkView(dynamics[0], metadata, sources);
                     return;
                   }

                   for (const auto &arg : dynamics) {
                     walkView(arg, metadata, sources);
                   }
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
    auto viewLeftKey = serializeExpr(viewEdge.leftKeys);
    auto viewRightKey = serializeExpr(viewEdge.rightKeys);
    bool matched = false;
    for (size_t i = 0; i < queryParts.joinEdges.size(); ++i) {
      const auto &queryEdge = queryParts.joinEdges[i];
      // TODO: join matching is purely syntactic — semantically equivalent joins
      // (e.g. different key ordering or aliased column names) are not detected.
      // Needs semantic join key comparison.
      if (viewEdge.joinType == queryEdge.joinType &&
          viewEdge.leftSources == queryEdge.leftSources &&
          viewEdge.rightSources == queryEdge.rightSources &&
          viewLeftKey == serializeExpr(queryEdge.leftKeys) &&
          viewRightKey == serializeExpr(queryEdge.rightKeys)) {
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
  }

  return W_TABLE * tableCoverage + W_PRED * predicateCoverage + W_JOIN * joinCoverage +
         W_PROJ * projectionCoverage;
}

std::optional<std::string> findRewriting(const Expression &query) {
  static const std::unordered_set<std::string> rewritableOperators = {"Filter", "Join", "LeftJoin",
                                                                      "AntiJoin", "Project"};

  // Only attempt rewriting if the top-level operator is one we support rewriting for
  auto *ce = std::get_if<ComplexExpression>(&query);
  if (!ce || !rewritableOperators.count(ce->getHead().getName()))
    return std::nullopt;

  // Step 1: extract query signature
  ViewMetadata queryMetadata;
  SourceSets querySources;
  walkView(query, queryMetadata, querySources);

  if (queryMetadata.sideEffect)
    return std::nullopt;

  // Step 2: find candidate views
  std::unordered_set<std::string> candidates;
  findCandidateViews(querySources.first, candidates);

  if (candidates.empty())
    return std::nullopt;

  // Step 3: score each candidate and return if perfect match found
  // TODO: consider non-perfect matches and extract remainder to apply on top of the view
  for (const auto &viewName : candidates) {
    auto it = viewRegistry.find(viewName);
    if (it == viewRegistry.end())
      continue;

    double score = scoreView(it->second.signature, queryMetadata.signature);

    if (score >= 1.0)
      return viewName;
  }
  return std::nullopt;
}