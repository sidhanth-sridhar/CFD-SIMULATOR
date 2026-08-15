// Distribution.hpp - how nodes are spaced along a line.
//
// Grid generators differ in shape but share the same one-dimensional problem:
// place n+1 nodes across a span so the spacing starts at some prescribed value
// and grows smoothly. Both the C-grid's wall-normal marching and the box
// mesh's wall grading need it, so it lives here rather than in either.
//
// All functions return normalised positions running from exactly 0 to exactly
// 1, strictly increasing.

#pragma once

#include <vector>

namespace cfd::mesh {

/// Evenly spaced nodes. `intervals` must be at least 1.
[[nodiscard]] std::vector<double> uniformDistribution(int intervals);

/// Ratio r of a geometric series with `intervals` terms, first term `first`,
/// summing to `total`. Solved by bisection.
///
/// Near a wall the spacing has to start very small and end up comparable to
/// the far-field spacing - often four orders of magnitude apart. A geometric
/// progression bridges that smoothly: each layer is a fixed multiple of the
/// one before, so the solver never meets an abrupt jump in cell size. Fixing
/// the first layer and the total distance determines the ratio implicitly,
/// through
///
///     first * (r^n - 1) / (r - 1) = total
///
/// which has no closed form. The sum is monotone in r, so bisection cannot
/// fail. Returns 1 when the requested first spacing already fills the span.
[[nodiscard]] double solveGeometricRatio(double first, double total, int intervals) noexcept;

/// Nodes clustered at the start, spacing growing geometrically from
/// `firstSpacing` across a span of `total`.
///
/// The result is renormalised so the last node lands exactly on 1, which also
/// absorbs the small error left when the ratio search hits its bracket.
[[nodiscard]] std::vector<double> geometricDistribution(double firstSpacing, double total,
                                                        int intervals);

/// Nodes clustered at *both* ends, for a channel with a wall on each side.
/// Built on the first half and mirrored, so the result is exactly symmetric
/// about the midpoint.
[[nodiscard]] std::vector<double> symmetricGeometricDistribution(double firstSpacing,
                                                                 double total, int intervals);

}  // namespace cfd::mesh
