#include "cfd/mesh/Mesh.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <numbers>

namespace cfd::mesh {
namespace {

/// Rotate a vector a quarter turn anticlockwise.
constexpr Vec2 rotateLeft(const Vec2& v) noexcept { return Vec2{-v.y, v.x}; }
/// Rotate a vector a quarter turn clockwise.
constexpr Vec2 rotateRight(const Vec2& v) noexcept { return Vec2{v.y, -v.x}; }

/// Signed area and centroid of a polygon, both from the shoelace sums.
///
/// The centroid of a polygon is *not* the mean of its corners once the shape
/// is skewed; it is the area-weighted first moment,
///
///     C = (1 / 6A) * sum_i ( P_i + P_{i+1} ) * ( x_i y_{i+1} - x_{i+1} y_i )
///
/// The finite-volume method stores cell values at the centroid, so using the
/// corner average would introduce a first-order error in every skewed cell.
struct PolygonMoments {
  double signedArea{0.0};
  Vec2 centroid{};
};

PolygonMoments quadMoments(const Vec2& p0, const Vec2& p1, const Vec2& p2,
                           const Vec2& p3) noexcept {
  const std::array<Vec2, 4> p{p0, p1, p2, p3};

  double twiceArea = 0.0;
  double cx = 0.0;
  double cy = 0.0;
  for (std::size_t k = 0; k < 4; ++k) {
    const Vec2& a = p[k];
    const Vec2& b = p[(k + 1) % 4];
    const double w = cross(a, b);  // x_a*y_b - x_b*y_a
    twiceArea += w;
    cx += (a.x + b.x) * w;
    cy += (a.y + b.y) * w;
  }

  PolygonMoments moments;
  moments.signedArea = 0.5 * twiceArea;
  if (std::abs(twiceArea) > 0.0) {
    moments.centroid = Vec2{cx / (3.0 * twiceArea), cy / (3.0 * twiceArea)};
  } else {
    // Degenerate cell: fall back to the corner average so the value is at
    // least finite. The zero area is reported separately as an inversion.
    moments.centroid = (p0 + p1 + p2 + p3) * 0.25;
  }
  return moments;
}

}  // namespace

std::string_view toString(BoundaryType type) noexcept {
  switch (type) {
    case BoundaryType::Interior: return "Interior";
    case BoundaryType::Wall:     return "Wall";
    case BoundaryType::Farfield: return "Farfield";
    case BoundaryType::Outlet:   return "Outlet";
    case BoundaryType::WakeCut:  return "WakeCut";
  }
  return "Unknown";
}

std::size_t Mesh::countFaces(BoundaryType type) const {
  return static_cast<std::size_t>(std::count_if(
      faces_.begin(), faces_.end(),
      [type](const Face& face) { return face.boundary == type; }));
}

Result<Mesh> buildStructured(StructuredMeshSpec spec) {
  const int ni = spec.nodesI;
  const int nj = spec.nodesJ;

  if (ni < 2 || nj < 2) {
    return Error{ErrorCode::InvalidArgument,
                 std::format("a structured block needs at least 2x2 nodes, got {}x{}", ni, nj)};
  }
  const auto expectedNodes = static_cast<std::size_t>(ni) * static_cast<std::size_t>(nj);
  if (spec.nodes.size() != expectedNodes) {
    return Error{ErrorCode::InvalidArgument,
                 std::format("expected {} nodes for a {}x{} block, got {}", expectedNodes,
                             ni, nj, spec.nodes.size())};
  }
  const auto expectedJMin = static_cast<std::size_t>(ni - 1);
  if (spec.jMinBoundary.size() != expectedJMin) {
    return Error{ErrorCode::InvalidArgument,
                 std::format("expected {} j-min boundary entries, got {}", expectedJMin,
                             spec.jMinBoundary.size())};
  }
  if (!spec.jMinPartner.empty() && spec.jMinPartner.size() != expectedJMin) {
    return Error{ErrorCode::InvalidArgument,
                 std::format("expected {} j-min partner entries, got {}", expectedJMin,
                             spec.jMinPartner.size())};
  }

  Mesh mesh;
  mesh.nodesI_ = ni;
  mesh.nodesJ_ = nj;
  mesh.nodes_ = std::move(spec.nodes);

  const int cellsI = ni - 1;
  const int cellsJ = nj - 1;
  const auto cellCount = static_cast<std::size_t>(cellsI) * static_cast<std::size_t>(cellsJ);

  const auto nodeAt = [ni](int i, int j) { return j * ni + i; };
  const auto cellAt = [cellsI](int i, int j) { return j * cellsI + i; };

  // --- cells ---
  // Corners anticlockwise, which makes the shoelace area positive whenever the
  // (i, j) parametrisation is right-handed.
  mesh.cellNodes_.resize(cellCount);
  for (int j = 0; j < cellsJ; ++j) {
    for (int i = 0; i < cellsI; ++i) {
      mesh.cellNodes_[static_cast<std::size_t>(cellAt(i, j))] = {
          nodeAt(i, j), nodeAt(i + 1, j), nodeAt(i + 1, j + 1), nodeAt(i, j + 1)};
    }
  }

  // --- faces ---
  // Two families, laid out so a face index can be computed directly:
  //   j-faces run along i at constant j   -> index  j*(ni-1) + i
  //   i-faces run along j at constant i   -> index  offset + i*(nj-1) + j
  const auto jFaceCount = static_cast<std::size_t>(cellsI) * static_cast<std::size_t>(nj);
  const auto iFaceCount = static_cast<std::size_t>(ni) * static_cast<std::size_t>(cellsJ);
  const auto iFaceOffset = jFaceCount;

  mesh.faces_.resize(jFaceCount + iFaceCount);
  mesh.cellFaces_.resize(cellCount);

  const auto jFaceIndex = [cellsI](int i, int j) {
    return static_cast<std::size_t>(j) * static_cast<std::size_t>(cellsI) +
           static_cast<std::size_t>(i);
  };
  const auto iFaceIndex = [cellsJ, iFaceOffset](int i, int j) {
    return iFaceOffset + static_cast<std::size_t>(i) * static_cast<std::size_t>(cellsJ) +
           static_cast<std::size_t>(j);
  };

  for (int j = 0; j < nj; ++j) {
    for (int i = 0; i < cellsI; ++i) {
      Face& face = mesh.faces_[jFaceIndex(i, j)];
      face.nodes = {nodeAt(i, j), nodeAt(i + 1, j)};

      if (j == 0) {
        face.owner = cellAt(i, 0);
        face.neighbour = -1;
        face.boundary = spec.jMinBoundary[static_cast<std::size_t>(i)];
        if (!spec.jMinPartner.empty()) {
          const int partnerI = spec.jMinPartner[static_cast<std::size_t>(i)];
          face.partner = (partnerI >= 0 && partnerI < cellsI)
                             ? static_cast<int>(jFaceIndex(partnerI, 0))
                             : -1;
        }
      } else if (j == nj - 1) {
        face.owner = cellAt(i, cellsJ - 1);
        face.neighbour = -1;
        face.boundary = spec.jMaxBoundary;
      } else {
        face.owner = cellAt(i, j - 1);
        face.neighbour = cellAt(i, j);
        face.boundary = BoundaryType::Interior;
      }
    }
  }

  for (int i = 0; i < ni; ++i) {
    for (int j = 0; j < cellsJ; ++j) {
      Face& face = mesh.faces_[iFaceIndex(i, j)];
      face.nodes = {nodeAt(i, j), nodeAt(i, j + 1)};

      if (i == 0) {
        face.owner = cellAt(0, j);
        face.neighbour = -1;
        face.boundary = spec.iMinBoundary;
      } else if (i == ni - 1) {
        face.owner = cellAt(cellsI - 1, j);
        face.neighbour = -1;
        face.boundary = spec.iMaxBoundary;
      } else {
        face.owner = cellAt(i - 1, j);
        face.neighbour = cellAt(i, j);
        face.boundary = BoundaryType::Interior;
      }
    }
  }

  for (int j = 0; j < cellsJ; ++j) {
    for (int i = 0; i < cellsI; ++i) {
      mesh.cellFaces_[static_cast<std::size_t>(cellAt(i, j))] = {
          static_cast<int>(jFaceIndex(i, j)),      // below
          static_cast<int>(iFaceIndex(i + 1, j)),  // right
          static_cast<int>(jFaceIndex(i, j + 1)),  // above
          static_cast<int>(iFaceIndex(i, j)),      // left
      };
    }
  }

  // --- cell metrics ---
  mesh.cellAreas_.resize(cellCount);
  mesh.cellCentroids_.resize(cellCount);
  for (std::size_t c = 0; c < cellCount; ++c) {
    const std::array<int, 4>& n = mesh.cellNodes_[c];
    const PolygonMoments moments =
        quadMoments(mesh.nodes_[static_cast<std::size_t>(n[0])],
                    mesh.nodes_[static_cast<std::size_t>(n[1])],
                    mesh.nodes_[static_cast<std::size_t>(n[2])],
                    mesh.nodes_[static_cast<std::size_t>(n[3])]);
    mesh.cellAreas_[c] = moments.signedArea;
    mesh.cellCentroids_[c] = moments.centroid;
  }

  // --- face metrics ---
  // The normal always points out of the owner. For the two face families that
  // is a fixed quarter turn from the face tangent; only the sense differs, and
  // it flips on the two low-index boundaries where the owner sits on the far
  // side from the interior convention.
  mesh.faceCentres_.resize(mesh.faces_.size());
  mesh.faceNormals_.resize(mesh.faces_.size());
  mesh.faceAreas_.resize(mesh.faces_.size());

  for (std::size_t f = 0; f < mesh.faces_.size(); ++f) {
    const Face& face = mesh.faces_[f];
    const Vec2& a = mesh.nodes_[static_cast<std::size_t>(face.nodes[0])];
    const Vec2& b = mesh.nodes_[static_cast<std::size_t>(face.nodes[1])];

    const Vec2 tangent = b - a;
    const double len = length(tangent);

    mesh.faceCentres_[f] = (a + b) * 0.5;
    mesh.faceAreas_[f] = len;

    Vec2 normal{0.0, 0.0};
    if (len > 0.0) {
      const bool isJFace = f < jFaceCount;
      if (isJFace) {
        // Increasing j is a left turn from the face tangent.
        const int j = static_cast<int>(f / static_cast<std::size_t>(cellsI));
        normal = (j == 0) ? rotateRight(tangent) : rotateLeft(tangent);
      } else {
        // Increasing i is a right turn from the face tangent.
        const int i = static_cast<int>((f - iFaceOffset) / static_cast<std::size_t>(cellsJ));
        normal = (i == 0) ? rotateLeft(tangent) : rotateRight(tangent);
      }
      normal *= 1.0 / len;
    }
    mesh.faceNormals_[f] = normal;
  }

  // --- aggregate quantities and quality ---
  mesh.totalArea_ = 0.0;
  double minArea = std::numeric_limits<double>::max();
  double maxArea = std::numeric_limits<double>::lowest();
  double maxAspect = 0.0;
  std::size_t inverted = 0;

  for (std::size_t c = 0; c < cellCount; ++c) {
    const double area = mesh.cellAreas_[c];
    mesh.totalArea_ += area;
    minArea = std::min(minArea, area);
    maxArea = std::max(maxArea, area);
    if (!(area > 0.0)) {
      ++inverted;
    }

    const std::array<int, 4>& n = mesh.cellNodes_[c];
    const Vec2& p0 = mesh.nodes_[static_cast<std::size_t>(n[0])];
    const Vec2& p1 = mesh.nodes_[static_cast<std::size_t>(n[1])];
    const Vec2& p2 = mesh.nodes_[static_cast<std::size_t>(n[2])];
    const Vec2& p3 = mesh.nodes_[static_cast<std::size_t>(n[3])];

    // Mean length of each pair of opposite sides.
    const double alongI = 0.5 * (distance(p0, p1) + distance(p3, p2));
    const double alongJ = 0.5 * (distance(p1, p2) + distance(p0, p3));
    const double small = std::min(alongI, alongJ);
    if (small > 0.0) {
      maxAspect = std::max(maxAspect, std::max(alongI, alongJ) / small);
    }
  }

  // Non-orthogonality: the angle between a face normal and the line joining
  // the two centroids it separates. A gradient reconstructed across a face is
  // only exact when those are parallel.
  double maxNonOrtho = 0.0;
  for (std::size_t f = 0; f < mesh.faces_.size(); ++f) {
    const Face& face = mesh.faces_[f];
    if (!face.isInterior()) {
      continue;
    }
    const Vec2 link = mesh.cellCentroids_[static_cast<std::size_t>(face.neighbour)] -
                      mesh.cellCentroids_[static_cast<std::size_t>(face.owner)];
    const double linkLength = length(link);
    if (linkLength <= 0.0) {
      continue;
    }
    const double cosine = std::clamp(dot(mesh.faceNormals_[f], link) / linkLength, -1.0, 1.0);
    maxNonOrtho = std::max(maxNonOrtho, std::acos(cosine));
  }

  // Wall spacing: how far the first node sits off a wall face.
  double minWallSpacing = std::numeric_limits<double>::max();
  bool sawWall = false;
  for (int i = 0; i < cellsI; ++i) {
    if (spec.jMinBoundary[static_cast<std::size_t>(i)] != BoundaryType::Wall) {
      continue;
    }
    sawWall = true;
    minWallSpacing = std::min(minWallSpacing,
                              distance(mesh.nodes_[static_cast<std::size_t>(nodeAt(i, 0))],
                                       mesh.nodes_[static_cast<std::size_t>(nodeAt(i, 1))]));
  }

  mesh.quality_.minCellArea = (cellCount > 0) ? minArea : 0.0;
  mesh.quality_.maxCellArea = (cellCount > 0) ? maxArea : 0.0;
  mesh.quality_.maxAspectRatio = maxAspect;
  mesh.quality_.maxNonOrthogonalityDeg = maxNonOrtho * 180.0 / std::numbers::pi;
  mesh.quality_.invertedCells = inverted;
  mesh.quality_.minWallSpacing = sawWall ? minWallSpacing : 0.0;

  return mesh;
}

}  // namespace cfd::mesh
