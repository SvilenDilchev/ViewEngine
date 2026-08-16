#include "ViewRegistry.hpp"
#include "Cache.hpp"

std::unordered_map<boss::Symbol, ViewEntry> viewRegistry;
std::unordered_set<boss::Symbol> evaluationStack;
std::unordered_set<boss::Symbol> resolvedCacheRefs;
std::unordered_map<boss::Symbol, std::unordered_set<boss::Symbol>> tableToViews;
std::unordered_map<boss::Symbol, std::unordered_set<boss::Symbol>> viewToViews;
uint64_t veTick = 0;

void ageEntry(ViewEntry &entry) {
  uint64_t currentQuery = (veTick + 1) / 2; // VE1 and VE2 both result in the same query idx;
  if (currentQuery <= entry.importanceLastAgedQuery)
    return; // Still the same query, no need to age again

  // veTick is incremented twice per query
  uint64_t queriesElapsed = currentQuery - entry.importanceLastAgedQuery;
  entry.importanceFactor *= std::pow(IMPORTANCE_DECAY_FACTOR, static_cast<double>(queriesElapsed));
  entry.importanceLastAgedQuery = currentQuery;
}

bool hasCycle(const boss::Symbol &newView, const boss::Symbol &current,
              std::unordered_set<boss::Symbol> &visited) {
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

void invalidateDependentCaches(const boss::Symbol &invalidatedView,
                               std::unordered_set<boss::Symbol> &seen) {
  auto it = viewToViews.find(invalidatedView);
  if (it == viewToViews.end())
    return;

  for (const auto &dependent : it->second) {
    if (seen.count(dependent))
      continue;
    seen.insert(dependent);
    auto vit = viewRegistry.find(dependent);
    if (vit != viewRegistry.end()) {
      // TODO: invalidate instrumentation data as well
      viewCacheOccupancy -= vit->second.size;
      viewCache.erase(dependent); // Invalidate the cache for the dependent view
    }
    invalidateDependentCaches(dependent, seen);
  }
}

void storeIfPositive(double &target, const double newValue) {
  if (newValue > 0.0)
    target = newValue;
}
