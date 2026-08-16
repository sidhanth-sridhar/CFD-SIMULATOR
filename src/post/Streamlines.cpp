#include "cfd/post/Streamlines.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>

namespace cfd::post {
namespace {

/// Signed area of the triangle (a, b, c), times two. Positive when the three
/// are in anticlockwise order.
double turn(const Vec2& a, const Vec2& b, const Vec2& c) noexcept {
  return cross(b - a, c - a);
}

/// How far outside edge `k` of a cell the point lies, negative when inside.
///
/// Cell corners are stored anticlockwise, so a point inside the cell is to the
/// left of every edge in turn.
double edgeDistance(const mesh::Mesh& mesh, std::size_t cell, int k, Vec2 point) noexcept {
  const std::array<int, 4>& corners = mesh.cellNodes()[cell];
  const auto first = static_cast<std::size_t>(k);
  const auto second = static_cast<std::size_t>((k + 1) % 4);
  const Vec2& a = mesh.nodes()[static_cast<std::size_t>(corners[first])];
  const Vec2& b = mesh.nodes()[static_cast<std::size_t>(corners[second])];
  return -turn(a, b, point);
}

bool contains(const mesh::Mesh& mesh, std::size_t cell, Vec2 point) noexcept {
  for (int k = 0; k < 4; ++k) {
    if (edgeDistance(mesh, cell, k, point) > 0.0) {
      return false;
    }
  }
  return true;
}

/// Rough size of a cell, for choosing a step length.
double cellScale(const mesh::Mesh& mesh, std::size_t cell) noexcept {
  return std::sqrt(std::max(mesh.cellAreas()[cell], 0.0));
}

}  // namespace

int locateCell(const mesh::Mesh& mesh, Vec2 point, int hint) {
  if (mesh.cellCount() == 0) {
    return -1;
  }

  if (hint >= 0 && hint < static_cast<int>(mesh.cellCount())) {
    // Walk towards the point: at each step leave through whichever edge the
    // point is furthest beyond. Bounded, because a walk can circle on a badly
    // shaped mesh rather than converging.
    int current = hint;
    for (int step = 0; step < 4096; ++step) {
      const auto cell = static_cast<std::size_t>(current);
      int worstEdge = -1;
      double worst = 0.0;
      for (int k = 0; k < 4; ++k) {
        const double outside = edgeDistance(mesh, cell, k, point);
        if (outside > worst) {
          worst = outside;
          worstEdge = k;
        }
      }
      if (worstEdge < 0) {
        return current;  // inside
      }

      const auto f = static_cast<std::size_t>(mesh.cellFaces()[cell][static_cast<std::size_t>(worstEdge)]);
      const mesh::Face& face = mesh.faces()[f];
      // Crossing the wake cut has to work: it is a slit, not a barrier, and a
      // streamline in the wake runs straight along it.
      const int across = mesh::oppositeCell(mesh, f);
      const int next = (face.owner == current) ? across : face.owner;
      if (next < 0) {
        return -1;  // walked out through a real boundary
      }
      current = next;
    }
  }

  // No usable hint: fall back to a scan. Slow, but only ever used to place a
  // seed, and a walk from the wrong side of the mesh can fail entirely.
  for (std::size_t c = 0; c < mesh.cellCount(); ++c) {
    if (contains(mesh, c, point)) {
      return static_cast<int>(c);
    }
  }
  return -1;
}

Result<std::vector<Streamline>> traceStreamlines(const mesh::Mesh& mesh,
                                                 const flow::FlowField& field,
                                                 const StreamlineOptions& options) {
  if (!field.isConsistent() || field.size() != mesh.cellCount()) {
    return Error{ErrorCode::InvalidArgument,
                 std::format("field has {} cells but the mesh has {}", field.size(),
                             mesh.cellCount())};
  }
  if (options.maxSteps < 1 || !(options.stepFraction > 0.0)) {
    return Error{ErrorCode::InvalidArgument, "streamline step settings are unusable"};
  }

  const double stallSpeed =
      std::max(options.stallFraction, 0.0) * std::max(options.referenceSpeed, 0.0);

  std::vector<Streamline> lines;
  lines.reserve(options.seeds.size());

  for (const Vec2& seed : options.seeds) {
    const int seedCell = locateCell(mesh, seed, -1);
    if (seedCell < 0) {
      continue;  // seeded outside the domain
    }

    // Trace backwards first, then forwards, so the finished curve reads in
    // the direction of the flow.
    Streamline backward;
    Streamline forward;

    for (int pass = 0; pass < 2; ++pass) {
      const bool downstream = (pass == 1);
      if (!downstream && !options.traceBackwards) {
        continue;
      }
      const double sense = downstream ? 1.0 : -1.0;

      Vec2 position = seed;
      int cell = seedCell;
      Streamline& target = downstream ? forward : backward;
      target.push_back(position);

      for (int step = 0; step < options.maxSteps; ++step) {
        // Classical fourth-order Runge-Kutta. The velocity is piecewise
        // constant per cell, so the extra stages mostly buy a smooth path
        // through the cell rather than formal accuracy, which is what a
        // picture needs.
        const auto velocityAt = [&](Vec2 point, int& carriedCell) {
          const int found = locateCell(mesh, point, carriedCell);
          if (found < 0) {
            return Vec2{0.0, 0.0};
          }
          carriedCell = found;
          return field.velocity[static_cast<std::size_t>(found)] * sense;
        };

        int probe = cell;
        const Vec2 k1 = velocityAt(position, probe);
        const double speed = length(k1);
        if (!(speed > stallSpeed)) {
          break;
        }

        const double step0 = options.stepFraction * cellScale(mesh, static_cast<std::size_t>(cell));
        const double dt = (speed > 0.0) ? step0 / speed : 0.0;
        if (!(dt > 0.0)) {
          break;
        }

        int probe2 = probe;
        const Vec2 k2 = velocityAt(position + k1 * (0.5 * dt), probe2);
        int probe3 = probe2;
        const Vec2 k3 = velocityAt(position + k2 * (0.5 * dt), probe3);
        int probe4 = probe3;
        const Vec2 k4 = velocityAt(position + k3 * dt, probe4);

        const Vec2 advance = (k1 + k2 * 2.0 + k3 * 2.0 + k4) * (dt / 6.0);
        const Vec2 next = position + advance;

        const int nextCell = locateCell(mesh, next, cell);
        if (nextCell < 0) {
          break;  // left the domain
        }
        position = next;
        cell = nextCell;
        target.push_back(position);
      }
    }

    if (backward.size() + forward.size() < 4) {
      continue;
    }

    Streamline line;
    line.reserve(backward.size() + forward.size());
    line.insert(line.end(), backward.rbegin(), backward.rend());
    // The seed itself appears in both halves.
    line.insert(line.end(), forward.begin() + (forward.empty() ? 0 : 1), forward.end());
    lines.push_back(std::move(line));
  }

  return lines;
}

}  // namespace cfd::post
