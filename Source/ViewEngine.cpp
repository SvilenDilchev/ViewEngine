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

            if (head == "DefineView"_) {
              if (dynamics.size() != 2) {
                return Expression(false); // DefineView requires exactly 2 arguments: a symbol for
                                          // the view name and an expression for the view definition
              }
              if (auto *name = std::get_if<Symbol>(&dynamics[0])) {
                viewRegistry[name->getName()] = std::move(dynamics[1]);
                return Expression(true);
              }
              return Expression(false); // DefineView requires a symbol for the view name
            }

            if (head == "QueryView"_) {
              if (dynamics.size() != 1) {
                throw std::runtime_error("QueryView requires exactly 1 symbol argument");
              }
              if (auto *name = std::get_if<Symbol>(&dynamics[0])) {
                auto viewName = name->getName();
                auto it = viewRegistry.find(viewName);
                if (it != viewRegistry.end()) {
                  if (evaluationStack.count(viewName)) {
                    throw std::runtime_error("Circular view dependency detected: " + viewName);
                  }
                  evaluationStack.insert(viewName);
                  struct EvaluationGuard { // RAII guard for evaluation stack cleanup
                    std::string viewName;
                    ~EvaluationGuard() { evaluationStack.erase(viewName); }
                  } guard{viewName};
                  return evaluate(it->second.clone(CloneReason::EVALUATE_CONST_EXPRESSION));
                }
                throw std::runtime_error("View not found: " + name->getName());
              }
              throw std::runtime_error("QueryView argument must be a symbol");
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
