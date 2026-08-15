#include "cfd/mesh/CGrid.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>
#include <vector>

#include "cfd/core/Log.hpp"
#include "cfd/mesh/Distribution.hpp"

namespace cfd::mesh {
namespace {

constexpr std::string_view kLogCategory = "mesh";

constexpr Vec2 rotateLeft(const Vec2& v) noexcept { return Vec2{-v.y, v.x}; }

Vec2 normalized(const Vec2& v) noexcept {
  const double len = length(v);
  return (len > 0.0) ? v * (1.0 / len) : Vec2{0.0, 0.0};
}

Vec2 rotate(const Vec2& v, double angle) noexcept {
  const double s = std::sin(angle);
  const double co = std::cos(angle);
  return Vec2{v.x * co - v.y * s, v.x * s + v.y * co};
}

/// Smooth step from 0 to 1 over [0, 1], with zero slope at both ends.
double smoothStep(double x) noexcept {
  const double t = std::clamp(x, 0.0, 1.0);
  return t * t * (3.0 - 2.0 * t);
}

/// Signed curvature of a polyline at each interior node, from the turning
/// angle between adjacent segments divided by the mean segment length.
///
/// The sign matters. With the traversal used here the outward normal is a left
/// turn from the tangent, so a negative curvature means the boundary is convex
/// as the fluid sees it and marching normals *diverge* - always safe. Positive
/// curvature means concave, and normals converge to a focus a distance 1/kappa
/// away. Beyond that focus the grid folds.
std::vector<double> signedCurvature(const std::vector<Vec2>& curve) {
  std::vector<double> kappa(curve.size(), 0.0);
  for (std::size_t i = 1; i + 1 < curve.size(); ++i) {
    const Vec2 back = curve[i] - curve[i - 1];
    const Vec2 forward = curve[i + 1] - curve[i];
    const double lenBack = length(back);
    const double lenForward = length(forward);
    if (lenBack <= 0.0 || lenForward <= 0.0) {
      continue;
    }
    const Vec2 tb = back * (1.0 / lenBack);
    const Vec2 tf = forward * (1.0 / lenForward);
    const double turn = std::atan2(cross(tb, tf), dot(tb, tf));
    kappa[i] = turn / (0.5 * (lenBack + lenForward));
  }
  return kappa;
}

}  // namespace

std::string_view toString(MeshResolution resolution) noexcept {
  switch (resolution) {
    case MeshResolution::Coarse: return "Coarse";
    case MeshResolution::Medium: return "Medium";
    case MeshResolution::Fine:   return "Fine";
  }
  return "Unknown";
}

CGridOptions optionsFor(MeshResolution resolution) {
  CGridOptions options;
  switch (resolution) {
    case MeshResolution::Coarse:
      options.surfacePoints = 80;
      options.wakePoints = 32;
      options.normalPoints = 40;
      options.firstLayerHeight = 1.0e-3;
      break;
    case MeshResolution::Medium:
      options.surfacePoints = 160;
      options.wakePoints = 56;
      options.normalPoints = 72;
      options.firstLayerHeight = 3.0e-4;
      break;
    case MeshResolution::Fine:
      options.surfacePoints = 320;
      options.wakePoints = 96;
      options.normalPoints = 128;
      options.firstLayerHeight = 1.0e-4;
      break;
  }
  return options;
}

Result<Mesh> generateCGrid(const geom::Airfoil& airfoil, const CGridOptions& options) {
  // --- validate ---
  if (airfoil.trailingEdgeStyle() != geom::TrailingEdge::Closed) {
    return Error{ErrorCode::InvalidArgument,
                 "a C-grid needs a closed trailing edge: the wake cut starts from a "
                 "single point, and a blunt base leaves a gap the topology cannot "
                 "represent. Set the trailing edge to Closed."};
  }
  if (options.surfacePoints < 8) {
    return Error{ErrorCode::InvalidArgument,
                 std::format("surfacePoints must be at least 8, got {}", options.surfacePoints)};
  }
  if (options.wakePoints < 2) {
    return Error{ErrorCode::InvalidArgument,
                 std::format("wakePoints must be at least 2, got {}", options.wakePoints)};
  }
  if (options.normalPoints < 3) {
    return Error{ErrorCode::InvalidArgument,
                 std::format("normalPoints must be at least 3, got {}", options.normalPoints)};
  }
  if (!(options.upstreamChords > 0.5) || !(options.downstreamChords > 0.5) ||
      !(options.verticalChords > 0.5)) {
    return Error{ErrorCode::InvalidArgument,
                 "domain extents must each be more than half a chord"};
  }
  if (!(options.firstLayerHeight > 0.0) || options.firstLayerHeight > 0.1) {
    return Error{ErrorCode::InvalidArgument,
                 std::format("firstLayerHeight must lie in (0, 0.1] chords, got {}",
                             options.firstLayerHeight)};
  }

  const double c = airfoil.chord();

  // --- the wall, re-evaluated at the mesh's own resolution ---
  const geom::AirfoilOptions wallOptions{
      .pointsPerSurface = options.surfacePoints,
      .chord = c,
      .trailingEdge = geom::TrailingEdge::Closed,
  };
  Result<geom::Airfoil> wallSection = geom::generate(airfoil.designation(), wallOptions);
  if (!wallSection) {
    return wallSection.error();
  }

  // The section's contour runs anticlockwise (trailing edge, upper surface,
  // leading edge, lower surface). The C-grid needs the opposite sense so that
  // "a left turn from the tangent" points away from the body, so reverse it.
  std::vector<Vec2> wall = wallSection.value().contour();
  std::reverse(wall.begin(), wall.end());

  const int wallNodes = static_cast<int>(wall.size());  // 2*surfacePoints - 1
  const int wakeNodes = options.wakePoints;
  const int ni = 2 * wakeNodes + wallNodes;
  const int nj = options.normalPoints;

  const int iTrailingLower = wakeNodes;                 // first wall node
  const int iTrailingUpper = wakeNodes + wallNodes - 1; // last wall node

  // --- wake cut stations, measured back from the trailing edge ---
  const double xTrailing = airfoil.trailingEdge().x;
  const double wakeLength = options.downstreamChords * c;

  // Start the wake with the same spacing the surface ends with, so cells do
  // not jump in size where the wall becomes a cut.
  const double trailingSpacing = std::max(distance(wall[0], wall[1]), 1e-9 * c);
  const std::vector<double> wakeFraction =
      geometricDistribution(trailingSpacing, wakeLength, wakeNodes);

  // The far field gets its *own*, much gentler distribution rather than
  // inheriting the one above.
  //
  // Copying the inner spacing outwards makes the grid lines in the wake
  // exactly vertical, which looks tidy and is a trap: the surface clustering
  // near the trailing edge is around 1e-4 chords, so the outermost cells end
  // up 1e-4 wide and a whole chord tall - aspect ratios of 10^4 out where the
  // flow is uniform and nothing needs resolving. Those cells make the pressure
  // equation violently stiff and were enough to diverge the solver outright.
  //
  // Starting from half the uniform spacing instead keeps far-field cells
  // well shaped. The grid lines shear rather than staying vertical, but only
  // by the difference in x over the full domain height - a fraction of a
  // percent, and invisible in the result.
  const double outerSpacing = wakeLength / (2.0 * static_cast<double>(wakeNodes));
  const std::vector<double> outerWakeFraction =
      geometricDistribution(outerSpacing, wakeLength, wakeNodes);

  // --- outer boundary shape ---
  const double halfHeight = options.verticalChords * c;
  const double frontRadius = xTrailing + options.upstreamChords * c;  // semi-axis in x

  // --- assemble the inner and outer boundary curves ---
  std::vector<Vec2> inner(static_cast<std::size_t>(ni));
  std::vector<Vec2> outer(static_cast<std::size_t>(ni));

  for (int i = 0; i < wakeNodes; ++i) {
    // Lower side of the cut, running upstream from the outflow towards the
    // trailing edge. Station wakeNodes - i, so i = 0 sits at the outflow and
    // the node nearest the trailing edge is the last one.
    const double s = wakeFraction[static_cast<std::size_t>(wakeNodes - i)] * wakeLength;
    const double x = xTrailing + s;
    const double outerS =
        outerWakeFraction[static_cast<std::size_t>(wakeNodes - i)] * wakeLength;
    const double outerX = xTrailing + outerS;

    inner[static_cast<std::size_t>(i)] = Vec2{x, 0.0};
    outer[static_cast<std::size_t>(i)] = Vec2{outerX, -halfHeight};

    // Upper side, mirrored, so the two sides of the cut coincide exactly.
    const int mirror = ni - 1 - i;
    inner[static_cast<std::size_t>(mirror)] = Vec2{x, 0.0};
    outer[static_cast<std::size_t>(mirror)] = Vec2{outerX, halfHeight};
  }

  for (int k = 0; k < wallNodes; ++k) {
    const int i = iTrailingLower + k;
    inner[static_cast<std::size_t>(i)] = wall[static_cast<std::size_t>(k)];

    // Half ellipse from directly below the trailing edge, round the front, to
    // directly above it. Uniform in the parameter angle: the wall is heavily
    // clustered at the nose, and copying that clustering out to the far field
    // would spend cells where nothing happens.
    const double phi = std::numbers::pi * static_cast<double>(k) /
                       static_cast<double>(wallNodes - 1);
    outer[static_cast<std::size_t>(i)] =
        Vec2{xTrailing - frontRadius * std::sin(phi), -halfHeight * std::cos(phi)};
  }

  // --- outward normals along the inner boundary ---
  std::vector<Vec2> normals(static_cast<std::size_t>(ni));
  for (int i = 0; i < ni; ++i) {
    const std::size_t u = static_cast<std::size_t>(i);
    Vec2 tangent{};
    if (i == 0) {
      tangent = inner[1] - inner[0];
    } else if (i == ni - 1) {
      tangent = inner[u] - inner[u - 1];
    } else {
      tangent = inner[u + 1] - inner[u - 1];
    }
    normals[u] = rotateLeft(normalized(tangent));
  }

  // Smoothing widens the corner where the wake cut meets the trailing edge.
  // The two surfaces arrive there at a few degrees to the cut, and an
  // unsmoothed kink focuses the marching normals within a few cell heights.
  // The outflow ends are pinned: they must stay exactly vertical.
  for (int pass = 0; pass < options.normalSmoothingPasses; ++pass) {
    std::vector<Vec2> smoothed = normals;
    for (int i = 1; i < ni - 1; ++i) {
      const std::size_t u = static_cast<std::size_t>(i);
      smoothed[u] = normalized(normals[u - 1] + normals[u] * 2.0 + normals[u + 1]);
    }
    normals = std::move(smoothed);
  }

  // --- how far the wall-normal zone may extend before the grid would fold ---
  const std::vector<double> kappa = signedCurvature(inner);
  const double blendLimit = options.normalBlendLength * c;
  std::vector<double> blendLength(static_cast<std::size_t>(ni), blendLimit);
  for (int i = 0; i < ni; ++i) {
    const std::size_t u = static_cast<std::size_t>(i);
    if (kappa[u] > 0.0) {
      // Keep well inside the focal distance 1/kappa where normals cross.
      blendLength[u] = std::min(blendLimit, 0.3 / kappa[u]);
    }
  }
  // Propagate the restriction along the boundary with a bounded growth rate.
  //
  // Averaging is the wrong tool here, and was the cause of a stubborn fold at
  // the trailing edge: it can lift a station's value back above the limit its
  // own curvature demands, quietly undoing the constraint. A Lipschitz sweep -
  // one pass forward, one back - instead lets the zone thicken only gradually
  // with distance from a constrained station. The result is smooth *and*
  // never exceeds the limit anywhere, which averaging cannot promise.
  constexpr double kGrowthRate = 0.25;
  for (int pass = 0; pass < 2; ++pass) {
    for (int i = 1; i < ni; ++i) {
      const std::size_t u = static_cast<std::size_t>(i);
      blendLength[u] = std::min(blendLength[u],
                                blendLength[u - 1] + kGrowthRate * distance(inner[u], inner[u - 1]));
    }
    for (int i = ni - 2; i >= 0; --i) {
      const std::size_t u = static_cast<std::size_t>(i);
      blendLength[u] = std::min(blendLength[u],
                                blendLength[u + 1] + kGrowthRate * distance(inner[u], inner[u + 1]));
    }
  }

  // --- march outwards ---
  //
  // Each grid line leaves the wall along its normal and turns to point at its
  // far-field node. Two things about *how* it turns decide whether the grid is
  // usable, and getting either wrong folds it:
  //
  //   1. Rotate by a constant angular rate, not by interpolating the two
  //      direction vectors. A normalised lerp between unit vectors separated
  //      by an angle D sweeps at up to 2*tan(D/2) per unit of weight, which
  //      runs away as D approaches 180 degrees. Rotating directly is 1:1.
  //
  //   2. Spread the turn over the *logarithm* of distance. A line at distance
  //      r turning at rate dtheta/dr traces a curve whose own direction is
  //      d + r * dd/dr; once r * |dtheta/dr| exceeds 1 the line doubles back
  //      and the cells behind it invert. Because r * d/dr(f(r/L)) depends only
  //      on r/L, widening a linear blend zone does not help at all - the
  //      product is scale invariant. Blending in log r instead gives
  //      r * dw/dr = f'/ln(outer/inner), which can be made as small as we like
  //      by widening the *ratio* of the two radii.
  //
  // Together these give the condition ln(outer/inner) > |turn| * max(f'), and
  // the span below is chosen to satisfy it with margin at every station.
  const double firstLayer = options.firstLayerHeight * c;
  // Cells within a few first layers of the wall stay exactly orthogonal to it:
  // that is the region a boundary layer will occupy.
  const double orthogonalZone = 5.0 * firstLayer;

  std::vector<Vec2> nodes(static_cast<std::size_t>(ni) * static_cast<std::size_t>(nj));
  double largestTurn = 0.0;

  for (int i = 0; i < ni; ++i) {
    const std::size_t u = static_cast<std::size_t>(i);
    const Vec2 start = inner[u];
    const Vec2 finish = outer[u];
    const Vec2 span = finish - start;
    const double distanceOut = length(span);
    const Vec2 straight = normalized(span);
    const Vec2 wallNormal = normals[u];

    // Signed angle the line has to turn through, from the wall normal round to
    // the direction of its far-field node.
    const double turn =
        std::atan2(cross(wallNormal, straight), dot(wallNormal, straight));
    largestTurn = std::max(largestTurn, std::abs(turn));

    // smoothStep peaks at f' = 1.5, so a span of 1.5*|turn| is the break-even
    // point; 2.5 leaves a comfortable margin.
    const double requiredSpan = std::max(1.5, 2.5 * std::abs(turn));
    const double outerBlend = std::max(blendLength[u], 1e-9 * c);
    const double innerBlend = std::min(orthogonalZone, outerBlend * std::exp(-requiredSpan));
    const double logSpan = std::log(outerBlend / innerBlend);

    const std::vector<double> t = geometricDistribution(firstLayer, distanceOut, nj - 1);

    for (int j = 0; j < nj; ++j) {
      const double reach = t[static_cast<std::size_t>(j)] * distanceOut;

      double w = 1.0;
      if (reach <= innerBlend) {
        w = 0.0;
      } else if (reach < outerBlend) {
        w = smoothStep(std::log(reach / innerBlend) / logSpan);
      }

      // At w = 1 this is exactly `straight`, so the outermost node lands on
      // the far-field boundary to the last bit.
      const Vec2 direction = rotate(wallNormal, w * turn);

      nodes[static_cast<std::size_t>(j) * static_cast<std::size_t>(ni) + u] =
          start + direction * reach;
    }
  }

  // --- boundary tagging along j = 0 ---
  StructuredMeshSpec spec;
  spec.nodesI = ni;
  spec.nodesJ = nj;
  spec.nodes = std::move(nodes);
  spec.jMaxBoundary = BoundaryType::Farfield;
  spec.iMinBoundary = BoundaryType::Outlet;
  spec.iMaxBoundary = BoundaryType::Outlet;
  spec.jMinBoundary.assign(static_cast<std::size_t>(ni - 1), BoundaryType::Wall);
  spec.jMinPartner.assign(static_cast<std::size_t>(ni - 1), -1);

  for (int k = 0; k < ni - 1; ++k) {
    const std::size_t u = static_cast<std::size_t>(k);
    const bool onWall = (k >= iTrailingLower && k < iTrailingUpper);
    if (onWall) {
      spec.jMinBoundary[u] = BoundaryType::Wall;
    } else {
      spec.jMinBoundary[u] = BoundaryType::WakeCut;
      // Face k spans nodes k and k+1; those coincide with nodes ni-1-k and
      // ni-2-k, which is the face indexed ni-2-k.
      spec.jMinPartner[u] = ni - 2 - k;
    }
  }

  Result<Mesh> mesh = buildStructured(std::move(spec));
  if (!mesh) {
    return mesh.error();
  }

  const MeshQuality& quality = mesh.value().quality();
  CFD_LOG_DEBUG(kLogCategory,
                "C-grid {}x{}: {} cells, {} faces, area {:.4g} m^2, min cell {:.3e}, "
                "aspect {:.1f}, non-orthogonality {:.1f} deg, max turn {:.1f} deg, "
                "inverted {}",
                ni, nj, mesh.value().cellCount(), mesh.value().faceCount(),
                mesh.value().totalArea(), quality.minCellArea, quality.maxAspectRatio,
                quality.maxNonOrthogonalityDeg, largestTurn * 180.0 / std::numbers::pi,
                quality.invertedCells);

  return mesh;
}

}  // namespace cfd::mesh
