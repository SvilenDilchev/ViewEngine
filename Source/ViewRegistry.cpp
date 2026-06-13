#include "ViewRegistry.hpp"

std::unordered_map<boss::Symbol, ViewEntry> viewRegistry;
std::unordered_set<boss::Symbol> evaluationStack;
std::unordered_map<boss::Symbol, std::unordered_set<boss::Symbol>> tableToViews;
std::unordered_map<boss::Symbol, std::unordered_set<boss::Symbol>> viewToViews;

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
    if (vit != viewRegistry.end())
      vit->second.cached = std::nullopt;
    invalidateDependentCaches(dependent, seen);
  }
}