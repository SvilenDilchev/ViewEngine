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

// compute the size of a column from its dynamics
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
    throw std::runtime_error("computeSize: Expression is not a Table");

  double total = 0.0;

  // Go over spans, currently dead code because ACE does not produce spans, but we keep it for
  // future-proofing
  for (auto const &span : ce->getSpanArguments()) {
    total += std::visit(
        [](auto const &s) -> double {
          using ElementT = std::remove_const_t<typename std::decay_t<decltype(s)>::element_type>;
          if constexpr (std::is_same_v<ElementT, boss::Symbol>) {
            return estimateVarWidthSize(s, s.size(), [](boss::Symbol const &sym) {
              return static_cast<double>(sym.getName().size());
            });
          } else {
            return static_cast<double>(s.size()) * sizeof(ElementT);
          }
        },
        span);
  }

  for (auto const &colExpr : ce->getDynamicArguments())
    if (auto const *col = std::get_if<ComplexExpression>(&colExpr))
      total += sizeOfColumn(col->getDynamicArguments());

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

std::optional<double> stdCost(ViewEntry const &entry, bool fallbackToIso) {
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
      auto depCost = trueCost(dep, fallbackToIso);
      if (!depCost)
        return std::nullopt; // Missing info anywhere down the chain, shouldn't happen if
                             // fallbackToIso is true, but we don't want to crash if it does.
      total += *depCost;
    }
  }

  return total;
}

std::optional<double> trueCost(boss::Symbol const &name, bool fallbackToIso) {
  auto it = viewRegistry.find(name);
  if (it == viewRegistry.end())
    return std::nullopt; // Shouldn't happen in practice

  auto const &entry = it->second;
  auto iso = isoCost(entry);
  auto std_ = stdCost(entry, fallbackToIso);

  if (iso && std_)
    return std::min(*iso, *std_);
  if (iso && fallbackToIso)
    return iso;
  return std_;
}

std::optional<double> benefit(boss::Symbol const &name, bool fallbackToIso) {
  auto it = viewRegistry.find(name);
  if (it == viewRegistry.end())
    return std::nullopt; // Shouldn't happen in practice

  auto const &entry = it->second;
  if (entry.size <= 0.0)
    return std::nullopt;

  auto cost = trueCost(name, fallbackToIso);
  if (!cost)
    return std::nullopt;

  double savings = *cost - entry.reuseCost;
  return savings * entry.importanceFactor / entry.size;
}

namespace {

struct EvictionPlan {
  std::vector<boss::Symbol> names;
  double totalSize = 0.0;
  double avgBenefit = 0.0; // size-weighted mean benefit of the selected set
};

// Selects (without evicting) the lowest-benefit entries from viewCache needed to free at
// least `bytesToFree` bytes, or as many as are available if the cache can't cover it.
EvictionPlan selectForEviction(double bytesToFree) {
  struct Candidate {
    boss::Symbol name;
    double size;
    double benefit;
  };
  std::vector<Candidate> candidates;
  candidates.reserve(viewCache.size());

  for (auto const &[cachedName, value] : viewCache) {
    auto b = benefit(cachedName, /*fallbackToIso=*/true);
    if (!b)
      continue; // shouldn't happen for a cached entry, but stay defensive
    candidates.push_back({cachedName, viewRegistry.at(cachedName).size, *b});
  }

  std::sort(candidates.begin(), candidates.end(),
            [](auto const &a, auto const &b) { return a.benefit < b.benefit; });

  EvictionPlan plan;
  double accWeighted = 0.0;
  for (auto const &c : candidates) {
    if (plan.totalSize >= bytesToFree)
      break;
    plan.names.push_back(c.name);
    plan.totalSize += c.size;
    accWeighted += c.benefit * c.size;
  }
  plan.avgBenefit = plan.totalSize > 0.0 ? accWeighted / plan.totalSize : 0.0;
  return plan;
}

// Commits a previously selected eviction plan.
void applyEviction(EvictionPlan const &plan) {
  for (auto const &name : plan.names) {
    viewCacheOccupancy -= viewRegistry.at(name).size;
    viewCache.erase(name);
  }
}

} // namespace

ExecutionStrategy selectExecutionStrategy(ViewEntry &entry) {
  auto iso = isoCost(entry);
  if (!iso) {
    // Run isolated measurement to get instrumentation info and defer the caching decision to VE2
    // where we have more information.
    return ExecutionStrategy::IsolatedMeasurement;
  }

  auto std_ = stdCost(entry);
  if (!std_) {
    // Run standard execution to get instrumentation info and defer the caching decision to VE2
    // where we have more information.
    return ExecutionStrategy::Standard;
  }

  ExecutionStrategy strategy =
      *iso < *std_ ? ExecutionStrategy::IsolatedMeasurement : ExecutionStrategy::Standard;

  return strategy;
}

bool tryAdmit(Symbol const &name) {
  if (viewCacheOccupancy > viewCacheSize) {
    auto plan = selectForEviction(viewCacheOccupancy - viewCacheSize);
    applyEviction(plan); // enforce budget if changed by SetCacheBudget to a smaller value
  }

  auto it = viewRegistry.find(name);
  if (it == viewRegistry.end())
    return false; // Shouldn't happen in practice

  auto const &entry = it->second;

  if (viewCacheOccupancy + entry.size <= viewCacheSize)
    return true; // Fits without eviction
  if (entry.size > viewCacheSize)
    return false; // can never fit, even with the cache fully emptied

  auto candidateBenefit = benefit(name, true);
  if (!candidateBenefit || *candidateBenefit <= 0.0)
    return false; // Shouldn't happen but guard to be safe

  double bytesNeeded = (viewCacheOccupancy + entry.size) - viewCacheSize;
  auto plan = selectForEviction(bytesNeeded);

  // Check in the case we couldn't calculate the benefit score of every cached view
  if (plan.totalSize < bytesNeeded)
    return false; // not enough evictable space even if we took everything

  if (plan.avgBenefit >= *candidateBenefit)
    return false; // Evicting the least beneficial views would be worse than keeping them

  applyEviction(plan);
  return true;
}