#include "cfd/post/SurfaceData.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>

namespace cfd::post {
namespace {

/// One wall face, before it is split into upper and lower surfaces.
struct WallFace {
  std::size_t face{0};
  std::size_t owner{0};
  Vec2 centre{};
  Vec2 tangent{};  ///< along increasing i, i.e. the contour direction
  Vec2 normal{};   ///< away from the wall, into the fluid
  double length{0.0};
};

/// Collect the wall faces in contour order.
///
/// The C-grid stores its j = 0 faces consecutively along i, and that run is
/// exactly the section's contour: trailing edge, round the lower surface, over
/// the nose, back along the upper surface. Walking it in index order therefore
/// gives a continuous path, which is what an arc length needs.
std::vector<WallFace> collectWallFaces(const mesh::Mesh& mesh) {
  std::vector<WallFace> wall;
  const int cellsI = mesh.nodesI() - 1;
  wall.reserve(static_cast<std::size_t>(std::max(cellsI, 0)));

  for (int i = 0; i < cellsI; ++i) {
    const auto f = static_cast<std::size_t>(i);  // j = 0 faces come first
    if (mesh.faces()[f].boundary != mesh::BoundaryType::Wall) {
      continue;
    }

    const mesh::Face& face = mesh.faces()[f];
    const Vec2& a = mesh.nodes()[static_cast<std::size_t>(face.nodes[0])];
    const Vec2& b = mesh.nodes()[static_cast<std::size_t>(face.nodes[1])];
    const Vec2 along = b - a;
    const double len = length(along);
    if (!(len > 0.0)) {
      continue;
    }

    WallFace entry;
    entry.face = f;
    entry.owner = static_cast<std::size_t>(face.owner);
    entry.centre = mesh.faceCentres()[f];
    entry.tangent = along * (1.0 / len);
    // The mesh normal points out of the domain, which at a wall means into the
    // solid. The fluid side is the other way.
    entry.normal = mesh.faceNormals()[f] * -1.0;
    entry.length = len;
    wall.push_back(entry);
  }
  return wall;
}

/// Fill in everything measurable at one station.
SurfacePoint evaluate(const mesh::Mesh& mesh, const flow::FlowField& field,
                      const WallFace& wall, Vec2 tangent, double dynamicPressure,
                      double referencePressure, double chord) {
  SurfacePoint point;
  point.position = wall.centre;
  point.tangent = tangent;
  point.normal = wall.normal;
  point.chordFraction = (chord > 0.0) ? wall.centre.x / chord : wall.centre.x;

  point.pressure = field.pressure[wall.owner];
  point.pressureCoefficient =
      (dynamicPressure > 0.0) ? (point.pressure - referencePressure) / dynamicPressure : 0.0;

  // Perpendicular distance from the cell centroid to the wall. Using the
  // straight-line distance instead would overestimate the gradient wherever
  // the cell is skewed, which near a wall is most of them.
  const Vec2 offset = mesh.cellCentroids()[wall.owner] - wall.centre;
  const double wallDistance = std::abs(dot(offset, wall.normal));

  // Velocity of the first cell, resolved parallel to the surface. The normal
  // component carries no shear.
  const Vec2& cellVelocity = field.velocity[wall.owner];
  const Vec2 parallel = cellVelocity - wall.normal * dot(cellVelocity, wall.normal);
  point.nearWallSpeed = length(parallel);

  if (wallDistance > 0.0) {
    // The wall itself is at zero velocity, so the gradient across the first
    // cell is just the cell value over the distance.
    const double gradient = dot(parallel, tangent) / wallDistance;
    point.wallShear = field.viscosity[wall.owner] * gradient;
  }
  point.skinFriction =
      (dynamicPressure > 0.0) ? point.wallShear / dynamicPressure : 0.0;
  point.reversed = point.wallShear < 0.0;

  return point;
}

/// Walk a surface looking for the first place the wall shear changes sign.
///
/// Interpolated between the two stations either side of the crossing rather
/// than snapped to a cell, so the reported location does not jump around as
/// the mesh is refined.
SeparationPoint findSeparation(const std::vector<SurfacePoint>& surface) {
  SeparationPoint result;
  for (std::size_t i = 1; i < surface.size(); ++i) {
    if (surface[i].nearStagnation || surface[i - 1].nearStagnation) {
      continue;  // the sign either side of the dividing point means nothing
    }
    const double before = surface[i - 1].wallShear;
    const double after = surface[i].wallShear;
    if (!(before > 0.0) || !(after < 0.0)) {
      continue;
    }

    const double span = before - after;
    const double fraction = (span > 0.0) ? before / span : 0.0;

    result.found = true;
    result.chordFraction = surface[i - 1].chordFraction +
                           fraction * (surface[i].chordFraction - surface[i - 1].chordFraction);
    result.arcLength =
        surface[i - 1].arcLength + fraction * (surface[i].arcLength - surface[i - 1].arcLength);
    result.position = surface[i - 1].position +
                      (surface[i].position - surface[i - 1].position) * fraction;
    return result;
  }
  return result;
}

}  // namespace

Result<SurfaceDistribution> extractSurface(const mesh::Mesh& mesh,
                                           const flow::FlowField& field,
                                           const flow::FreestreamConditions& freestream,
                                           double chord) {
  if (!mesh.isStructured()) {
    return Error{ErrorCode::NotImplemented,
                 "surface extraction walks the wall faces in contour order, which needs "
                 "a structured mesh"};
  }
  if (!field.isConsistent() || field.size() != mesh.cellCount()) {
    return Error{ErrorCode::InvalidArgument,
                 std::format("field has {} cells but the mesh has {}", field.size(),
                             mesh.cellCount())};
  }
  if (const Status valid = freestream.validate(); !valid) {
    return valid.error();
  }

  const std::vector<WallFace> wall = collectWallFaces(mesh);
  if (wall.size() < 4) {
    return Error{ErrorCode::NotFound, "the mesh has no aerofoil surface to extract"};
  }

  // Split the contour at the leading edge *node*, not at the nearest face.
  //
  // The nose sits on a node, with one face either side of it. Assigning that
  // pair to different surfaces is what makes station i on one surface the
  // mirror of station i on the other, which is the only way a symmetric
  // section can produce two identical distributions. Sharing a face instead
  // offsets the two lists by half a cell and quietly breaks that symmetry.
  std::size_t leadingEdge = 0;  // index of the first face on the upper surface
  {
    double furthestForward = std::numeric_limits<double>::max();
    for (std::size_t i = 0; i < wall.size(); ++i) {
      // Node between face i-1 and face i, i.e. this face's first endpoint.
      const Vec2& node =
          mesh.nodes()[static_cast<std::size_t>(mesh.faces()[wall[i].face].nodes[0])];
      if (node.x < furthestForward) {
        furthestForward = node.x;
        leadingEdge = i;
      }
    }
  }

  const double dynamicPressure = freestream.dynamicPressure();
  SurfaceDistribution distribution;
  distribution.chord = chord;

  // Locate the stagnation point: the station of highest surface pressure, which
  // is where the oncoming flow is brought to rest and divides. It is the origin
  // of both boundary layers, and at incidence it is not the geometric leading
  // edge - it slides aft along the lower surface.
  std::size_t stagnation = 0;
  double highestPressure = -std::numeric_limits<double>::max();
  for (std::size_t i = 0; i < wall.size(); ++i) {
    const double pressure = field.pressure[wall[i].owner];
    if (pressure > highestPressure) {
      highestPressure = pressure;
      stagnation = i;
    }
  }
  distribution.stagnationPosition = wall[stagnation].centre;
  distribution.stagnationChordFraction =
      (chord > 0.0) ? wall[stagnation].centre.x / chord : wall[stagnation].centre.x;

  // The contour runs trailing edge -> lower -> leading edge -> upper ->
  // trailing edge, so its own tangent points towards the upper trailing edge.
  // The boundary layer runs *away* from the stagnation point in both
  // directions, so stations before it take the opposite tangent. This one rule
  // covers positive and negative incidence alike.
  const auto flowTangent = [&](std::size_t i) {
    if (i == stagnation) {
      // The dividing streamline lands somewhere inside this face, so part of
      // it feeds each boundary layer and "away from stagnation" has no single
      // answer. Give it the direction of its own surface, so its sign at least
      // agrees with its neighbours on a plot; the value itself is flagged as
      // meaningless through nearStagnation.
      return (i >= leadingEdge) ? wall[i].tangent : wall[i].tangent * -1.0;
    }
    return (i > stagnation) ? wall[i].tangent : wall[i].tangent * -1.0;
  };

  distribution.lower.reserve(leadingEdge);
  for (std::size_t i = leadingEdge; i-- > 0;) {
    distribution.lower.push_back(evaluate(mesh, field, wall[i], flowTangent(i),
                                          dynamicPressure, freestream.referencePressure,
                                          chord));
    distribution.lower.back().nearStagnation = (i == stagnation);
  }
  distribution.upper.reserve(wall.size() - leadingEdge);
  for (std::size_t i = leadingEdge; i < wall.size(); ++i) {
    distribution.upper.push_back(evaluate(mesh, field, wall[i], flowTangent(i),
                                          dynamicPressure, freestream.referencePressure,
                                          chord));
    distribution.upper.back().nearStagnation = (i == stagnation);
  }

  // The dividing station carries no usable sign, so it is never "reversed".
  for (std::vector<SurfacePoint>* side : {&distribution.upper, &distribution.lower}) {
    for (SurfacePoint& point : *side) {
      if (point.nearStagnation) {
        point.reversed = false;
      }
    }
  }

  // Arc length, accumulated from the leading edge outwards along each surface.
  const auto accumulate = [](std::vector<SurfacePoint>& surface) {
    double travelled = 0.0;
    for (std::size_t i = 0; i < surface.size(); ++i) {
      if (i > 0) {
        travelled += distance(surface[i - 1].position, surface[i].position);
      }
      surface[i].arcLength = travelled;
    }
  };
  accumulate(distribution.lower);
  accumulate(distribution.upper);

  distribution.upperSeparation = findSeparation(distribution.upper);
  distribution.lowerSeparation = findSeparation(distribution.lower);

  double minCp = std::numeric_limits<double>::max();
  double maxCp = std::numeric_limits<double>::lowest();
  double minCf = std::numeric_limits<double>::max();
  double maxCf = std::numeric_limits<double>::lowest();
  for (const std::vector<SurfacePoint>* side : {&distribution.upper, &distribution.lower}) {
    for (const SurfacePoint& point : *side) {
      minCp = std::min(minCp, point.pressureCoefficient);
      maxCp = std::max(maxCp, point.pressureCoefficient);
      minCf = std::min(minCf, point.skinFriction);
      maxCf = std::max(maxCf, point.skinFriction);
    }
  }
  distribution.minPressureCoefficient = minCp;
  distribution.maxPressureCoefficient = maxCp;
  distribution.minSkinFriction = minCf;
  distribution.maxSkinFriction = maxCf;

  return distribution;
}

}  // namespace cfd::post
