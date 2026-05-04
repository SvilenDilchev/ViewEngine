#include <BOSS.hpp>
#include <Expression.hpp>
#include <ExpressionUtilities.hpp>
#include <Utilities.hpp>

#include <string>
#include <unordered_map>

using boss::utilities::operator""_;
using boss::ComplexExpression;
using boss::Symbol;

using boss::Expression;
using boss::expressions::CloneReason;

// In memory register of Views
static std::unordered_map<std::string, Expression> viewRegistry;

static Expression substituteViews(Expression &&e) {
  return std::visit(
      boss::utilities::overload(
          [](Symbol &&s) -> Expression {
            auto it = viewRegistry.find(s.getName());
            if (it != viewRegistry.end()) {
              return substituteViews(it->second.clone(CloneReason::EVALUATE_CONST_EXPRESSION));
            }
            return Expression(std::move(s));
          },
          [](ComplexExpression &&ce) -> Expression {
            auto [head, statics, dynamics, spans] = std::move(ce).decompose();
            for (auto &arg : dynamics) {
              arg = substituteViews(std::move(arg));
            }
            return Expression(ComplexExpression(std::move(head), std::move(statics),
                                                std::move(dynamics), std::move(spans)));
          },
          [](auto &&other) -> Expression { return Expression(std::move(other)); }),
      std::move(e));
}

static Expression evaluate(Expression &&e) {
  return std::visit(
      [](auto &&expr) -> Expression {
        if constexpr (std::is_same_v<std::decay_t<decltype(expr)>, ComplexExpression>) {
          auto [head, statics, dynamics, spans] = std::move(expr).decompose();

          if (head == "DefineView"_) {
            if (dynamics.size() != 2) {
              throw std::runtime_error("DefineView requires exactly 2 dynamic arguments");
            }

            if (auto *name = std::get_if<Symbol>(&dynamics[0])) {
              viewRegistry[name->getName()] = std::move(dynamics[1]);
            } else {
              throw std::runtime_error("First argument to DefineView must be a symbol");
            }
            return Expression("ViewDefined"_);
          } else if (head == "QueryView"_) {
            if (dynamics.size() != 1) {
              throw std::runtime_error("QueryView requires exactly 1 dynamic argument");
            }

            if (auto *name = std::get_if<Symbol>(&dynamics[0])) {
              auto it = viewRegistry.find(name->getName());
              if (it != viewRegistry.end()) {
                return substituteViews(it->second.clone(CloneReason::EVALUATE_CONST_EXPRESSION));
              } else {
                throw std::runtime_error("View not found: " + name->getName());
              }
            } else {
              throw std::runtime_error("Argument to QueryView must be a symbol");
            }
          }
          return Expression(ComplexExpression(std::move(head), std::move(statics),
                                              std::move(dynamics), std::move(spans)));
        } else {
          return Expression(std::move(expr));
        }
      },
      std::move(e));
};

extern "C" BOSSExpression *evaluate(BOSSExpression *e) {
  return new BOSSExpression{.delegate = evaluate(std::move(e->delegate))};
};
