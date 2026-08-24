#include "Cache.hpp"
#include <Expression.hpp>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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
    // Unwrap Nullable(<column>, Nulls(Span<int64>[off,len,...])). ACE wraps this
    // OUTSIDE Date(...), so it comes off first. The run table is real cached bytes,
    // so it counts towards the column's size.
    if (col->getHead() == "Nullable"_ && col->getDynamicArguments().size() == 2) {
      if (auto const *nulls = std::get_if<ComplexExpression>(&col->getDynamicArguments()[1]))
        total += sizeOfColumnSpans(nulls->getSpanArguments());
      col = std::get_if<ComplexExpression>(&col->getDynamicArguments()[0]);
      if (!col)
        continue;
    }
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

std::optional<double> stdCost(ViewEntry const &entry, bool fallbackToIso,
                              std::unordered_set<boss::Symbol> *visited) {
  if (!hasStdInfo(entry))
    return std::nullopt; // parent true cost will handle fallback to iso

  std::unordered_set<boss::Symbol> localVisited;
  auto &visitedSet = visited ? *visited : localVisited;

  double total = entry.marginalComputeCost + entry.materialiseCost;
  for (auto const &dep : entry.dependencies) {
    auto it = viewRegistry.find(dep);
    if (it == viewRegistry.end())
      return std::nullopt; // Shouldn't happen in practice

    auto const &depEntry = it->second;
    if (viewCache.count(dep)) {
      if (depEntry.reuseCost <= 0.0)
        return std::nullopt; // View has been cached but never reused, so reuse cost is unknown, the
                             // selection of the execution strategy doesn't actually matter because
                             // we will be using the cached value, and the reuse cost will be
                             // measured for future runs.
      if (visitedSet.insert(dep).second)
        total += depEntry.reuseCost;
    } else {
      auto depCost = trueCost(dep, fallbackToIso, &visitedSet);
      if (!depCost)
        return std::nullopt; // Missing info anywhere down the chain, shouldn't happen if
                             // fallbackToIso is true, but we don't want to crash if it does.
      total += *depCost;
    }
  }

  return total;
}

std::optional<double> trueCost(boss::Symbol const &name, bool fallbackToIso,
                               std::unordered_set<boss::Symbol> *visited) {
  auto it = viewRegistry.find(name);
  if (it == viewRegistry.end())
    return std::nullopt; // Shouldn't happen in practice

  std::unordered_set<boss::Symbol> localVisited;
  auto &visitedSet = visited ? *visited : localVisited;
  if (!visitedSet.insert(name).second)
    return 0.0; // Already visited, don't add the cost again, in ACE if a view is used multiple
                // times in a query, it's computed only once then reused, so we don't want to
                // double-count its cost.

  auto const &entry = it->second;
  auto iso = isoCost(entry);
  auto std_ = stdCost(entry, fallbackToIso, &visitedSet);

  if (iso && std_)
    return std::min(*iso, *std_);
  else if (iso && fallbackToIso)
    return iso;
  return std_;
}

std::optional<double> benefit(boss::Symbol const &name, bool fallbackToIso) {
  auto it = viewRegistry.find(name);
  if (it == viewRegistry.end())
    return std::nullopt; // Shouldn't happen in practice

  auto &entry = it->second;
  if (entry.size <= 0.0)
    return std::nullopt;

  auto cost = trueCost(name, fallbackToIso);
  if (!cost)
    return std::nullopt;

  ageEntry(entry); // Age the entry if it wasn't already, no-op otherwise
  double savings = *cost - entry.reuseCost;
  return savings * entry.importanceFactor / entry.size;
}

