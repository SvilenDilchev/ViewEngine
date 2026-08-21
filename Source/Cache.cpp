#include "Cache.hpp"
#include <Expression.hpp>
#include <algorithm>
#include <unordered_map>

std::unordered_map<Symbol, Expression> viewCache;
double viewCacheSize = 1e9; // 1 GB default limit
double viewCacheOccupancy = 0.0;

// Table size computation
namespace {
constexpr size_t kVarWidthSampleSize = 64; // Number of rows to sample for variable-width columns

bool isNullMarker(Expression const &e) {
  auto const *s = std::get_if<Symbol>(&e);
  return s && *s == "NULL"_;
}

double sizeOfScalar(Expression const &val) {
  return std::visit(
      boss::utilities::overload(
          [](int64_t const &) -> double { return sizeof(int64_t); },
          [](double const &) -> double { return sizeof(double); },
          [](bool const &) -> double { return sizeof(bool); },
          [](std::string const &s) -> double { return static_cast<double>(s.size()); },
          [](boss::Symbol const &s) -> double { return static_cast<double>(s.getName().size()); },
          [](auto const &) -> double { return 8.0; }), // defensive fallback
      val);
}

// Samples up to kVarWidthSampleSize elements from an index-accessible range
template <typename Range, typename ByteSizeFunc>
double estimateVarWidthSize(Range const &range, size_t totalCount, ByteSizeFunc &&byteSizeOf) {
  if (totalCount == 0)
    return 0.0;

  size_t sampleCount = std::min(totalCount, kVarWidthSampleSize);
  size_t stride = std::max<size_t>(1, totalCount / sampleCount);

  double sampledBytes = 0.0;
  size_t sampled = 0;
  for (size_t i = 0; i < totalCount && sampled < sampleCount; i += stride) {
    sampledBytes += byteSizeOf(range[i]);
    ++sampled;
  }

  double avgBytes = sampled > 0 ? sampledBytes / static_cast<double>(sampled) : 0.0;
  return avgBytes * static_cast<double>(totalCount);
}

// Compute the size of a column from its spans
double sizeOfColumnSpans(boss::expressions::ExpressionSpanArguments const &spans) {
  double total = 0.0;
  for (auto const &span : spans) {
    total += std::visit(
        [](auto const &s) -> double {
          using ElementT = std::remove_const_t<typename std::decay_t<decltype(s)>::element_type>;
          if constexpr (std::is_same_v<ElementT, boss::Symbol>) {
            return estimateVarWidthSize(s, s.size(), [](boss::Symbol const &sym) {
              return static_cast<double>(sym.getName().size());
            });
          } else if constexpr (std::is_same_v<ElementT, std::string>) {
            return estimateVarWidthSize(s, s.size(), [](std::string const &str) {
              return static_cast<double>(str.size());
            });
          } else {
            return static_cast<double>(s.size()) * sizeof(ElementT);
          }
        },
        span);
  }
  return total;
}

// Compute the size of a column from its dynamics
double sizeOfColumn(boss::ExpressionArguments const &dynamics) {
  if (dynamics.empty())
    return 0.0;

  auto rowCount = dynamics.size();

  auto first = std::find_if(dynamics.begin(), dynamics.end(),
                            [](auto const &e) { return !isNullMarker(e); });

  if (first == dynamics.end())
    return 0.0; // Null column, size is 0

  bool fixedWidth = std::holds_alternative<int64_t>(*first) ||
                    std::holds_alternative<double>(*first) || std::holds_alternative<bool>(*first);

  if (fixedWidth)
    return sizeOfScalar(*first) * static_cast<double>(rowCount);

  return estimateVarWidthSize(dynamics, rowCount, [](Expression const &e) {
    return isNullMarker(e) ? 0.0 : sizeOfScalar(e);
  });
}
} // namespace

double computeSize(Expression const &tableExpr) {
  auto const *ce = std::get_if<ComplexExpression>(&tableExpr);
  if (!ce || ce->getHead() != "Table"_)
    return 0.0; // Not a table expression, size is 0, cannot be admitted to the cache

  double total = 0.0;
  for (auto const &colExpr : ce->getDynamicArguments()) {
    auto const *col = std::get_if<ComplexExpression>(&colExpr);
    if (!col)
      continue;
    // Unwrap Date column
    if (col->getHead() == "Date"_ && col->getDynamicArguments().size() == 1)
      col = std::get_if<ComplexExpression>(&col->getDynamicArguments()[0]);
    if (!col)
      continue;
    total += col->getDynamicArguments().empty() ? sizeOfColumnSpans(col->getSpanArguments())
                                                : sizeOfColumn(col->getDynamicArguments());
  }

  return total;
}

