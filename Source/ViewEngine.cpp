#include <BOSS.hpp>
#include <Expression.hpp>
#include <ExpressionUtilities.hpp>
#include <Utilities.hpp>

#include <algorithm>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

using boss::utilities::operator""_;
using boss::ComplexExpression;
using boss::Symbol;

using boss::Expression;
using boss::expressions::CloneReason;

struct ViewEntry {
  std::optional<Expression> cached;
  Expression definition;
  std::unordered_set<std::string> dependencies;
};

static std::unordered_map<std::string, ViewEntry> viewRegistry; // In memory register of Views
static std::unordered_set<std::string> evaluationStack;         // Guard against runtime cycles

struct WalkResult {
  std::unordered_set<std::string> dependencies;
  bool sideEffect = false;
};

// Used to build the dependency graph with direct dependencies and detect calls to ClearViews
static void walkViews(const Expression &expr, WalkResult &result) {
  std::visit(boss::utilities::overload(
                 [&](const ComplexExpression &ce) {
                   auto const &dynamics = ce.getDynamicArguments();
                   auto head = ce.getHead();
                   if (head == "QueryView"_) {
                     if (!dynamics.empty())
                       if (auto *sym = std::get_if<Symbol>(&dynamics[0]))
                         result.dependencies.insert(sym->getName());
                     return;
                   }
                   if (head == "DefineView"_ || head == "DropView"_ || head == "ClearViews"_ ||
                       head == "CacheView"_) {
                     result.sideEffect = true;
                     return;
                   }
                   for (const auto &arg : dynamics) {
                     walkViews(arg, result);
                   }
                 },
                 [](const auto &) {}),
             expr);
}

// DFS traversal to detect cycles in the dependency graph
static bool hasCycle(const std::string &newView, const std::string &current,
                     std::unordered_set<std::string> &visited) {
  if (current == newView)
    return true;

  if (visited.count(current))
    return false;

  visited.insert(current);
  auto it = viewRegistry.find(current);
  if (it == viewRegistry.end())
    return false;

  for (const auto &dep : it->second.dependencies)
    if (hasCycle(newView, dep, visited))
      return true;

  return false;
}

// Invalidate caches of all views that directly or indirectly depend on the given view
static void invalidateDependentCaches(const std::string &invalidatedView) {
  for (auto &[name, entry] : viewRegistry) {
    if (entry.dependencies.count(invalidatedView)) {
      entry.cached = std::nullopt;
      invalidateDependentCaches(name);
    }
  }
}

static Expression evaluate(Expression &&e, bool topLevel = false) {
  return std::visit(
      boss::utilities::overload(
          [topLevel](ComplexExpression &&ce) -> Expression {
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
              WalkResult result;
              walkViews(dynamics[1], result); // Walk the view expression to collect data for
                                              // dependency graph construction and validation

              if (result.sideEffect)
                // Block if definition has side effects such as defining or dropping views
                return Expression(false);

              std::unordered_set<std::string> visited;
              for (const auto &dep : result.dependencies) {
                if (hasCycle(viewName, dep, visited))
                  return Expression(false); // Block if definition creates a cycle
              }

              invalidateDependentCaches(viewName);

              viewRegistry[viewName] =
                  ViewEntry{std::nullopt, std::move(dynamics[1]), std::move(result.dependencies)};

              return Expression(true);
            }

            if (head == "QueryView"_) {
              if (dynamics.size() != 1)
                throw std::runtime_error("QueryView requires exactly 1 symbol argument");

              auto *name = std::get_if<Symbol>(&dynamics[0]);
              if (!name)
                throw std::runtime_error("QueryView argument must be a symbol");

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
                  evaluate(entry.definition.clone(CloneReason::EVALUATE_CONST_EXPRESSION));

              if (!topLevel) {
                // Only cache top-level calls to QueryView avoid nesting CacheViews,
                // which would cause issues to other engines evaluating
                return std::move(result);
              }

              boss::expressions::ExpressionArguments cacheArgs;
              cacheArgs.emplace_back(Symbol(viewName));
              cacheArgs.emplace_back(std::move(result));
              return Expression(ComplexExpression("CacheView"_, {}, std::move(cacheArgs), {}));
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
              return std::move(dynamics[1]);
            }

            if (head == "DropView"_) {
              if (!topLevel) {
                throw std::runtime_error("DropView can only be used at the top level");
              }

              if (dynamics.size() != 1)
                throw std::runtime_error("DropView requires exactly 1 symbol argument");

              auto *name = std::get_if<Symbol>(&dynamics[0]);
              if (!name)
                throw std::runtime_error("DropView argument must be a symbol");

              auto viewName = name->getName();
              if (evaluationStack.count(viewName))
                throw std::runtime_error("Cannot drop view currently being evaluated: " + viewName);

              for (const auto &[dependent, entry] : viewRegistry) {
                if (entry.dependencies.count(viewName))
                  throw std::runtime_error("Cannot drop view " + viewName + ": " + dependent +
                                           " depends on it");
              }

              viewRegistry.erase(viewName);
              return Expression(true);
            }

            if (head == "ClearViews"_) {
              if (!topLevel) {
                throw std::runtime_error("ClearViews can only be used at the top level");
              }

              if (!dynamics.empty())
                throw std::runtime_error("ClearViews does not take any arguments");

              if (!evaluationStack.empty())
                throw std::runtime_error(
                    "Cannot clear views while " + std::to_string(evaluationStack.size()) +
                    " view(s) are being evaluated, e.g.: " + *evaluationStack.begin());

              viewRegistry.clear();
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
                arg = evaluate(std::move(arg));
              }
            }

            return Expression(ComplexExpression(std::move(head), std::move(statics),
                                                std::move(dynamics), std::move(spans)));
          },
          [](auto &&other) -> Expression { return Expression(std::move(other)); }),
      std::move(e));
}

extern "C" BOSSExpression *evaluate(BOSSExpression *e) {
  return new BOSSExpression{.delegate = evaluate(std::move(e->delegate), true)};
};
