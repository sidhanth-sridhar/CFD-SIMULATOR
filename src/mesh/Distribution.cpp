#include "cfd/mesh/Distribution.hpp"

#include <cmath>
#include <cstddef>

namespace cfd::mesh {

std::vector<double> uniformDistribution(int intervals) {
  if (intervals < 1) {
    return {};
  }
  std::vector<double> t(static_cast<std::size_t>(intervals) + 1, 0.0);
  for (int k = 0; k <= intervals; ++k) {
    t[static_cast<std::size_t>(k)] =
        static_cast<double>(k) / static_cast<double>(intervals);
  }
  t.back() = 1.0;
  return t;
}

double solveGeometricRatio(double first, double total, int intervals) noexcept {
  if (intervals <= 1 || first <= 0.0 || total <= 0.0) {
    return 1.0;
  }
  const double uniform = first * static_cast<double>(intervals);
  if (uniform >= total) {
    return 1.0;
  }

  const auto sum = [first, intervals](double r) {
    if (std::abs(r - 1.0) < 1e-12) {
      return first * static_cast<double>(intervals);
    }
    return first * (std::pow(r, static_cast<double>(intervals)) - 1.0) / (r - 1.0);
  };

  double lo = 1.0;
  double hi = 3.0;
  if (sum(hi) < total) {
    return hi;  // even aggressive growth cannot span it; caller renormalises
  }
  for (int iteration = 0; iteration < 200; ++iteration) {
    const double mid = 0.5 * (lo + hi);
    if (sum(mid) < total) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return 0.5 * (lo + hi);
}

std::vector<double> geometricDistribution(double firstSpacing, double total, int intervals) {
  if (intervals < 1) {
    return {};
  }
  std::vector<double> t(static_cast<std::size_t>(intervals) + 1, 0.0);
  const double ratio = solveGeometricRatio(firstSpacing, total, intervals);

  double step = firstSpacing;
  double accumulated = 0.0;
  for (int k = 1; k <= intervals; ++k) {
    accumulated += step;
    t[static_cast<std::size_t>(k)] = accumulated;
    step *= ratio;
  }

  if (accumulated > 0.0) {
    for (double& value : t) {
      value /= accumulated;
    }
  }
  t.front() = 0.0;
  t.back() = 1.0;
  return t;
}

std::vector<double> symmetricGeometricDistribution(double firstSpacing, double total,
                                                   int intervals) {
  if (intervals < 2) {
    return uniformDistribution(intervals);
  }

  // Build the lower half, then mirror it. Working on the half and reflecting
  // guarantees exact symmetry, which repeating the search on the upper half
  // would only approximate.
  const int half = intervals / 2;
  const std::vector<double> lower =
      geometricDistribution(firstSpacing, 0.5 * total, half);

  std::vector<double> t(static_cast<std::size_t>(intervals) + 1, 0.0);
  for (int k = 0; k <= half; ++k) {
    t[static_cast<std::size_t>(k)] = 0.5 * lower[static_cast<std::size_t>(k)];
  }
  for (int k = 0; k <= half; ++k) {
    t[static_cast<std::size_t>(intervals - k)] =
        1.0 - 0.5 * lower[static_cast<std::size_t>(k)];
  }

  // An odd interval count leaves the middle node unset; centre it.
  if (intervals % 2 != 0) {
    const auto middle = static_cast<std::size_t>(half + 1);
    t[middle] = 0.5 * (t[middle - 1] + t[middle + 1]);
  }

  t.front() = 0.0;
  t.back() = 1.0;
  return t;
}

}  // namespace cfd::mesh
