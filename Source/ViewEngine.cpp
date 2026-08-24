#include "Cache.hpp"
#include "CachingProtocol.hpp"
#include "MetadataRegistry.hpp"
#include "ViewRegistry.hpp"

#include <unordered_set>
#include <utility>

using boss::expressions::CloneReason;

static Expression evaluate(Expression &&e, ViewMetadata *queryMetadata = nullptr,
                           bool skipRewrite = false, bool flatten = false, bool topLevel = false) {
  // Attempt to rewrite incoming query using view definitions top-down
  // Skip rewrite if incoming expression was generated from a evaluation of a QueryView
  ViewMetadata metadata;
  if (!skipRewrite) {
    // Extract expression metadata always
    walkView(e, metadata);

    // TODO: explore having a cross-query cache (goes together with the tableToViews TODO)
    // Cache for expanded view signatures
    std::unordered_map<boss::Symbol, ViewMetadata> cache;
    // Track seen views to avoid merging the same view signature multiple times
    std::unordered_set<boss::Symbol> seen;
    // Expand query signature in place - resolves uncached view references to their base tables
    expandSignature(metadata, cache, seen);

    if (topLevel) {
      queryMetadata = &metadata;
      queryMetadata->signature.extractAllReferencedColumns(queryMetadata->referencedColumns);
    }

    if (auto match = findRewriting(e, metadata, cache, seen)) {
      // Clear referenced columns if the view definition needs more than what the query needs
      // TODO: re-extract from view definition for proper pruning
      queryMetadata->referencedColumns.clear();
      queryMetadata->creditAwarded = true;
      return evaluate(std::move(*match), queryMetadata, true, flatten, topLevel);
    }
  }

  return std::visit(
      boss::utilities::overload(
          [queryMetadata, skipRewrite, flatten, topLevel](ComplexExpression &&ce) -> Expression {
            auto [head, statics, dynamics, spans] = std::move(ce).decompose();

            if (head == "DefineView"_) {
              if (!topLevel) {
                return Expression(false); // DefineView cannot be nested within other expressions
              }

              if (dynamics.size() != 2)
                return Expression(false); // DefineView requires exactly 2 arguments: a symbol for
                                          // the view name and an expression for the view definition

              auto *name = std::get_if<Symbol>(&dynamics[0]);
              if (!name)
                return Expression(false); // DefineView requires a symbol for the view name
              auto viewName = std::move(*name);

              // TODO: use metadata from the top instead of re-walking the view definition
              // TODO: block defining a view that queries a view that has not yet been defined
              ViewMetadata metadata;
              walkView(dynamics[1], metadata); // Walk the view expression to collect data for
                                               // dependency graph construction and validation

              if (metadata.sideEffect)
                // Block if definition has side effects such as defining or dropping views
                return Expression(false);

              std::unordered_set<boss::Symbol> visited;
              for (const auto &dep : metadata.dependencies) {
                if (hasCycle(viewName, dep, visited))
                  return Expression(false); // Block if definition creates a cycle
              }

              // Clear the entries in the indexes that contain the view that's being redefined
              if (auto existing = viewRegistry.find(viewName); existing != viewRegistry.end()) {
                if (viewCache.count(viewName)) {
                  viewCache.erase(viewName);
                  viewCacheOccupancy -= existing->second.size;
                }
                for (const auto &tableName : existing->second.expandedBaseTables)
                  tableToViews[tableName].erase(viewName);
                for (const auto &dep : existing->second.dependencies)
                  viewToViews[dep].erase(viewName);
              }

              std::unordered_set<boss::Symbol> seen;
              invalidateDependants(viewName, seen);

              // Clear the table index of any dependants that were invalidated, since their
              // expandedBaseTables will be recomputed below
              for (const auto &dependant : seen)
                for (const auto &tableName : viewRegistry.at(dependant).expandedBaseTables)
                  tableToViews[tableName].erase(dependant);

              // Recompute the expandedBaseTables for the view being defined
              std::unordered_set<boss::Symbol> expandVisited;
              auto newSelfExpanded = unionExpanded(metadata.signature.baseTables,
                                                   metadata.dependencies, expandVisited);
              expandVisited.insert(viewName);

              viewRegistry[viewName] = ViewEntry{std::move(dynamics[1]), metadata.dependencies,
                                                 newSelfExpanded, metadata.signature};

              // Update the indexes for the new view definition
              for (const auto &dep : metadata.dependencies)
                viewToViews[dep].insert(viewName);
              for (const auto &tableName : newSelfExpanded)
                tableToViews[tableName].insert(viewName);

              // Recompute the expandedBaseTables for all dependants that were invalidated, and
              // update the tableToViews index for them as well
              for (const auto &dependant : seen)
                for (const auto &tableName : expandBaseTables(dependant, expandVisited))
                  tableToViews[tableName].insert(dependant);

              return Expression(true);
            }

            if (head == "QueryView"_) {
              if (dynamics.size() < 1 || dynamics.size() > 3)
                throw std::runtime_error("QueryView requires 1 to 3 arguments");

              auto *name = std::get_if<Symbol>(&dynamics[0]);
              if (!name)
                throw std::runtime_error("QueryView first argument must be a symbol");

              auto viewName = std::move(*name);
              auto it = viewRegistry.find(viewName);
              if (it == viewRegistry.end())
                throw std::runtime_error("View not found: " + viewName.getName());

              if (evaluationStack.count(viewName))
                throw std::runtime_error("Circular view dependency detected: " +
                                         viewName.getName());

              ViewEntry &entry = it->second;

              evaluationStack.insert(viewName);
              struct EvaluationGuard { // RAII guard for evaluation stack cleanup
                boss::Symbol viewName;
                ~EvaluationGuard() { evaluationStack.erase(viewName); }
              } guard{viewName};

              if (flatten) {
                return evaluate(entry.definition.clone(CloneReason::EVALUATE_CONST_EXPRESSION),
                                queryMetadata, true, true);
              }

              if (dynamics.size() < 2)
                throw std::runtime_error(
                    "QueryView requires a caching decision (Admit, Reject, Defer) argument");

              auto decision = symbolToCachingDecision(std::get_if<Symbol>(&dynamics[1]));
              if (!decision)
                throw std::runtime_error(
                    "QueryView second argument must be a symbol: Admit, Reject, or Defer");

              std::optional<ExecutionStrategy> forcedStrategy = std::nullopt;
              if (dynamics.size() == 3) {
                auto *strat = std::get_if<Symbol>(&dynamics[2]);
                if (!strat)
                  throw std::runtime_error("QueryView third argument must be a symbol");
                forcedStrategy = symbolToExecutionStrategy(*strat);
                if (!forcedStrategy)
                  throw std::runtime_error("QueryView unknown execution strategy: " +
                                           strat->getName());
              }

              if (queryMetadata && queryMetadata->creditAwarded) {
                queryMetadata->creditAwarded = false;
              } else if (queryMetadata && !queryMetadata->creditAwarded) {
                // Non-rewritten QueryView, award credit to the view for reuse
                ageEntry(entry);
                entry.importanceFactor += 1.0;
              }

              // Only update the caching decision if it's not Reject and the current is not Admit
              if (*decision != CachingDecision::Reject &&
                  entry.cachingDecision != CachingDecision::Admit)
                entry.cachingDecision = *decision;

              // Invalidate the admission snapshot so that couldWinAdmission on future rewriting
              // candidates will be based on the latest view state.
              invalidateAdmissionSnapshot();

              // Short circuit repeated references to the same view in the same query
              // Read the cache registry to see the type of the view, and if it is Borrowed or
              // Pending so we correctly return the same type of cache ref for the same view
              if (auto existing = defaultCacheRegistry.find(viewName);
                  existing != defaultCacheRegistry.end()) {
                boss::ExpressionArguments cacheRefArgs;
                cacheRefArgs.emplace_back(viewName);
                cacheRefArgs.emplace_back(
                    existing->second.type == CacheEntryType::Borrowed ? "Borrowed"_ : "Pending"_);
                return Expression(ComplexExpression("CacheRef"_, {}, std::move(cacheRefArgs), {}));
              }

              // If cached before, avoid cloning by following borrowed cache protocol to move out
              // and move back in at the second pass of ViewEngine in the pipelines
              // TODO: allow the user to explicitly prefer recalculating the view instead of using
              // the cached value
              if (viewCache.count(viewName)) {
                if (*decision == CachingDecision::Reject)
                  // Serves as an eviction hint, but only works if every time the view is
                  // queried in the query, it is marked as rejected.
                  entry.cachingDecision = *decision;

                boss::ExpressionArguments cacheRefArgs;
                cacheRefArgs.emplace_back(viewName);
                cacheRefArgs.emplace_back("Borrowed"_);
                // If it is a top-level QueryView and a cache hit, then the result is just the value
                // of the cached view, and there is no further evaluation needed. Consequently, we
                // can just mark the view as Borrowed (for consistency with non-top-level cache
                // hits), but not move it out of the cache (set the value as a placeholder Symbol in
                // this case) and then in the second pass of VE we can just clone the cached value
                // as the return value or move it out of cache into the return value if it happens
                // that the view gets evicted from cache.
                Expression value = topLevel ? Expression(viewName) : std::move(viewCache[viewName]);
                if (!topLevel) {
                  viewCacheOccupancy -=
                      entry.size; // borrowed view is no longer occupying the cache
                  viewCache.erase(viewName);
                }
                defaultCacheRegistry[viewName] =
                    CacheEntry{std::move(value), CacheEntryType::Borrowed};

                return Expression(ComplexExpression("CacheRef"_, {}, std::move(cacheRefArgs), {}));
              }

              // Cache miss
              auto result = evaluate(entry.definition.clone(CloneReason::EVALUATE_CONST_EXPRESSION),
                                     queryMetadata, true);

              // Only inlines for this specific call to QueryView, if a subsequent call doesn't
              // reject a cache entry will still materialise it.
              if (*decision == CachingDecision::Reject)
                return evaluate(entry.definition.clone(CloneReason::EVALUATE_CONST_EXPRESSION),
                                queryMetadata, true, true);

              // Decide on execution strategy
              ExecutionStrategy strategy =
                  forcedStrategy.value_or((entry.computeCost > 0.0 || entry.materialiseCost > 0.0)
                                              ? ExecutionStrategy::Standard
                                              : ExecutionStrategy::IsolatedMeasurement);

              // If the strategy is IsolatedMeasurement, then inline the view's definition to avoid
              // CacheRefs to dependencies
              Expression entryValue =
                  strategy == ExecutionStrategy::IsolatedMeasurement
                      ? evaluate(entry.definition.clone(CloneReason::EVALUATE_CONST_EXPRESSION),
                                 queryMetadata, true, true)
                      : std::move(result);

              defaultCacheRegistry[viewName] = CacheEntry{
                  std::move(entryValue),
                  CacheEntryType::Pending,
                  strategy,
              };

              boss::ExpressionArguments cacheRefArgs;
              cacheRefArgs.emplace_back(viewName);
              cacheRefArgs.emplace_back("Pending"_);
              return Expression(ComplexExpression("CacheRef"_, {}, std::move(cacheRefArgs), {}));
            }

            if (head == "WithCaches"_) {
              if (!topLevel) {
                throw std::runtime_error("WithCaches can only be used at the top level");
              }

              auto finalExpr = unpackWithCaches(
                  Expression(ComplexExpression(std::move(head), std::move(statics),
                                               std::move(dynamics), std::move(spans))),
                  defaultCacheRegistry);

              // If the query was a top-level QueryView call that was a cache hit, then the
              // finalExpr is just the cache value of the view. If said view is being evicted,
              // we can just move it out of the cache and into the final result.
              // Otherwise, we just clone.
              std::optional<boss::Symbol> inPlaceName;

              // Phase 1: Collect all execution measurements
              for (auto &[name, entry] : defaultCacheRegistry) {
                auto it = viewRegistry.find(name);
                if (it == viewRegistry.end())
                  throw std::runtime_error("View not found for caching in WithCaches: " +
                                           name.getName());

                // View in registry and in cache --> top-level QueryView cache hit
                if (viewCache.count(name)) {
                  inPlaceName = name;
                  continue;
                }

                storeIfPositive(it->second.computeCost, entry.computeCost);
                storeIfPositive(it->second.materialiseCost, entry.materialiseCost);
                // Reuse cost is only relevant when reusing a view's value to compute a new result,
                // so should not be updated for a top-level QueryView cache hit
                storeIfPositive(it->second.reuseCost, entry.reuseCost);
                storeIfPositive(it->second.marginalComputeCost, entry.marginalComputeCost);
                it->second.size = computeSize(entry.value);
              }

              std::unordered_set<boss::Symbol> finalSet; // Views to remain/admit in cache

              // Skip the full admission process calculations if the top-level QueryView
              // was a cache hit, it has been set to Reject, and the cache is not full
              bool skipAdmissionProcess = false;
              if (inPlaceName) {
                auto &viewEntry = viewRegistry.at(*inPlaceName);
                bool rejected = viewEntry.cachingDecision == CachingDecision::Reject;
                double remainingOccupancy = viewCacheOccupancy - (rejected ? viewEntry.size : 0.0);

                // If after evicting the top-level cache hit view, the cache occupancy is still
                // larger than the cache size, we still have to go through admissions and evict
                // more.
                if (remainingOccupancy <= viewCacheSize) {
                  skipAdmissionProcess = true;
                  viewEntry.cachingDecision = CachingDecision::Defer;
                  for (auto const &[name, value] : viewCache)
                    finalSet.insert(name);
                  if (rejected)
                    finalSet.erase(*inPlaceName);
                }
              }

              if (!skipAdmissionProcess) {
                // Phase 2: Batch split - split this pass's candidates by decision
                std::vector<AdmissionCandidate> tier1, tier2;
                for (auto &[name, entry] : defaultCacheRegistry) {
                  auto &viewEntry = viewRegistry.at(name);
                  auto decision = viewEntry.cachingDecision;
                  viewEntry.cachingDecision = CachingDecision::Defer; // reset for next query

                  if (decision == CachingDecision::Reject)
                    continue; // discard rejected views

                  if (viewEntry.size <= 0.0)
                    continue; // never materialised as Table_ (still symbolic);
                              // don't admit unknown size views

                  // Fallback benefit to 0.0 if it cannot be computed, which shouldn't happen,
                  // but stay defensive
                  auto b = benefit(name, true).value_or(0.0);

                  // If both compute and reuse costs are known, and the reuse cost is somehow more
                  // expensive, it will result in a negative benefit score, which means that the
                  // view is not worth caching, so we skip it.
                  if (decision != CachingDecision::Admit && b < 0.0)
                    continue;

                  AdmissionCandidate candidate{name, viewEntry.size, b};
                  (decision == CachingDecision::Admit ? tier1 : tier2)
                      .push_back(std::move(candidate));
                }

                // Presently cached views compete with Defer candidates for eviction
                for (auto const &[cachedName, value] : viewCache) {
                  if (defaultCacheRegistry.count(cachedName))
                    continue; // skip views that have already been processed in this pass

                  auto b = benefit(cachedName, true).value_or(0.0);

                  // A view that was admitted in a previous query (due to an unknown reuse cost)
                  // that is now known to be more expensive to reuse than to recompute will have a
                  // negative benefit score and should be evicted.
                  if (b < 0.0)
                    continue;
                  tier2.push_back({cachedName, viewRegistry.at(cachedName).size, b});
                }

                // Sort tier1 and tier2 by benefit, descending
                auto benefitComparator = [](AdmissionCandidate const &a,
                                            AdmissionCandidate const &b) {
                  return a.benefit > b.benefit;
                };
                std::sort(tier1.begin(), tier1.end(), benefitComparator);
                std::sort(tier2.begin(), tier2.end(), benefitComparator);

                double usedSize = 0.0;

                auto buildCacheFrom = [&](std::vector<AdmissionCandidate> const &candidates) {
                  for (auto const &candidate : candidates) {
                    if (usedSize + candidate.size > viewCacheSize)
                      continue; // skip and try potentially smaller candidates with lower benefit
                    finalSet.insert(candidate.name);
                    usedSize += candidate.size;
                  }
                };
                buildCacheFrom(tier1); // admit all user-admitted views first
                buildCacheFrom(tier2); // then admit as many Defer views as possible
              }

              // Final result should be a materialised "Table"_ from ACE or a cached view
              Expression result;
              if (inPlaceName) {
                // If we have a top-level QueryView cache hit and it is being evicted, we move it
                // out of the cache and into the result, otherwise we clone it
                result =
                    finalSet.count(*inPlaceName)
                        ? viewCache.at(*inPlaceName).clone(CloneReason::EVALUATE_CONST_EXPRESSION)
                        : std::move(viewCache.at(*inPlaceName));
              } else {
                // If no top-level QueryView cache hit, the final result is just the ACE evaluated
                // expression
                result = std::move(finalExpr);
              }

              // Phase 3: Evict all views not in the final set, and admit new views that made it
              for (auto it = viewCache.begin(); it != viewCache.end();) {
                if (!finalSet.count(it->first)) {
                  viewCacheOccupancy -= viewRegistry.at(it->first).size;
                  it = viewCache.erase(it);
                } else {
                  ++it;
                }
              }
              for (auto &[name, entry] : defaultCacheRegistry) {
                if (inPlaceName && name == *inPlaceName)
                  continue; // Skip the top-level QueryView cache hit view
                if (finalSet.count(name)) {
                  viewCache[name] = std::move(entry.value);
                  viewCacheOccupancy += viewRegistry.at(name).size;
                }
              }

              defaultCacheRegistry.clear();
              return result;
            }

            if (head == "DropView"_) {
              if (!topLevel)
                return Expression(false); // DropView can only be used at the top level

              if (dynamics.size() != 1)
                return Expression(false); // DropView requires exactly 1 argument

              auto *name = std::get_if<Symbol>(&dynamics[0]);
              if (!name)
                return Expression(false); // DropView requires a symbol argument for the view name

              auto viewName = std::move(*name);
              if (evaluationStack.count(viewName))
                throw std::runtime_error("Cannot drop view currently being evaluated: " +
                                         viewName.getName());

              if (auto it = viewToViews.find(viewName);
                  it != viewToViews.end() && !it->second.empty())
                throw std::runtime_error("Cannot drop view " + viewName.getName() + ": " +
                                         it->second.begin()->getName() + " depends on it");

              auto it = viewRegistry.find(viewName);
              if (it == viewRegistry.end())
                return Expression(false); // Dropping a non-existent view fails gracefully

              // Cleanup indexes
              for (const auto &tableName : it->second.expandedBaseTables)
                tableToViews[tableName].erase(viewName);
              for (const auto &dep : it->second.dependencies)
                viewToViews[dep].erase(viewName);

              if (viewCache.count(viewName)) {
                viewCacheOccupancy -= it->second.size; // Remove old size from occupancy
                viewCache.erase(viewName);             // Invalidate the cache for the view itself
              }
              viewRegistry.erase(it);
              return Expression(true);
            }

            if (head == "ClearViews"_) {
              if (!topLevel)
                return Expression(false); // ClearViews can only be used at the top level

              if (!dynamics.empty())
                return Expression(false); // ClearViews does not take any arguments

              if (!evaluationStack.empty())
                throw std::runtime_error(
                    "Cannot clear views while " + std::to_string(evaluationStack.size()) +
                    " view(s) are being evaluated, e.g., " + evaluationStack.begin()->getName());

              viewRegistry.clear();
              tableToViews.clear();
              viewToViews.clear();
              viewCache.clear();
              viewCacheOccupancy = 0.0;
              return Expression(true);
            }

            if (head == "ListViews"_) {
              if (!dynamics.empty())
                throw std::runtime_error("ListViews does not take any arguments");

              // Sort views for deterministic output
              std::vector<std::pair<boss::Symbol, const ViewEntry *>> sortedViews;
              sortedViews.reserve(viewRegistry.size());
              for (const auto &[name, expr] : viewRegistry)
                sortedViews.emplace_back(name, &expr);
              std::sort(sortedViews.begin(), sortedViews.end(), [](const auto &a, const auto &b) {
                return a.first.getName() < b.first.getName();
              });

              boss::ExpressionArguments nameArgs;
              boss::ExpressionArguments defArgs;
              for (const auto &[name, entry] : sortedViews) {
                nameArgs.emplace_back(name);
                defArgs.emplace_back(entry->definition.clone(CloneReason::EXPRESSION_WRAPPING));
              }

              boss::ExpressionArguments columns;
              columns.emplace_back(ComplexExpression("Name"_, {}, std::move(nameArgs), {}));
              columns.emplace_back(ComplexExpression("Definition"_, {}, std::move(defArgs), {}));
              return Expression(ComplexExpression("ViewList"_, {}, std::move(columns), {}));
            }

            if (head == "Name"_) {
              if (dynamics.size() != 2)
                return Expression(false);

              auto *loadExpr = std::get_if<ComplexExpression>(&dynamics[0]);
              if (!loadExpr || loadExpr->getHead() != "Load"_)
                return Expression(false);

              auto const &loadArgs = loadExpr->getDynamicArguments();
              if (loadArgs.empty())
                return Expression(false);

              auto const *path = std::get_if<std::string>(&loadArgs[0]);
              auto const *tableName = std::get_if<Symbol>(&dynamics[1]);
              if (path && tableName) {
                std::vector<boss::Symbol> columns;
                bool valid = true;
                for (size_t i = 1; i < loadArgs.size(); ++i) {
                  auto const *col = std::get_if<Symbol>(&loadArgs[i]);
                  if (!col) {
                    valid = false;
                    break;
                  }
                  columns.push_back(*col);
                }
                if (valid)
                  registerTable(*tableName, *path, "", false, std::move(columns));
              }

              return Expression(ComplexExpression(std::move(head), std::move(statics),
                                                  std::move(dynamics), std::move(spans)));
            }

            if (head == "RegisterTable"_) {
              if (!topLevel)
                return Expression(false);

              if (dynamics.size() < 4)
                return Expression(false); // name, url, loaderPath, lazy required at minimum

              auto *name = std::get_if<Symbol>(&dynamics[0]);
              if (!name)
                return Expression(false);

              auto *url = std::get_if<std::string>(&dynamics[1]);
              if (!url)
                return Expression(false);

              auto *loaderPath = std::get_if<std::string>(&dynamics[2]);
              if (!loaderPath)
                return Expression(false);

              auto *lazy = std::get_if<bool>(&dynamics[3]);
              if (!lazy)
                return Expression(false);

              std::vector<boss::Symbol> columns;
              columns.reserve(dynamics.size() - 4);
              for (size_t i = 4; i < dynamics.size(); ++i) {
                auto *col = std::get_if<Symbol>(&dynamics[i]);
                if (!col)
                  return Expression(false);
                columns.push_back(std::move(*col));
              }

              return Expression(registerTable(std::move(*name), std::move(*url),
                                              std::move(*loaderPath), *lazy, std::move(columns)));
            }

            if (head == "DropTable"_) {
              if (!topLevel)
                return Expression(false);

              if (dynamics.size() != 1)
                return Expression(false);

              auto *name = std::get_if<Symbol>(&dynamics[0]);
              if (!name)
                return Expression(false);

              return Expression(dropTable(*name));
            }

            if (head == "ClearTables"_) {
              if (!topLevel)
                return Expression(false);

              if (!dynamics.empty())
                return Expression(false);

              clearTables();
              return Expression(true);
            }

            if (head == "ListTables"_) {
              if (!dynamics.empty())
                throw std::runtime_error("ListTables does not take any arguments");

              return listTables();
            }

            if (head == "SetCacheBudget"_) {
              if (!topLevel)
                return Expression(false);

              if (dynamics.size() != 1)
                return Expression(false); // SetCacheBudget requires exactly 1 argument: bytes

              double bytes;
              if (auto *d = std::get_if<double>(&dynamics[0]))
                bytes = *d;
              else if (auto *i = std::get_if<int64_t>(&dynamics[0]))
                bytes = static_cast<double>(*i);
              else
                return Expression(false);

              if (bytes <= 0.0)
                return Expression(false);

              viewCacheSize = bytes;
              return Expression(true);
            }

            if (head == "SetEngineMode"_) {
              if (!topLevel)
                return Expression(false);

              if (dynamics.size() != 1)
                return Expression(false); // SetEngineMode requires exactly 1 argument: Lite or Full

              auto *mode = std::get_if<Symbol>(&dynamics[0]);
              if (!mode)
                return Expression(false);

              if (*mode == "Lite"_)
                engineMode = EngineMode::Lite;
              else if (*mode == "Full"_)
                engineMode = EngineMode::Full;
              else
                return Expression(false);

              return Expression(true);
            }

            // Recursively evaluate arguments of other expressions
            // - Do not eval args of ViewList or TableList to preserve definitions for output
            // - Do not eval args of ByName to avoid symbol handler replacement leading to
            //   ByName(ByName(actualName))
            // - Do not eval args of Table_ for performance reasons
            if (head != "ViewList"_ && head != "TableList"_ && head != "ByName"_ &&
                head != "Table"_) {
              for (auto &arg : dynamics) {
                arg = evaluate(std::move(arg), queryMetadata, skipRewrite, flatten);
              }
            }

            return Expression(ComplexExpression(std::move(head), std::move(statics),
                                                std::move(dynamics), std::move(spans)));
          },
          [queryMetadata, flatten, topLevel](Symbol &&s) -> Expression {
            auto viewIt = viewRegistry.find(s);
            if (viewIt != viewRegistry.end()) {
              boss::ExpressionArguments queryViewArgs;
              queryViewArgs.emplace_back(std::move(s));

              if (!flatten) {
                auto strategy = selectExecutionStrategy(viewIt->second);
                queryViewArgs.emplace_back("Defer"_);
                queryViewArgs.emplace_back(strategy == ExecutionStrategy::IsolatedMeasurement
                                               ? Symbol("IsolatedMeasurement")
                                               : Symbol("Standard"));
              }

              auto rewritten =
                  Expression(ComplexExpression("QueryView"_, {}, std::move(queryViewArgs), {}));
              return evaluate(std::move(rewritten), queryMetadata, true, flatten, topLevel);
            }

            auto tableIt = tableRegistry.find(s);
            if (tableIt == tableRegistry.end())
              return Expression(std::move(s)); // Unknown symbol, return as-is

            auto const &entry = tableIt->second;

            if (!entry.lazy) {
              // Eagerly loaded table by ArrowComputeEngine, wrap in ByName operator
              boss::ExpressionArguments nameArgs;
              nameArgs.emplace_back(std::move(s));
              return Expression(ComplexExpression("ByName"_, {}, std::move(nameArgs), {}));
            }

            boss::ExpressionArguments colArgs;
            if (queryMetadata && !queryMetadata->referencedColumns.empty() &&
                !queryMetadata->signature.projectedColumns.empty()) {
              for (auto const &col : entry.columns)
                if (queryMetadata->referencedColumns.count(col))
                  // Follow schema ordering for columns in the Gather args
                  colArgs.emplace_back(col);
            }
            boss::ExpressionArguments gatherArgs;
            gatherArgs.emplace_back(entry.url);
            gatherArgs.emplace_back(entry.loaderPath);
            gatherArgs.emplace_back(ComplexExpression("Table"_, {}, {}, {}));
            gatherArgs.emplace_back(ComplexExpression("List"_, {}, std::move(colArgs), {}));

            return Expression(ComplexExpression("Gather"_, {}, std::move(gatherArgs), {}));
          },
          [](auto &&other) -> Expression { return Expression(std::move(other)); }),
      std::move(e));
}

extern "C" BOSSExpression *evaluate(BOSSExpression *e) {
  defaultCacheRegistry.clear();
  resolvedCacheRefs.clear();
  ++veTick;

  if (engineMode == EngineMode::Lite) {
    // skipRewrite + flatten: no rewriting, no caching - QueryView and bare-symbol
    // view references just substitute the stored definition, recursively.
    auto result = evaluate(std::move(e->delegate), nullptr, true, true, true);
    return new BOSSExpression{.delegate = std::move(result)};
  }

  auto result = evaluate(std::move(e->delegate), nullptr, false, false, true);

  if (defaultCacheRegistry.empty())
    return new BOSSExpression{.delegate = std::move(result)};
  return new BOSSExpression{
      .delegate = repackWithCaches(std::move(defaultCacheRegistry), std::move(result))};
}