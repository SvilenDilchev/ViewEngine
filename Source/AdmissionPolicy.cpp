#include "AdmissionPolicy.hpp"
#include <algorithm>

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
    if (depEntry.cached) {
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

AdaptiveCachingDecision resolveAdaptiveCaching(ViewEntry &entry) {
  auto iso = isoCost(entry);
  if (!iso) {
    // Run isolated measurement to get instrumentation info and defer the caching decision to VE2
    // where we have more information.
    entry.admissionDecided = false;
    return AdaptiveCachingDecision{true, ExecutionStrategy::IsolatedMeasurement};
  }

  auto std_ = stdCost(entry);
  if (!std_) {
    // Run standard execution to get instrumentation info and defer the caching decision to VE2
    // where we have more information.
    entry.admissionDecided = false;
    return AdaptiveCachingDecision{true, ExecutionStrategy::Standard};
  }

  // We have enough information to make a caching decision in VE1
  entry.admissionDecided = true;
  ExecutionStrategy strategy =
      *iso < *std_ ? ExecutionStrategy::IsolatedMeasurement : ExecutionStrategy::Standard;

  // TODO: real benefit analysis to decide whether to cache or not. For now, we always cache.
  return AdaptiveCachingDecision{true, strategy};
}