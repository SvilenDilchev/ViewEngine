#include "ViewRegistry.hpp"

#include <stack>

std::unordered_map<std::string, ViewEntry> viewRegistry;
std::unordered_set<std::string> evaluationStack;
std::unordered_map<std::string, std::unordered_set<std::string>> tableToViews;
std::unordered_map<std::string, std::unordered_set<std::string>> viewToViews;

bool hasCycle(const std::string &newView, const std::string &current,
              std::unordered_set<std::string> &visited) {
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

// TODO: use the viewToViews reverse dependency index to optimize this instead of brute force
// scanning all views
void invalidateDependentCaches(const std::string &invalidatedView,
                               std::unordered_set<std::string> &seen) {
  for (auto &[name, entry] : viewRegistry) {
    if (seen.count(name))
      continue;
    if (entry.dependencies.count(invalidatedView)) {
      entry.cached = std::nullopt;
      seen.insert(name);
      invalidateDependentCaches(name, seen);
    }
  }
}

void findCandidateViews(const std::unordered_set<std::string> &tables,
                        std::unordered_set<std::string> &candidates) {

  std::stack<std::string> toVisit;
  std::unordered_set<std::string> expanded;

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