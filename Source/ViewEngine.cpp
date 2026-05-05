#include <BOSS.hpp>
#include <Expression.hpp>
#include <ExpressionUtilities.hpp>
#include <Utilities.hpp>

#include <string>
#include <unordered_map>
#include <unordered_set>

using boss::utilities::operator""_;
using boss::ComplexExpression;
using boss::Symbol;

using boss::Expression;
using boss::expressions::CloneReason;

static std::unordered_map<std::string, Expression> viewRegistry; // In memory register of Views
static std::unordered_set<std::string> evaluationStack;          // To detect circular dependencies

static Expression evaluate(Expression &&e) {
  return std::visit(
      boss::utilities::overload(
          [](ComplexExpression &&ce) -> Expression {
            auto [head, statics, dynamics, spans] = std::move(ce).decompose();

            // TODO: validate view expression at define time to catch:
            //   - circular dependencies (A -> B -> A)
            //   - dropping a view that is later referenced in the same expression
            //   - calls to ClearViews
            if (head == "DefineView"_) {
              if (dynamics.size() != 2)
                return Expression(false); // DefineView requires exactly 2 arguments: a symbol for
                                          // the view name and an expression for the view definition

              auto *name = std::get_if<Symbol>(&dynamics[0]);
              if (!name)
                return Expression(false); // DefineView requires a symbol for the view name

              viewRegistry[name->getName()] = std::move(dynamics[1]);
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

              viewRegistry.erase(viewName);
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
              return Expression(true);
            }

            if (head == "ListViews"_) {
              if (!dynamics.empty())
                throw std::runtime_error("ListViews does not take any arguments");

              std::vector<std::string> sortedNames; // Sort view names for deterministic output
              for (const auto &[name, _] : viewRegistry)
                sortedNames.emplace_back(name);
              std::sort(sortedNames.begin(), sortedNames.end());

              boss::ExpressionArguments nameArgs;
              boss::ExpressionArguments defArgs;
              for (const auto &name : sortedNames) {
                nameArgs.emplace_back(Symbol(name));
                defArgs.emplace_back(viewRegistry.at(name).clone(CloneReason::EXPRESSION_WRAPPING));
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
