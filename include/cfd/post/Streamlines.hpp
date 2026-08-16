// Streamlines.hpp - curves everywhere tangent to the velocity field.
//
// A streamline is the path a massless particle would follow in a frozen
// instant of the flow: integrate dx/dt = u(x). In a steady flow it is also the
// path a real particle takes, which is what makes it the natural way to *see*
// a solution. A shaded scalar tells you how fast the fluid is going; a
// streamline tells you where it is going, and it is the only picture in which
// a separation bubble is obvious rather than inferred.
//
// Locating the cell
// -----------------
// Integrating needs the velocity at arbitrary points, and the field is stored
// per cell. Rather than searching the mesh at every step, the integrator walks:
// it remembers which cell it was in, and when a step leaves that cell it
// crosses into the neighbour through whichever face it exited. That is O(1) per
// step instead of O(cells), which matters when a single streamline takes
// thousands of steps through a mesh of a hundred thousand.

#pragma once

#include <cstddef>
#include <vector>

#include "cfd/core/Error.hpp"
#include "cfd/core/Vec2.hpp"
#include "cfd/flow/FlowField.hpp"
#include "cfd/mesh/Mesh.hpp"

namespace cfd::post {

struct StreamlineOptions {
  /// Points to seed from. Each produces one curve, traced both ways.
  std::vector<Vec2> seeds;

  /// Largest number of integration steps in each direction.
  int maxSteps{4000};
  /// Step length as a fraction of the local cell size. Small enough that the
  /// walk rarely skips a cell, which is what keeps the location cheap.
  double stepFraction{0.35};
  /// Stop once the flow is slower than this fraction of the reference speed,
  /// so a curve does not crawl to a halt inside a stagnation region.
  double stallFraction{1e-4};
  /// Speed used to judge the above; normally the freestream.
  double referenceSpeed{1.0};
  /// Trace upstream from each seed as well as downstream.
  bool traceBackwards{true};
};

/// One traced curve, in order along the flow.
using Streamline = std::vector<Vec2>;

/// Trace streamlines through a solved field.
///
/// Curves that leave the domain, stall, or exceed the step budget simply end;
/// that is not an error. Seeds outside the mesh are skipped.
[[nodiscard]] Result<std::vector<Streamline>> traceStreamlines(
    const mesh::Mesh& mesh, const flow::FlowField& field,
    const StreamlineOptions& options);

/// Index of the cell containing `point`, or -1.
///
/// `hint` must be a cell *near* the point - normally the one the previous step
/// ended in. The walk follows a straight line towards the target, so it cannot
/// reach a point on the far side of the aerofoil: it would have to pass through
/// the section, and it stops at the wall and reports -1. That is the right
/// behaviour for tracing, where every step is a fraction of a cell, but it
/// means a distant hint is worse than none.
///
/// Pass -1 to search the mesh instead, which always succeeds but is linear in
/// the cell count. Use that to place a seed, then carry the result forward.
[[nodiscard]] int locateCell(const mesh::Mesh& mesh, Vec2 point, int hint = -1);

}  // namespace cfd::post
