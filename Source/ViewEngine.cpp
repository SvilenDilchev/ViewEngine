#include <BOSS.hpp>
#include <Expression.hpp>
#include <ExpressionUtilities.hpp>
#include <Utilities.hpp>

#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>

using boss::utilities::operator""_;
using boss::ComplexExpression;
using boss::Symbol;

using boss::Expression;
using boss::expressions::CloneReason;

static std::unordered_map<std::string, Expression> viewRegistry; // In memory register of Views
static std::unordered_set<std::string> evaluationStack;          // Guard against runtime cycles
static std::unordered_map<std::string, std::unordered_set<std::string>>
    viewDependencies; // Dependency graph used for define time and drop time validation

struct WalkResult {
  std::unordered_set<std::string> dependencies;
  std::unordered_set<std::string> dropped;
  bool callsClearViews = false;
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
                   if (head == "DropView"_) {
                     if (!dynamics.empty())
                       if (auto *sym = std::get_if<Symbol>(&dynamics[0]))
                         result.dropped.insert(sym->getName());
                     return;
                   }
                   if (head == "ClearViews"_) {
                     result.callsClearViews = true;
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
  auto it = viewDependencies.find(current);
  if (it == viewDependencies.end())
    return false;

  for (const auto &dep : it->second)
    if (hasCycle(newView, dep, visited))
      return true;

  return false;
}

static Expression evaluate(Expression &&e) {
  return std::visit(
      boss::utilities::overload(
          [](ComplexExpression &&ce) -> Expression {
            auto [head, statics, dynamics, spans] = std::move(ce).decompose();

            if (head == "DefineView"_) {
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

              if (result.callsClearViews)
                return Expression(false); // Block if definition calls ClearViews

              std::unordered_set<std::string> visited;
              for (const auto &dep : result.dependencies) {
                if (result.dropped.count(dep))
                  // Block if definition drops any views it depends on
                  return Expression(false);

                if (hasCycle(viewName, dep, visited))
                  return Expression(false); // Block if definition creates a cycle
              }

              viewDependencies[viewName] = std::move(result.dependencies);
              viewRegistry[viewName] = std::move(dynamics[1]);
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

              evaluationStack.insert(viewName);
              struct EvaluationGuard { // RAII guard for evaluation stack cleanup
                std::string viewName;
                ~EvaluationGuard() { evaluationStack.erase(viewName); }
              } guard{viewName};

              return evaluate(it->second.clone(CloneReason::EVALUATE_CONST_EXPRESSION));
            }

            if (head == "DropView"_) {
              if (dynamics.size() != 1)
                throw std::runtime_error("DropView requires exactly 1 symbol argument");

              auto *name = std::get_if<Symbol>(&dynamics[0]);
              if (!name)
                throw std::runtime_error("DropView argument must be a symbol");

              auto viewName = name->getName();
              if (evaluationStack.count(viewName))
                throw std::runtime_error("Cannot drop view currently being evaluated: " + viewName);

              for (const auto &[dependent, deps] : viewDependencies) {
                if (deps.count(viewName))
                  throw std::runtime_error("Cannot drop view " + viewName + ": " + dependent +
                                           " depends on it");
              }

              viewRegistry.erase(viewName);
              viewDependencies.erase(viewName);
              return Expression(true);
            }

            if (head == "ClearViews"_) {
              if (!dynamics.empty())
                throw std::runtime_error("ClearViews does not take any arguments");

              if (!evaluationStack.empty())
                throw std::runtime_error(
                    "Cannot clear views while " + std::to_string(evaluationStack.size()) +
                    " view(s) are being evaluated, e.g.: " + *evaluationStack.begin());

              viewRegistry.clear();
              viewDependencies.clear();
              return Expression(true);
            }

            if (head == "ListViews"_) {
              if (!dynamics.empty())
                throw std::runtime_error("ListViews does not take any arguments");

              // Sort views for deterministic output
              std::vector<std::pair<std::string, const Expression *>> sortedViews;
              for (const auto &[name, expr] : viewRegistry)
                sortedViews.emplace_back(name, &expr);
              std::sort(sortedViews.begin(), sortedViews.end(),
                        [](const auto &a, const auto &b) { return a.first < b.first; });

              boss::ExpressionArguments nameArgs;
              boss::ExpressionArguments defArgs;
              for (const auto &[name, definition] : sortedViews) {
                nameArgs.emplace_back(Symbol(name));
                defArgs.emplace_back(definition->clone(CloneReason::EXPRESSION_WRAPPING));
              }

              boss::ExpressionArguments columns;
              columns.emplace_back(ComplexExpression("Name"_, {}, std::move(nameArgs), {}));
              columns.emplace_back(ComplexExpression("Definition"_, {}, std::move(defArgs), {}));
              return Expression(ComplexExpression("ViewList"_, {}, std::move(columns), {}));
            }

            for (auto &arg : dynamics) {
              arg = evaluate(std::move(arg)); // Recursively evaluate arguments of other expressions
            }

            return Expression(ComplexExpression(std::move(head), std::move(statics),
                                                std::move(dynamics), std::move(spans)));
          },
          [](auto &&other) -> Expression { return Expression(std::move(other)); }),
      std::move(e));
}

extern "C" BOSSExpression *evaluate(BOSSExpression *e) {
  return new BOSSExpression{.delegate = evaluate(std::move(e->delegate))};
};