namespace {
// Used during rewriting to determine whether a specific candidate view can be rejected from
// admission to the cache outright. Important to void materialising a view that will be rejected
// anyway, which is more expensive than just computing it from scratch as it just incurs the
// materialisation costs and it can break up the pipelined execution of a parent view or query.
struct AdmissionSnapshot {
  bool valid = false;
  std::vector<std::pair<double, double>> deferTier; // (benefit, space used before it), descending
  double usedAfterAll = 0.0;
};
// TODO: explore persisting benefit scores between VE1 and VE2. importanceFactor is
// provably identical across the two passes (ageEntry maps both ticks to the same query
// index, and increments only happen in VE1), so only the views whose costs/size VE2
// refreshes -- the defaultCacheRegistry entries -- and their transitive dependants
// (viewToViews) would need recomputing.
AdmissionSnapshot admissionSnapshot;

// Build a sorted benefit ranking of all views that are either cached or in the defaultCacheRegistry
// TODO: Currently rebuilds the whole ranking on each call, but could be optimised to only update
// the changed views and their transitive dependants.
void buildAdmissionSnapshot() {
  auto &snap = admissionSnapshot;
  snap.deferTier.clear();
  std::vector<std::pair<double, double>> admitTier; // (benefit, size)
  std::vector<std::pair<double, double>> deferSized;
  auto add = [&](boss::Symbol const &name) {
    auto it = viewRegistry.find(name);
    if (it == viewRegistry.end() || it->second.size <= 0.0)
      return;
    auto const b = benefit(name, true).value_or(0.0);
    if (it->second.cachingDecision == CachingDecision::Admit)
      admitTier.emplace_back(b, it->second.size);
    else
      deferSized.emplace_back(b, it->second.size);
  };
  // NOTE: no view can be simultaneously in viewCache and defaultCacheRegistry, except for the
  // top-level QueryView cache hit, which will never reach this code because QueryView is not a
  // rewritable operator
  for (auto const &[name, value] : viewCache)
    add(name);
  for (auto const &[name, entry] : defaultCacheRegistry)
    add(name);
  auto byBenefitDesc = [](auto const &a, auto const &b) { return a.first > b.first; };
  std::sort(admitTier.begin(), admitTier.end(), byBenefitDesc);
  std::sort(deferSized.begin(), deferSized.end(), byBenefitDesc);

  double used = 0.0;
  for (auto const &[b, size] : admitTier)
    if (used + size <= viewCacheSize)
      used += size;
  snap.deferTier.reserve(deferSized.size());
  for (auto const &[b, size] : deferSized) {
    snap.deferTier.emplace_back(b, used);
    if (used + size <= viewCacheSize)
      used += size;
  }
  snap.usedAfterAll = used;
  snap.valid = true;
}
} // namespace

void invalidateAdmissionSnapshot() { admissionSnapshot.valid = false; }

bool couldWinAdmission(boss::Symbol const &name, std::optional<double> cost) {
  auto it = viewRegistry.find(name);
  if (it == viewRegistry.end() || it->second.size <= 0.0)
    return true;
  auto &entry = it->second;
  // Safe to fallback to iso because iso is usually more expensive
  // than std, so it will overestimate the benefit and not cause the
  // view to get outright rejected from even being materialised.
  // Technically, if the currently evaluated candidate has std info but the
  // rest of the views in the snapshot are all iso-only, then this can be an
  // underestimate, but that is only an issue while std info is missing for the
  // views surrounding the candidate in the snapshot, which will gather their std
  // info the next time they are evaluated, so it is not a problem in practice.
  if (!cost)
    cost = trueCost(name, true);
  if (!cost)
    return true; // Missing info, so we can't make a decision, defer to VE2 where more information
                 // is available
  ageEntry(entry);
  double const myBenefit = (*cost - entry.reuseCost) * entry.importanceFactor / entry.size;

  if (!admissionSnapshot.valid)
    buildAdmissionSnapshot(); // Rebuild a valid snapshot

  // Mimic the admission process in VE2 WithCaches handler
  auto const &deferTier = admissionSnapshot.deferTier;
  auto pos = std::partition_point(deferTier.begin(), deferTier.end(),
                                  [&](auto const &e) { return e.first >= myBenefit; });
  double const used = pos == deferTier.end() ? admissionSnapshot.usedAfterAll : pos->second;
  return used + entry.size <= viewCacheSize;
}

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