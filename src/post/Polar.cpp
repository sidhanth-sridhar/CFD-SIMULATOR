#include "cfd/post/Polar.hpp"

#include <cmath>
#include <format>
#include <fstream>
#include <limits>

namespace cfd::post {

bool Polar::allConverged() const noexcept {
  for (const PolarPoint& point : points) {
    if (!point.converged) {
      return false;
    }
  }
  return !points.empty();
}

int Polar::bestLiftToDragIndex() const noexcept {
  int best = -1;
  double bestValue = -std::numeric_limits<double>::max();
  for (std::size_t i = 0; i < points.size(); ++i) {
    // Only points that converged, and only where drag is real: a lift-to-drag
    // ratio quoted from a run that never settled is not a design point.
    if (!points[i].converged || !(points[i].dragCoefficient > 0.0)) {
      continue;
    }
    if (points[i].liftToDrag > bestValue) {
      bestValue = points[i].liftToDrag;
      best = static_cast<int>(i);
    }
  }
  return best;
}

Result<std::vector<double>> sweepAngles(double start, double end, double step) {
  if (!std::isfinite(start) || !std::isfinite(end) || !std::isfinite(step)) {
    return Error{ErrorCode::InvalidArgument, "sweep bounds must be finite"};
  }
  if (!(step > 0.0)) {
    return Error{ErrorCode::InvalidArgument, "the sweep step must be positive"};
  }
  if (end < start) {
    return Error{ErrorCode::InvalidArgument,
                 "the sweep end must not be below the start"};
  }

  // Round rather than truncate, so a step that divides the range exactly is not
  // lost to floating point: (18 - 0) / 2 can land a hair under 9.
  const double span = (end - start) / step;
  const double rounded = std::round(span);
  const double count = (std::abs(span - rounded) < 1e-9) ? rounded : std::floor(span);

  if (count < 0.0) {
    return Error{ErrorCode::InvalidArgument, "the sweep covers no angles"};
  }
  const double points = count + 1.0;
  if (points > static_cast<double>(kMaxSweepPoints)) {
    return Error{ErrorCode::InvalidArgument,
                 std::format("a sweep of {:.0f} points is more than the {} allowed; "
                             "every point is a full solve",
                             points, kMaxSweepPoints)};
  }

  std::vector<double> angles;
  angles.reserve(static_cast<std::size_t>(points));
  const auto total = static_cast<std::size_t>(points);
  for (std::size_t i = 0; i < total; ++i) {
    // Computed, not accumulated, so a long sweep does not drift.
    angles.push_back(start + static_cast<double>(i) * step);
  }
  return angles;
}

std::string toCsv(const Polar& polar) {
  std::string out;
  out.reserve(256 + polar.points.size() * 128);

  out += "# cfd_simulator aerodynamic polar\n";
  out += std::format("# section,{}\n", polar.section.empty() ? "unknown" : polar.section);
  out += std::format("# mesh,{}\n",
                     polar.meshResolution.empty() ? "unknown" : polar.meshResolution);
  out += std::format("# reynolds,{:.6g}\n", polar.reynoldsNumber);
  out += std::format("# freestream_speed_mps,{:.6g}\n", polar.machEquivalentSpeed);
  out += std::format("# chord_m,{:.6g}\n", polar.chord);
  out += std::format("# moment_reference_xc,{:.6g}\n", polar.momentReferenceFraction);
  out += std::format("# continued_between_points,{}\n",
                     polar.continuedBetweenPoints ? "yes" : "no");
  out += "# every row is a separate Navier-Stokes solve; nothing is interpolated\n";

  out +=
      "alpha_deg,cl,cd,cd_pressure,cd_friction,cm,l_over_d,"
      "separation_upper_xc,separation_lower_xc,converged,iterations,continuity_residual\n";

  for (const PolarPoint& p : polar.points) {
    // Separation is written as an empty field where the surface stayed
    // attached. A -1 in a column of chord fractions invites being plotted.
    const std::string upper =
        (p.upperSeparation >= 0.0) ? std::format("{:.6f}", p.upperSeparation) : std::string{};
    const std::string lower =
        (p.lowerSeparation >= 0.0) ? std::format("{:.6f}", p.lowerSeparation) : std::string{};

    out += std::format("{:.6f},{:.8f},{:.8f},{:.8f},{:.8f},{:.8f},{:.6f},{},{},{},{},{:.6e}\n",
                       p.angleOfAttackDeg, p.liftCoefficient, p.dragCoefficient,
                       p.pressureDragCoefficient, p.frictionDragCoefficient,
                       p.momentCoefficient, p.liftToDrag, upper, lower,
                       p.converged ? "yes" : "no", p.iterations, p.continuityResidual);
  }
  return out;
}

Status writeCsv(const Polar& polar, const std::string& path) {
  if (path.empty()) {
    return Error{ErrorCode::InvalidArgument, "no output path for the polar"};
  }
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    return Error{ErrorCode::IoFailure, "cannot open '" + path + "' for writing"};
  }

  const std::string text = toCsv(polar);
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
  if (!out) {
    return Error{ErrorCode::IoFailure, "failed while writing '" + path + "'"};
  }
  return Status::ok();
}

}  // namespace cfd::post
