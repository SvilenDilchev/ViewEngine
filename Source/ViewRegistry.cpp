#include "ViewRegistry.hpp"
#include "Cache.hpp"

EngineMode engineMode = EngineMode::Full;
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

void invalidateDependants(const boss::Symbol &invalidatedView,
                          std::unordered_set<boss::Symbol> &seen) {
  auto it = viewToViews.find(invalidatedView);
  if (it == viewToViews.end())
    return;

  for (const auto &dependant : it->second) {
    if (seen.count(dependant))
      continue;
    seen.insert(dependant);
    auto vit = viewRegistry.find(dependant);
    if (vit != viewRegistry.end()) {
      if (viewCache.count(dependant)) {
        viewCacheOccupancy -= vit->second.size;
        viewCache.erase(dependant); // Invalidate the cache for the dependant view
      }
      // Invalidate the stale cost and size metrics for the dependant view as well
      vit->second.computeCost = 0.0;
      vit->second.materialiseCost = 0.0;
      vit->second.reuseCost = 0.0;
      vit->second.marginalComputeCost = 0.0;
      vit->second.size = 0.0;
    }
    invalidateDependants(dependant, seen);
  }
}

std::unordered_set<boss::Symbol> unionExpanded(const std::unordered_set<boss::Symbol> &baseTables,
                                               const std::unordered_set<boss::Symbol> &dependencies,
                                               std::unordered_set<boss::Symbol> &visited) {
  std::unordered_set<boss::Symbol> result = baseTables;
  for (const auto &dep : dependencies) {
    auto depTables = expandBaseTables(dep, visited);
    result.insert(depTables.begin(), depTables.end());
  }
  return result;
}

std::unordered_set<boss::Symbol> expandBaseTables(const boss::Symbol &viewName,
                                                  std::unordered_set<boss::Symbol> &visited) {
  auto it = viewRegistry.find(viewName);
  if (it == viewRegistry.end())
    return {};
  if (!visited.insert(viewName).second)
    return it->second.expandedBaseTables;
  it->second.expandedBaseTables =
      unionExpanded(it->second.signature.baseTables, it->second.dependencies, visited);
  return it->second.expandedBaseTables;
}

void storeCostSample(double &target, const double newValue) {
  if (newValue <= 0.0)
    return;
  target = target > 0.0 ? COST_EMA_ALPHA * newValue + (1.0 - COST_EMA_ALPHA) * target : newValue;
}