// Cost computation
namespace {

bool hasIsoInfo(ViewEntry const &e) { return e.computeCost > 0.0 && e.materialiseCost > 0.0; }
bool hasStdInfo(ViewEntry const &e) { return e.marginalComputeCost > 0.0; }

} // namespace

std::optional<double> isoCost(ViewEntry const &entry) {
  if (!hasIsoInfo(entry))
    return std::nullopt;
  return entry.computeCost + entry.materialiseCost;
}

std::optional<double> stdCost(ViewEntry const &entry,
                              std::unordered_map<boss::Symbol, std::optional<double>> &memo,
                              bool fallbackToIso) {
  if (!hasStdInfo(entry))
    return std::nullopt; // parent true cost will handle fallback to iso

  double total = entry.marginalComputeCost;
  for (auto const &dep : entry.dependencies) {
    auto it = viewRegistry.find(dep);
    if (it == viewRegistry.end())
      return std::nullopt; // Shouldn't happen in practice

    auto const &depEntry = it->second;
    if (viewCache.find(dep) != viewCache.end()) {
      // TODO: in the scope of the project as we know the actual reuse cost is close enough to zero,
      // it doesn't matter if we know the value or it's unknown (0.0 default), so it's fine to not
      // handle the case where the reuse cost is unknown.
      total += depEntry.reuseCost;
    } else {
      auto depCost = trueCost(dep, memo, fallbackToIso);
      if (!depCost)
        return std::nullopt; // Missing info anywhere down the chain, shouldn't happen if
                             // fallbackToIso is true, but we don't want to crash if it does.
      total += *depCost;
    }
  }

  return total;
}

std::optional<double> trueCost(boss::Symbol const &name,
                               std::unordered_map<boss::Symbol, std::optional<double>> &memo,
                               bool fallbackToIso) {
  if (auto it = memo.find(name); it != memo.end())
    return it->second;

  auto it = viewRegistry.find(name);
  if (it == viewRegistry.end())
    return std::nullopt; // Shouldn't happen in practice

  auto const &entry = it->second;
  auto iso = isoCost(entry);
  auto std_ = stdCost(entry, memo, fallbackToIso);

  std::optional<double> result;
  if (iso && std_)
    result = std::min(*iso, *std_);
  else if (iso && fallbackToIso)
    result = iso;
  else
    result = std_;

  memo[name] = result;

  return result;
}

std::optional<double> benefit(boss::Symbol const &name,
                              std::unordered_map<boss::Symbol, std::optional<double>> &memo,
                              bool fallbackToIso) {
  auto it = viewRegistry.find(name);
  if (it == viewRegistry.end())
    return std::nullopt; // Shouldn't happen in practice

  auto const &entry = it->second;
  if (entry.size <= 0.0)
    return std::nullopt;

  auto cost = trueCost(name, memo, fallbackToIso);
  if (!cost)
    return std::nullopt;

  double savings = *cost - entry.reuseCost;
  return savings * entry.importanceFactor / entry.size;
}

ExecutionStrategy
selectExecutionStrategy(ViewEntry &entry,
                        std::unordered_map<boss::Symbol, std::optional<double>> &memo) {
  auto iso = isoCost(entry);
  if (!iso) {
    // Run isolated measurement to get instrumentation info and defer the caching decision to VE2
    // where we have more information.
    return ExecutionStrategy::IsolatedMeasurement;
  }

  // Memoise true cost computations for this pass to avoid redundant calculations
  auto std_ = stdCost(entry, memo);
  if (!std_) {
    // Run standard execution to get instrumentation info and defer the caching decision to VE2
    // where we have more information.
    return ExecutionStrategy::Standard;
  }

  ExecutionStrategy strategy =
      *iso < *std_ ? ExecutionStrategy::IsolatedMeasurement : ExecutionStrategy::Standard;

  return strategy;
}