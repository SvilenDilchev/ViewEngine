#include "ViewRegistry.hpp"

#include <stack>

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

void findCandidateViews(const std::unordered_set<boss::Symbol> &tables,
                        std::unordered_set<boss::Symbol> &candidates) {

  std::stack<boss::Symbol> toVisit;
  std::unordered_set<boss::Symbol> expanded;

  // Add direct candidates based on table index
  for (const auto &table : tables)
    if (auto it = tableToViews.find(table); it != tableToViews.end())
      for (const auto &viewName : it->second)
        if (candidates.insert(viewName).second)
          toVisit.push(viewName);

  // DFS to find indirect candidates via view dependencies
  while (!toVisit.empty()) {
    auto current = toVisit.top();
    toVisit.pop();
    if (!expanded.insert(current).second)
      continue; // already expanded this view, skip
    if (auto it = viewToViews.find(current); it != viewToViews.end())
      for (const auto &dependent : it->second)
        if (candidates.insert(dependent).second)
          toVisit.push(dependent);
  }
}