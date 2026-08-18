// Polar.hpp - a section's coefficients as a function of angle of attack.
//
// What a polar is
// ---------------
// One solve gives one number for each coefficient, at one incidence. That is
// almost never the question. What a section is chosen by is how those numbers
// *behave* as the angle of attack changes: how steeply lift builds, where the
// lift curve stops being straight, how fast drag grows once it does, and where
// the best lift-to-drag ratio sits. The curves that answer this are the polar.
//
// Every point of it here is a separate converged solve. Nothing is fitted,
// interpolated between angles, or taken from a lift-curve-slope formula: if a
// row appears in the table, a Navier-Stokes solution was run to produce it, and
// if that solve did not converge the row says so rather than quietly reporting
// whatever the iteration was holding.
//
// This header owns the parts of that with no user interface in them - the list
// of angles to visit, the table of results, and how to write it out - so they
// can be tested without a window.

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "cfd/core/Error.hpp"

namespace cfd::post {

/// One converged (or attempted) operating point.
struct PolarPoint {
  double angleOfAttackDeg{0.0};

  double liftCoefficient{0.0};
  double dragCoefficient{0.0};
  double pressureDragCoefficient{0.0};
  double frictionDragCoefficient{0.0};
  double momentCoefficient{0.0};
  double liftToDrag{0.0};

  /// Chordwise separation stations, or -1 where the surface stayed attached.
  double upperSeparation{-1.0};
  double lowerSeparation{-1.0};

  /// Whether the solve behind this point actually converged, and what it cost.
  /// A polar that silently mixes converged and unconverged points is worse than
  /// no polar, so this travels with every row and into the CSV.
  bool converged{false};
  /// The solve blew up rather than merely running out of iterations. Recorded
  /// separately because the two are not the same failure: a run that hit its
  /// limit is somewhere near an answer, and one that diverged is not near
  /// anything. Both are "not converged", and reporting only that would lose the
  /// distinction that decides whether the row is worth looking at at all.
  bool diverged{false};
  /// Whether the point had to be retried from the undisturbed stream after
  /// continuing from the previous angle failed.
  bool retriedCold{false};
  long long iterations{0};
  double continuityResidual{0.0};
};

/// How a point's solve ended, as written to the CSV.
[[nodiscard]] std::string_view pointStatus(const PolarPoint& point) noexcept;

/// A whole sweep, plus enough of the conditions to make it meaningful later.
///
/// Coefficients without the Reynolds number and the section they belong to are
/// not a result, they are four columns of numbers.
struct Polar {
  std::string section;
  std::string meshResolution;
  double reynoldsNumber{0.0};
  double machEquivalentSpeed{0.0};  ///< freestream speed, m/s
  double chord{1.0};
  double momentReferenceFraction{0.25};
  /// Whether each point continued from the previous one's field.
  bool continuedBetweenPoints{true};

  std::vector<PolarPoint> points;

  [[nodiscard]] bool empty() const noexcept { return points.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return points.size(); }
  /// True when every point in the sweep converged.
  [[nodiscard]] bool allConverged() const noexcept;
  /// Index of the point with the largest lift-to-drag ratio, or -1 if none has
  /// a usable one. The single most-asked question of a polar.
  [[nodiscard]] int bestLiftToDragIndex() const noexcept;
};

/// The angles a sweep from `start` to `end` in steps of `step` should visit.
///
/// Inclusive of both ends where the step divides the range, and inclusive of
/// `end` when it lands within a rounding error of the last step. Angles are
/// computed as start + i*step rather than accumulated, so a long sweep does not
/// drift: 0 to 18 in steps of 0.1 must give exactly 18, not 17.999999999999.
///
/// `step` is a magnitude; the direction comes from which end is which, so
/// 18 to 0 in steps of 2 walks downwards. That is not a convenience: continuing
/// each point from the previous one is exactly how hysteresis would appear if
/// the flow ever had more than one steady state at an incidence, and the way to
/// look for it is to sweep down and compare with sweeping up.
///
/// Fails on a non-positive step, and on a range that would produce an
/// unreasonable number of solves - each point here is a full Navier-Stokes
/// solve, so a typo in the step is minutes or hours of work.
[[nodiscard]] Result<std::vector<double>> sweepAngles(double start, double end, double step);

/// Largest number of points a single sweep may ask for.
inline constexpr std::size_t kMaxSweepPoints = 400;

// ---------------------------------------------------------------------------
// Sweep sequencing
// ---------------------------------------------------------------------------
//
// A sweep cannot be a loop: every point is a full solve, so looping would
// freeze the interface for the minutes it takes. It is therefore advanced one
// frame at a time, and the question each frame is the same - given where the
// sweep is and what the solver looks like, what happens next?
//
// That question is pure, and it is where the interesting mistake lives. The
// obvious implementation asks "has the solver stopped?", which is
// indistinguishable from "has the solver not started yet" - so the frame right
// after a new angle is requested would record the *previous* angle's forces and
// move straight on. Separating it out is what lets that be tested without a
// window.

/// Where a sweep is between points.
enum class SweepPhase {
  /// Not sweeping.
  Idle,
  /// An angle has been requested; waiting for the solver to pick it up.
  Starting,
  /// The solver is working on the current angle.
  Solving,
};

/// What the solver looks like from the sweep's point of view this frame.
struct SweepObservation {
  bool solverRunning{false};
  /// The solver reported it could not run at all.
  bool solverFailed{false};
  /// Frames spent so far waiting for the solver to start.
  int framesWaiting{0};
};

/// What the sweep should do about it.
enum class SweepAction {
  /// Nothing this frame.
  Wait,
  /// The solver has taken the work up; start watching for it to finish.
  BeginSolving,
  /// This angle is finished; record the point and request the next.
  RecordPoint,
  /// The solver never started. Give up rather than wait forever.
  AbortNotStarted,
  /// The solver reported an error.
  AbortFailed,
};

/// Decide what a sweep does next.
///
/// `startupFrameBudget` bounds how long `Starting` will wait. Some bound is
/// needed because silently waiting forever is the worst possible failure for an
/// operation that legitimately takes minutes: it looks exactly like working.
[[nodiscard]] SweepAction nextSweepAction(SweepPhase phase,
                                          const SweepObservation& observation,
                                          int startupFrameBudget) noexcept;

/// Render the polar as CSV text.
///
/// The conditions go in as `#` comment lines above the header, which every
/// common reader - pandas, R, gnuplot, most spreadsheets - can be told to skip,
/// and which keep the result self-describing when the file is opened a month
/// later.
[[nodiscard]] std::string toCsv(const Polar& polar);

/// Write the polar to `path` as CSV.
[[nodiscard]] Status writeCsv(const Polar& polar, const std::string& path);

}  // namespace cfd::post
