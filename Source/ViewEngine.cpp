#include "CachingProtocol.hpp"
#include "ViewRegistry.hpp"

using boss::expressions::CloneReason;

static Expression evaluate(Expression &&e, bool skipRewrite = false, bool topLevel = false) {
  // Attempt to rewrite incoming query using view definitions top-down
  // Skip rewrite if incoming expression was generated from a evaluation of a QueryView
  if (!skipRewrite) {
    if (auto match = findRewriting(e)) {
      boss::ExpressionArguments args;
      args.emplace_back(Symbol(*match));
      return evaluate(Expression(ComplexExpression("QueryView"_, {}, std::move(args), {})),
                      skipRewrite, topLevel);
    }
  }

  return std::visit(
      boss::utilities::overload(
          [skipRewrite, topLevel](ComplexExpression &&ce) -> Expression {
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

              auto viewName = name->getName();
              ViewMetadata metadata;
              SourceSets sources;
              walkView(dynamics[1], metadata,
                       sources); // Walk the view expression to collect data for
                                 // dependency graph construction and validation

              if (metadata.sideEffect)
                // Block if definition has side effects such as defining or dropping views
                return Expression(false);

              std::unordered_set<std::string> visited;
              for (const auto &dep : metadata.dependencies) {
                if (hasCycle(viewName, dep, visited))
                  return Expression(false); // Block if definition creates a cycle
              }

              std::unordered_set<std::string> seen;
              invalidateDependentCaches(viewName, seen);

              // Remove old index entries if view already exists
              if (auto existing = viewRegistry.find(viewName); existing != viewRegistry.end()) {
                for (const auto &[tableName, _] : existing->second.signature.tablePredicates)
                  tableToViews[tableName].erase(viewName);
                for (const auto &dep : existing->second.dependencies)
                  viewToViews[dep].erase(viewName);
              }

              // Add new index entries
              for (const auto &[tableName, _] : metadata.signature.tablePredicates)
                tableToViews[tableName].insert(viewName);
              for (const auto &dep : metadata.dependencies)
                viewToViews[dep].insert(viewName);

              viewRegistry[viewName] =
                  ViewEntry{std::nullopt, std::move(dynamics[1]), std::move(metadata.dependencies),
                            std::move(metadata.signature)};

              return Expression(true);
            }

            if (head == "QueryView"_) {
              if (dynamics.size() < 1 || dynamics.size() > 2)
                throw std::runtime_error("QueryView requires 1 or 2 arguments");

              auto *name = std::get_if<Symbol>(&dynamics[0]);
              if (!name)
                throw std::runtime_error("QueryView first argument must be a symbol");

              bool shouldCache = false;
              if (dynamics.size() == 2) {
                auto *flag = std::get_if<bool>(&dynamics[1]);
                if (!flag)
                  throw std::runtime_error("QueryView second argument must be a boolean");
                shouldCache = *flag;
              }

              auto viewName = name->getName();
              auto it = viewRegistry.find(viewName);
              if (it == viewRegistry.end())
                throw std::runtime_error("View not found: " + name->getName());

              if (evaluationStack.count(viewName))
                throw std::runtime_error("Circular view dependency detected: " + viewName);

              ViewEntry &entry = it->second;
              if (entry.cached)
                // Return cached result if available
                return entry.cached->clone(CloneReason::EVALUATE_CONST_EXPRESSION);

              evaluationStack.insert(viewName);
              struct EvaluationGuard { // RAII guard for evaluation stack cleanup
                std::string viewName;
                ~EvaluationGuard() { evaluationStack.erase(viewName); }
              } guard{viewName};

              // Cache miss - wrap, evaluate, and pass through to other engines
              // Second pass will unwrap and save to cache
              auto result =
                  evaluate(entry.definition.clone(CloneReason::EVALUATE_CONST_EXPRESSION), true);

              if (!shouldCache)
                return std::move(result); // Return without caching

              // If nested call, return a CacheRef to be resolved by handled by subsequent engines
              if (!topLevel) {
                // Check if already registered to avoid duplicates
                auto it = std::find_if(
                    defaultPendingCacheRegistry.begin(), defaultPendingCacheRegistry.end(),
                    [&viewName](auto const &entry) { return entry.first == viewName; });
                if (it == defaultPendingCacheRegistry.end()) {
                  defaultPendingCacheRegistry.emplace_back(viewName, std::move(result));
                  // No need to update te lookup since it never gets used in ViewEngine
                }
                boss::ExpressionArguments cacheRefArgs;
                cacheRefArgs.emplace_back(Symbol(viewName));
                return Expression(ComplexExpression("CacheRef"_, {}, std::move(cacheRefArgs), {}));
              }
              // Cache the evaluated result for cases where only one instance
              // of ViewEngine is present and noone can consume the CacheView wrapper
              entry.cached = result.clone(CloneReason::EXPRESSION_WRAPPING);

              boss::expressions::ExpressionArguments cacheArgs;
              cacheArgs.emplace_back(Symbol(viewName));
              cacheArgs.emplace_back(std::move(result));
              return Expression(ComplexExpression("CacheView"_, {}, std::move(cacheArgs), {}));
            }

            if (head == "WithPendingCaches"_) {
              if (!topLevel) {
                throw std::runtime_error("WithPendingCaches can only be used at the top level");
              }

              auto finalExpr = unpackWithPendingCaches(
                  Expression(ComplexExpression(std::move(head), std::move(statics),
                                               std::move(dynamics), std::move(spans))),
                  defaultPendingCacheRegistry, defaultPendingCacheLookup);

              for (auto &[name, expr] : defaultPendingCacheRegistry) {
                auto it = viewRegistry.find(name);
                if (it != viewRegistry.end())
                  // Evaluate to handle leftover CacheRef operators
                  it->second.cached = evaluate(std::move(expr), false, false);
              }
              defaultPendingCacheRegistry.clear();
              defaultPendingCacheLookup.clear();

              // Eval with the final expression as a top-level expression, so that a CacheView
              // operator will be correctly handled and cached
              return evaluate(std::move(finalExpr), true, true);
            }

            if (head == "CacheView"_) {
              if (!topLevel) {
                throw std::runtime_error("CacheView can only be used at the top level");
              }

              if (dynamics.size() != 2)
                throw std::runtime_error("CacheView requires exactly 2 arguments");

              auto *name = std::get_if<Symbol>(&dynamics[0]);
              if (!name)
                throw std::runtime_error("CacheView first argument must be a symbol");

              auto viewName = name->getName();
              auto it = viewRegistry.find(viewName);
              if (it == viewRegistry.end())
                throw std::runtime_error("View not found for caching: " + viewName);

              // Cache the evaluated result and return it
              it->second.cached = dynamics[1].clone(CloneReason::EXPRESSION_WRAPPING);
              // Evaluate the expression to handle leftover CacheRef operators
              return evaluate(std::move(dynamics[1]), false, false);
            }

            // Handle leftover CacheRef operators in the pipeline on second pass
            if (head == "CacheRef"_) {
              if (dynamics.size() != 1)
                throw std::runtime_error("CacheRef requires exactly 1 symbol argument");

              auto *name = std::get_if<Symbol>(&dynamics[0]);
              if (!name)
                throw std::runtime_error("CacheRef argument must be a symbol");

              auto viewName = name->getName();
              auto it = viewRegistry.find(viewName);
              if (it == viewRegistry.end())
                throw std::runtime_error("CacheRef could not be resolved, view not found: " +
                                         viewName);
              if (!it->second.cached)
                throw std::runtime_error("CacheRef could not be resolved, view not cached: " +
                                         viewName);

              // The view should have already been cached by the WithPendingCaches handler
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

              auto viewName = name->getName();
              if (evaluationStack.count(viewName))
                throw std::runtime_error("Cannot drop view currently being evaluated: " + viewName);

              if (auto it = viewToViews.find(viewName);
                  it != viewToViews.end() && !it->second.empty())
                throw std::runtime_error("Cannot drop view " + viewName + ": " +
                                         *it->second.begin() + " depends on it");

              auto it = viewRegistry.find(viewName);
              if (it == viewRegistry.end())
                return Expression(false); // Dropping a non-existent view fails gracefully

              // Cleanup indexes
              for (const auto &[tableName, _] : it->second.signature.tablePredicates)
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
                    " view(s) are being evaluated, e.g.: " + *evaluationStack.begin());

              viewRegistry.clear();
              tableToViews.clear();
              viewToViews.clear();
              return Expression(true);
            }

            if (head == "ListViews"_) {
              if (!dynamics.empty())
                throw std::runtime_error("ListViews does not take any arguments");

              // Sort views for deterministic output
              std::vector<std::pair<std::string, const ViewEntry *>> sortedViews;
              sortedViews.reserve(viewRegistry.size());
              for (const auto &[name, expr] : viewRegistry)
                sortedViews.emplace_back(name, &expr);
              std::sort(sortedViews.begin(), sortedViews.end(),
                        [](const auto &a, const auto &b) { return a.first < b.first; });

              boss::ExpressionArguments nameArgs;
              boss::ExpressionArguments defArgs;
              for (const auto &[name, entry] : sortedViews) {
                nameArgs.emplace_back(Symbol(name));
                defArgs.emplace_back(entry->definition.clone(CloneReason::EXPRESSION_WRAPPING));
              }

              boss::ExpressionArguments columns;
              columns.emplace_back(ComplexExpression("Name"_, {}, std::move(nameArgs), {}));
              columns.emplace_back(ComplexExpression("Definition"_, {}, std::move(defArgs), {}));
              return Expression(ComplexExpression("ViewList"_, {}, std::move(columns), {}));
            }

            // Recursively evaluate arguments of other expressions
            // Do not eval args for ViewList to preserve view definitions for output
            // Check is here to block eval on second pass through ViewEngine in pipeline
            if (head != "ViewList"_) {
              for (auto &arg : dynamics) {
                arg = evaluate(std::move(arg), skipRewrite);
              }
            }

            return Expression(ComplexExpression(std::move(head), std::move(statics),
                                                std::move(dynamics), std::move(spans)));
          },
          [](auto &&other) -> Expression { return Expression(std::move(other)); }),
      std::move(e));
}

extern "C" BOSSExpression *evaluate(BOSSExpression *e) {
  defaultPendingCacheRegistry.clear();
  defaultPendingCacheLookup.clear();
  auto result = evaluate(std::move(e->delegate), false, true);

  if (defaultPendingCacheRegistry.empty())
    return new BOSSExpression{.delegate = std::move(result)};
  return new BOSSExpression{.delegate = repackWithPendingCaches(
                                std::move(defaultPendingCacheRegistry), std::move(result))};
}