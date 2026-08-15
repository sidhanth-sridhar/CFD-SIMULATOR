// FlowField.hpp - the solution state, plus how time and convergence are tracked.
//
// Storage layout
// --------------
// One value per cell, held in parallel arrays rather than an array of structs.
// A finite-volume sweep touches one quantity at a time across many cells, so
// separate contiguous arrays are what the cache wants, and they let a single
// field be handed to a routine without dragging the rest of the state along.
//
// Values live at the cell *centroid* and represent the average over the cell,
// not a sample at a point. That distinction is what makes a finite-volume
// method conservative, and it is why Mesh computes true polygon centroids.

#pragma once

#include <cstddef>
#include <deque>
#include <vector>

#include "cfd/core/Error.hpp"
#include "cfd/core/Vec2.hpp"
#include "cfd/flow/Freestream.hpp"

namespace cfd::flow {

/// Cell-centred state of the fluid.
///
/// Density and viscosity are stored per cell even though both are uniform
/// today. Viscosity will not stay uniform: from Phase 5 the turbulence model
/// adds an eddy viscosity that varies by orders of magnitude across the
/// boundary layer and the wake, and it is that combined *effective* viscosity
/// the momentum equation uses. Density is kept alongside it for symmetry and
/// because the assembly code reads the two together.
struct FlowField {
  std::vector<Vec2> velocity;    ///< m/s
  std::vector<double> pressure;  ///< Pa, gauge
  std::vector<double> density;   ///< kg/m^3
  std::vector<double> viscosity; ///< Pa.s, effective (molecular now, + eddy later)

  /// Allocate for `cells` cells, everything zeroed.
  void resize(std::size_t cells);

  [[nodiscard]] std::size_t size() const noexcept { return velocity.size(); }
  [[nodiscard]] bool empty() const noexcept { return velocity.empty(); }

  /// True when every array is the same length.
  [[nodiscard]] bool isConsistent() const noexcept;

  /// Fill every cell with the undisturbed stream.
  ///
  /// This is an *initial guess*, not a solution: it satisfies the far-field
  /// condition everywhere and the wall condition nowhere. Starting a solver
  /// from it is standard - the flow near the body then develops from the
  /// boundary conditions as the iteration proceeds.
  [[nodiscard]] static Result<FlowField> uniform(std::size_t cells,
                                                 const FreestreamConditions& freestream,
                                                 double chord);
};

/// Where the simulation has got to in time.
struct TimeState {
  double time{0.0};        ///< s
  double timeStep{0.0};    ///< s, the step most recently taken
  long long iteration{0};  ///< completed steps

  void advance(double dt) noexcept {
    timeStep = dt;
    time += dt;
    ++iteration;
  }
  void reset() noexcept { *this = TimeState{}; }
};

/// How far the discrete equations are from being satisfied.
///
/// A residual is what is left over when the current field is substituted back
/// into the equation being solved: for continuity it is the net volume flux
/// out of a cell, which must be zero for an incompressible flow. Residuals are
/// the only honest measure of convergence - a solution that stops changing but
/// whose residuals sit at 10^-2 has stalled, not converged.
struct ResidualSet {
  double continuity{0.0};
  double momentumX{0.0};
  double momentumY{0.0};

  /// Largest of the three; the usual single number to watch.
  [[nodiscard]] double worst() const noexcept;
};

/// Residuals against iteration number, for a convergence plot.
///
/// Bounded, for the same reason the log buffer is: a run that does not
/// converge must not consume memory without limit.
class ResidualHistory {
 public:
  struct Entry {
    long long iteration{0};
    ResidualSet residuals{};
  };

  explicit ResidualHistory(std::size_t capacity = 20000);

  void record(long long iteration, const ResidualSet& residuals);
  void clear() noexcept;

  [[nodiscard]] const std::deque<Entry>& entries() const noexcept { return entries_; }
  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
  [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
  [[nodiscard]] std::size_t droppedCount() const noexcept { return dropped_; }

  /// Most recent entry; a zeroed set if nothing has been recorded.
  [[nodiscard]] ResidualSet latest() const;

 private:
  std::deque<Entry> entries_;
  std::size_t capacity_;
  std::size_t dropped_{0};
};

}  // namespace cfd::flow
