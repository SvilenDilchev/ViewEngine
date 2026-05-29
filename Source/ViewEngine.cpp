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
              if (dynamics.size() < 1 || dynamics.size() > 3)
                throw std::runtime_error("QueryView requires 1 to 3 arguments");

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

              std::optional<IntegrityCheckMode> integrityMode = std::nullopt;
              if (dynamics.size() == 3) {
                auto *modeSym = std::get_if<Symbol>(&dynamics[2]);
                if (!modeSym)
                  throw std::runtime_error("QueryView third argument must be a symbol");

                auto const &modeStr = modeSym->getName();
                if (modeStr == "Structural")
                  integrityMode = IntegrityCheckMode::Structural;
                else if (modeStr == "Content")
                  integrityMode = IntegrityCheckMode::Content;
                else
                  throw std::runtime_error("QueryView unknown integrity mode: " + modeStr);
              }

              auto viewName = name->getName();
              auto it = viewRegistry.find(viewName);
              if (it == viewRegistry.end())
                throw std::runtime_error("View not found: " + name->getName());

              if (evaluationStack.count(viewName))
                throw std::runtime_error("Circular view dependency detected: " + viewName);

              ViewEntry &entry = it->second;

              // If cached before, avoid cloning by following borrowed cache protocol to move out
              // and move back in at the second pass of ViewEngine in the pipelines
              if (entry.cached) {
                boss::ExpressionArguments cacheRefArgs;
                cacheRefArgs.emplace_back(Symbol(viewName));
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
                std::string viewName;
                ~EvaluationGuard() { evaluationStack.erase(viewName); }
              } guard{viewName};

              // Cache miss
              auto result =
                  evaluate(entry.definition.clone(CloneReason::EVALUATE_CONST_EXPRESSION), true);

              if (!shouldCache)
                return std::move(result); // Return without caching

              // TODO: handle single view engine case - E.g., Cache the result evaluated only by
              // ViewEngine because noone can consume the wrapper otherwise
              // entry.cached = result.clone(CloneReason::EXPRESSION_WRAPPING);

              // Check if already registered to avoid duplicates
              if (!defaultCacheRegistry.count(viewName)) {
                defaultCacheRegistry[viewName] =
                    CacheEntry{std::move(result), CacheEntryType::Pending, integrityMode};
              }

              boss::ExpressionArguments cacheRefArgs;
              cacheRefArgs.emplace_back(Symbol(viewName));
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
                  throw std::runtime_error("View not found for caching in WithCaches: " + name);

                if (!it->second.cached)
                  it->second.cached = evaluate(std::move(entry.value), true);
              }
              defaultCacheRegistry.clear();

              // Evaluate with the final expression as a top-level expression, so that a CacheRef
              // operator will be correctly handled and cached
              return evaluate(std::move(finalExpr), true);
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

              auto viewName = name->getName();
              auto it = viewRegistry.find(viewName);
              if (it == viewRegistry.end())
                throw std::runtime_error("CacheRef could not be resolved, view not found: " +
                                         viewName);

              if (!it->second.cached) {
                auto regIt = defaultCacheRegistry.find(viewName);
                if (regIt == defaultCacheRegistry.end())
                  throw std::runtime_error(
                      "CacheRef could not be resolved, entry not found in cache registry: " +
                      viewName);
                it->second.cached = evaluate(std::move(regIt->second.value), true);
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
  defaultCacheRegistry.clear();
  auto result = evaluate(std::move(e->delegate), false, true);

  if (defaultCacheRegistry.empty())
    return new BOSSExpression{.delegate = std::move(result)};
  return new BOSSExpression{
      .delegate = repackWithCaches(std::move(defaultCacheRegistry), std::move(result))};
}