#include "ViewRegistry.hpp"
#include <variant>

std::unordered_map<boss::Symbol, ViewEntry> viewRegistry;
std::unordered_set<boss::Symbol> evaluationStack;
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
    if (vit != viewRegistry.end())
      vit->second.cached = std::nullopt;
    invalidateDependentCaches(dependent, seen);
  }
}

void storeIfPositive(double &target, const double newValue) {
  if (newValue > 0.0)
    target = newValue;
}

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