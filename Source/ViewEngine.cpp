#include "AdmissionPolicy.hpp"
#include "CachingProtocol.hpp"
#include "MetadataRegistry.hpp"
#include "ViewRegistry.hpp"

#include <iostream>

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

              std::unordered_set<boss::Symbol> seen;
              invalidateDependentCaches(viewName, seen);

              // Remove old index entries if view already exists
              if (auto existing = viewRegistry.find(viewName); existing != viewRegistry.end()) {
                for (const auto &tableName : existing->second.signature.baseTables)
                  tableToViews[tableName].erase(viewName);
                for (const auto &dep : existing->second.dependencies)
                  viewToViews[dep].erase(viewName);
              }

              // Add new index entries
              // TODO: expand signature at define time and populate tableToViews with full
              // transitive base table set — would allow findRewriting to use tableToViews for
              // efficient candidate lookup instead of scanning the full registry. Same DFS pass
              // could replace hasCycle traversal above, doing cycle detection and index building in
              // one walk.
              for (const auto &tableName : metadata.signature.baseTables)
                tableToViews[tableName].insert(viewName);
              for (const auto &dep : metadata.dependencies)
                viewToViews[dep].insert(viewName);

              viewRegistry[std::move(viewName)] =
                  ViewEntry{std::nullopt, std::move(dynamics[1]), std::move(metadata.dependencies),
                            std::move(metadata.signature)};

              return Expression(true);
            }

            if (head == "QueryView"_) {
              if (dynamics.size() < 1 || dynamics.size() > 4)
                throw std::runtime_error("QueryView requires 1 to 4 arguments");

              auto *name = std::get_if<Symbol>(&dynamics[0]);
              if (!name)
                throw std::runtime_error("QueryView first argument must be a symbol");

              bool shouldCache = false;
              if (dynamics.size() >= 2) {
                auto *flag = std::get_if<bool>(&dynamics[1]);
                if (!flag)
                  throw std::runtime_error("QueryView second argument must be a boolean");
                shouldCache = *flag;
              }

              std::optional<ExecutionStrategy> forcedStrategy = std::nullopt;
              if (dynamics.size() >= 3) {
                auto *strat = std::get_if<Symbol>(&dynamics[2]);
                if (!strat)
                  throw std::runtime_error("QueryView third argument must be a symbol");
                forcedStrategy = symbolToExecutionStrategy(*strat);
                if (!forcedStrategy)
                  throw std::runtime_error("QueryView unknown execution strategy: " +
                                           strat->getName());
              }

              std::optional<IntegrityCheckMode> integrityMode = std::nullopt;
              if (dynamics.size() == 4) {
                auto *mode = std::get_if<Symbol>(&dynamics[3]);
                if (!mode)
                  throw std::runtime_error("QueryView fourth argument must be a symbol");
                integrityMode = symbolToIntegrityCheckMode(*mode);
                if (!integrityMode)
                  throw std::runtime_error("QueryView unknown integrity mode: " + mode->getName());
              }

              auto viewName = std::move(*name);
              auto it = viewRegistry.find(viewName);
              if (it == viewRegistry.end())
                throw std::runtime_error("View not found: " + viewName.getName());

              if (evaluationStack.count(viewName))
                throw std::runtime_error("Circular view dependency detected: " +
                                         viewName.getName());

              ViewEntry &entry = it->second;
              std::cerr << "[VE1-diag] QueryView(" << viewName.getName()
                        << ") entry.cached.has_value()=" << entry.cached.has_value() << "\n";

              if (flatten) {
                return evaluate(entry.definition.clone(CloneReason::EVALUATE_CONST_EXPRESSION),
                                queryMetadata, true, true);
              }

              if (queryMetadata && queryMetadata->creditAwarded) {
                queryMetadata->creditAwarded = false;
              } else if (queryMetadata && !queryMetadata->creditAwarded) {
                // Non-rewritten QueryView, award credit to the view for reuse
                ageEntry(entry);
                entry.importanceFactor += 1.0;
              }

              // If cached before, avoid cloning by following borrowed cache protocol to move out
              // and move back in at the second pass of ViewEngine in the pipelines
              // TODO: allow the user to explicitly prefer recalculating the view instead of using
              // the cached value
              if (entry.cached) {
                boss::ExpressionArguments cacheRefArgs;
                cacheRefArgs.emplace_back(viewName);
                cacheRefArgs.emplace_back("Borrowed"_);
                if (defaultCacheRegistry.count(viewName)) {
                  return Expression(
                      ComplexExpression("CacheRef"_, {}, std::move(cacheRefArgs), {}));
                }

                defaultCacheRegistry[viewName] =
                    CacheEntry{std::move(*entry.cached), CacheEntryType::Borrowed, integrityMode};
                entry.cached = std::nullopt;

                return Expression(ComplexExpression("CacheRef"_, {}, std::move(cacheRefArgs), {}));
              }

              evaluationStack.insert(viewName);
              struct EvaluationGuard { // RAII guard for evaluation stack cleanup
                boss::Symbol viewName;
                ~EvaluationGuard() { evaluationStack.erase(viewName); }
              } guard{viewName};

              // Cache miss
              auto result = evaluate(entry.definition.clone(CloneReason::EVALUATE_CONST_EXPRESSION),
                                     queryMetadata, true);

              // Mark the decision to cache for VE2 to handle
              entry.shouldCache = shouldCache;

              // Check if already registered to avoid duplicates
              if (!defaultCacheRegistry.count(viewName)) {
                ExecutionStrategy strategy =
                    forcedStrategy.value_or((entry.computeCost > 0.0 || entry.materialiseCost > 0.0)
                                                ? ExecutionStrategy::Standard
                                                : ExecutionStrategy::IsolatedMeasurement);

                Expression entryValue =
                    strategy == ExecutionStrategy::IsolatedMeasurement
                        ? evaluate(entry.definition.clone(CloneReason::EVALUATE_CONST_EXPRESSION),
                                   queryMetadata, true, true)
                        : std::move(result);

                defaultCacheRegistry[viewName] = CacheEntry{
                    std::move(entryValue),
                    CacheEntryType::Pending,
                    integrityMode,
                    strategy,
                };
              }

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

              for (auto &[name, entry] : defaultCacheRegistry) {
                auto it = viewRegistry.find(name);
                if (it == viewRegistry.end())
                  throw std::runtime_error("View not found for caching in WithCaches: " +
                                           name.getName());

                if (!it->second.cached) {
                  storeIfPositive(it->second.computeCost, entry.computeCost);
                  storeIfPositive(it->second.materialiseCost, entry.materialiseCost);
                  storeIfPositive(it->second.reuseCost, entry.reuseCost);
                  storeIfPositive(it->second.marginalComputeCost, entry.marginalComputeCost);
                  it->second.size = computeSize(entry.value);
                  std::cerr << "[VE2] " << name.getName() << " write-back (WithCaches loop): "
                            << "compute=" << it->second.computeCost
                            << " materialise=" << it->second.materialiseCost
                            << " reuse=" << it->second.reuseCost
                            << " marginalCompute=" << it->second.marginalComputeCost << " strategy="
                            << (entry.executionStrategy == ExecutionStrategy::IsolatedMeasurement
                                    ? "IsolatedMeasurement"
                                    : "Standard")
                            << " size=" << it->second.size << "\n";

                  if (!it->second.admissionDecided) {
                    // TODO: actual benefit cost analysis leading to a decision
                    it->second.shouldCache = true;
                    it->second.admissionDecided = true;
                  }

                  if (entry.type == CacheEntryType::Borrowed || it->second.shouldCache)
                    it->second.cached = evaluate(std::move(entry.value), queryMetadata, true);
                }

                std::cerr << "[VE2-diag] " << name.getName()
                          << " cached.has_value()=" << it->second.cached.has_value() << "\n";
              }
              // Evaluate with the final expression as a top-level expression, so that a CacheRef
              // operator will be correctly handled and cached
              auto result = evaluate(std::move(finalExpr), queryMetadata, true);
              defaultCacheRegistry.clear();

              return result;
            }

            // Handle leftover CacheRef operators in the pipeline on second pass
            if (head == "CacheRef"_) {
              if (dynamics.size() != 2)
                throw std::runtime_error("CacheRef requires exactly 2 arguments");

              auto *name = std::get_if<Symbol>(&dynamics[0]);
              if (!name)
                throw std::runtime_error("CacheRef first argument must be a symbol");

              if (!std::get_if<Symbol>(&dynamics[1]))
                throw std::runtime_error("CacheRef second argument must be a symbol");

              auto viewName = std::move(*name);
              auto it = viewRegistry.find(viewName);
              if (it == viewRegistry.end())
                throw std::runtime_error("CacheRef could not be resolved, view not found: " +
                                         viewName.getName());

              if (!it->second.cached) {
                auto regIt = defaultCacheRegistry.find(viewName);
                if (regIt == defaultCacheRegistry.end())
                  throw std::runtime_error(
                      "CacheRef could not be resolved, entry not found in cache registry: " +
                      viewName.getName());

                storeIfPositive(it->second.computeCost, regIt->second.computeCost);
                storeIfPositive(it->second.materialiseCost, regIt->second.materialiseCost);
                storeIfPositive(it->second.reuseCost, regIt->second.reuseCost);
                storeIfPositive(it->second.marginalComputeCost, regIt->second.marginalComputeCost);
                it->second.size = computeSize(regIt->second.value);
                std::cerr << "[VE2] " << viewName.getName() << " write-back (CacheRef fallback): "
                          << "compute=" << it->second.computeCost
                          << " materialise=" << it->second.materialiseCost
                          << " reuse=" << it->second.reuseCost
                          << " marginalCompute=" << it->second.marginalComputeCost
                          << " size=" << it->second.size << "\n";

                if (!it->second.admissionDecided) {
                  // TODO: actual benefit cost analysis leading to a decision
                  it->second.shouldCache = true;
                  it->second.admissionDecided = true;
                }

                if (regIt->second.type != CacheEntryType::Borrowed && !it->second.shouldCache)
                  return evaluate(std::move(regIt->second.value), queryMetadata, true);

                it->second.cached = evaluate(std::move(regIt->second.value), queryMetadata, true);
              }

              // Return cached value for final result
              return it->second.cached->clone(CloneReason::EVALUATE_CONST_EXPRESSION);
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
              for (const auto &tableName : it->second.signature.baseTables)
                tableToViews[tableName].erase(viewName);
              for (const auto &dep : it->second.dependencies)
                viewToViews[dep].erase(viewName);

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

            // Recursively evaluate arguments of other expressions
            // Do not eval args to preserve definitions for output
            // Check is here to block eval on second pass through ViewEngine in pipeline
            if (head != "ViewList"_ && head != "TableList"_ && head != "ByName"_) {
              for (auto &arg : dynamics) {
                arg = evaluate(std::move(arg), queryMetadata, skipRewrite, flatten);
              }
            }

            return Expression(ComplexExpression(std::move(head), std::move(statics),
                                                std::move(dynamics), std::move(spans)));
          },
          [queryMetadata, flatten](Symbol &&s) -> Expression {
            auto viewIt = viewRegistry.find(s);
            if (viewIt != viewRegistry.end()) {
              boss::ExpressionArguments queryViewArgs;
              queryViewArgs.emplace_back(std::move(s));

              if (!flatten) {
                auto decision = resolveAdaptiveCaching(viewIt->second);
                queryViewArgs.emplace_back(decision.shouldCache);
                queryViewArgs.emplace_back(decision.forcedStrategy ==
                                                   ExecutionStrategy::IsolatedMeasurement
                                               ? Symbol("IsolatedMeasurement")
                                               : Symbol("Standard"));
              }

              auto rewritten =
                  Expression(ComplexExpression("QueryView"_, {}, std::move(queryViewArgs), {}));
              return evaluate(std::move(rewritten), queryMetadata, true, flatten);
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
            if (!queryMetadata->referencedColumns.empty() &&
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
  ++veTick;
  auto result = evaluate(std::move(e->delegate), nullptr, false, false, true);

  if (defaultCacheRegistry.empty())
    return new BOSSExpression{.delegate = std::move(result)};
  return new BOSSExpression{
      .delegate = repackWithCaches(std::move(defaultCacheRegistry), std::move(result))};
}