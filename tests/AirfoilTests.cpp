// Numerical validation of the generated airfoil geometry.
//
// The checks fall into three groups:
//
//   * Exact identities that follow from how the section is constructed - the
//     surface midpoint lying on the camber line, thickness being perpendicular
//     to it, the two surfaces of a symmetric section mirroring exactly.
//   * Agreement with the designation - measured thickness, camber and their
//     chordwise positions, recovered from the generated points rather than
//     read back from the input.
//   * Agreement with calculus - the enclosed area computed from the discrete
//     contour by the shoelace formula, compared against the analytic integral
//     of the thickness polynomial, including a refinement study.

#include "cfd/geom/Airfoil.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "cfd/geom/Naca4.hpp"

namespace {

using cfd::Vec2;
using cfd::geom::Airfoil;
using cfd::geom::AirfoilOptions;
using cfd::geom::camberSlope;
using cfd::geom::generate;
using cfd::geom::makeNaca4Digit;
using cfd::geom::Naca4Digit;
using cfd::geom::parseNaca4Digit;
using cfd::geom::thicknessDistribution;
using cfd::geom::TrailingEdge;

Airfoil build(const std::string& designation, AirfoilOptions options = {}) {
  auto result = makeNaca4Digit(designation, options);
  EXPECT_TRUE(result) << "failed to build " << designation << ": "
                      << (result.hasError() ? result.error().format() : "");
  return std::move(result).value();
}

/// Exact area enclosed by a symmetric section of unit chord.
///
/// The section is +/- y_t about the chord, so
///
///   A = 2 * integral_0^1 y_t(x) dx
///     = 2 * 5t * ( a0*(2/3) + a1/2 + a2/3 + a3/4 + a4/5 )
///
/// integrating each polynomial term of y_t/(5t) analytically.
double analyticSymmetricArea(double thickness, TrailingEdge trailingEdge) {
  constexpr double a0 = 0.2969;
  constexpr double a1 = -0.1260;
  constexpr double a2 = -0.3516;
  constexpr double a3 = 0.2843;
  const double a4 = (trailingEdge == TrailingEdge::Closed) ? -0.1036 : -0.1015;

  const double integral = a0 * (2.0 / 3.0) + a1 / 2.0 + a2 / 3.0 + a3 / 4.0 + a4 / 5.0;
  return 2.0 * 5.0 * thickness * integral;
}

// The three requested sections plus a thin symmetric one and a strongly
// cambered one, so the tests below cover the full range of behaviour.
const std::vector<std::string> kSections{"0012", "2412", "4412", "0006", "6409"};

// ---------------------------------------------------------------------------
// Chord, leading edge, trailing edge
// ---------------------------------------------------------------------------

TEST(Airfoil, LeadingEdgeSitsAtTheOrigin) {
  for (const std::string& name : kSections) {
    const Airfoil foil = build(name);

    EXPECT_DOUBLE_EQ(0.0, foil.leadingEdge().x) << name;
    EXPECT_DOUBLE_EQ(0.0, foil.leadingEdge().y) << name;
    // Both surfaces must start from that same point, bit for bit.
    EXPECT_EQ(foil.upper().front(), foil.lower().front()) << name;
  }
}

TEST(Airfoil, TrailingEdgeSitsAtTheChordLength) {
  for (const std::string& name : kSections) {
    const Airfoil foil = build(name);

    // The two surface points are offset perpendicular to the camber line, so
    // on a cambered section with a blunt trailing edge they straddle x = c
    // rather than both landing on it. Their midpoint is the trailing edge, and
    // that is exact because the offsets are equal and opposite.
    EXPECT_NEAR(1.0, foil.trailingEdge().x, 1e-12) << name;
    EXPECT_NEAR(0.0, foil.trailingEdge().y - foil.camberLine().back().y, 1e-12) << name;
    EXPECT_NEAR(1.0, foil.chord(), 1e-15) << name;
  }
}

// With the trailing edge closed there is no thickness left to offset, so both
// surfaces genuinely terminate on the chord station.
TEST(Airfoil, ClosedTrailingEdgePointsLandExactlyOnTheChordStation) {
  for (const std::string& name : kSections) {
    const Airfoil foil = build(name, AirfoilOptions{.trailingEdge = TrailingEdge::Closed});

    EXPECT_NEAR(1.0, foil.upper().back().x, 1e-12) << name;
    EXPECT_NEAR(1.0, foil.lower().back().x, 1e-12) << name;
  }
}

// Quantifies the straddle above, so the behaviour is pinned rather than merely
// tolerated: the shift is the half-gap times the sine of the camber angle.
TEST(Airfoil, OpenTrailingEdgePointsStraddleTheChordStationSymmetrically) {
  const Airfoil foil = build("2412", AirfoilOptions{.trailingEdge = TrailingEdge::Open});

  const double halfGap = thicknessDistribution(1.0, 0.12, TrailingEdge::Open);
  const double slope = camberSlope(1.0, 0.02, 0.4);
  const double expectedShift = halfGap * std::sin(std::atan(slope));

  EXPECT_NEAR(1.0 - expectedShift, foil.upper().back().x, 1e-12);
  EXPECT_NEAR(1.0 + expectedShift, foil.lower().back().x, 1e-12);

  // A symmetric section has no camber angle, so there is no shift at all.
  const Airfoil symmetric = build("0012", AirfoilOptions{.trailingEdge = TrailingEdge::Open});
  EXPECT_NEAR(1.0, symmetric.upper().back().x, 1e-12);
  EXPECT_NEAR(1.0, symmetric.lower().back().x, 1e-12);
}

TEST(Airfoil, StationsRunFromLeadingToTrailingEdge) {
  const Airfoil foil = build("2412");

  ASSERT_FALSE(foil.stations().empty());
  EXPECT_DOUBLE_EQ(0.0, foil.stations().front());
  EXPECT_DOUBLE_EQ(1.0, foil.stations().back());
  EXPECT_EQ(foil.stations().size(), foil.upper().size());
  EXPECT_EQ(foil.stations().size(), foil.lower().size());
  EXPECT_EQ(foil.stations().size(), foil.camberLine().size());
}

// ---------------------------------------------------------------------------
// Thickness and camber recovered from the generated points
// ---------------------------------------------------------------------------

TEST(Airfoil, MeasuredThicknessMatchesTheDesignation) {
  struct Case {
    std::string name;
    double thickness;
  };
  for (const Case& c : {Case{"0012", 0.12}, Case{"2412", 0.12}, Case{"4412", 0.12},
                        Case{"0006", 0.06}, Case{"6409", 0.09}}) {
    const Airfoil foil = build(c.name, AirfoilOptions{.pointsPerSurface = 401});

    // The polynomial peaks at 1.0003*t, and the discrete stations may not land
    // exactly on the maximum, so allow a little slack.
    EXPECT_NEAR(c.thickness, foil.maxThickness(), 1e-3) << c.name;
    EXPECT_NEAR(0.30, foil.maxThicknessPosition(), 0.02) << c.name;
  }
}

TEST(Airfoil, MeasuredCamberMatchesTheDesignation) {
  struct Case {
    std::string name;
    double camber;
    double position;
  };
  for (const Case& c : {Case{"2412", 0.02, 0.4}, Case{"4412", 0.04, 0.4},
                        Case{"6409", 0.06, 0.4}}) {
    const Airfoil foil = build(c.name, AirfoilOptions{.pointsPerSurface = 401});

    EXPECT_NEAR(c.camber, foil.maxCamber(), 1e-4) << c.name;
    EXPECT_NEAR(c.position, foil.maxCamberPosition(), 0.01) << c.name;
  }
}

TEST(Airfoil, SymmetricSectionsHaveNoCamber) {
  for (const std::string& name : {"0012", "0006"}) {
    const Airfoil foil = build(name);

    EXPECT_NEAR(0.0, foil.maxCamber(), 1e-15) << name;
    for (const Vec2& point : foil.camberLine()) {
      EXPECT_DOUBLE_EQ(0.0, point.y) << name;
    }
  }
}

// ---------------------------------------------------------------------------
// Symmetry
// ---------------------------------------------------------------------------

TEST(Airfoil, SymmetricSectionsAreExactMirrorImages) {
  const Airfoil foil = build("0012");

  for (std::size_t i = 0; i < foil.upper().size(); ++i) {
    const Vec2& upper = foil.upper()[i];
    const Vec2& lower = foil.lower()[i];

    // With no camber the offsets are purely vertical, so the two surfaces
    // share a station and differ only in the sign of y.
    EXPECT_DOUBLE_EQ(upper.x, lower.x) << "at index " << i;
    EXPECT_DOUBLE_EQ(upper.y, -lower.y) << "at index " << i;
    EXPECT_DOUBLE_EQ(foil.stations()[i], upper.x) << "at index " << i;
  }
}

TEST(Airfoil, CamberedSectionsAreNotSymmetric) {
  const Airfoil foil = build("4412");

  double largestAsymmetry = 0.0;
  for (std::size_t i = 0; i < foil.upper().size(); ++i) {
    largestAsymmetry =
        std::max(largestAsymmetry, std::abs(foil.upper()[i].y + foil.lower()[i].y));
  }
  // Twice the camber, since y_u + y_l = 2*y_c.
  EXPECT_NEAR(2.0 * 0.04, largestAsymmetry, 1e-3);
}

// ---------------------------------------------------------------------------
// Construction identities
// ---------------------------------------------------------------------------

// (upper + lower)/2 must be the camber line, exactly, because the two surfaces
// are equal and opposite offsets from it.
TEST(Airfoil, SurfaceMidpointsLieOnTheCamberLine) {
  for (const std::string& name : kSections) {
    const Airfoil foil = build(name);

    for (std::size_t i = 0; i < foil.upper().size(); ++i) {
      const Vec2 midpoint = (foil.upper()[i] + foil.lower()[i]) * 0.5;
      EXPECT_NEAR(foil.camberLine()[i].x, midpoint.x, 1e-15) << name << " at " << i;
      EXPECT_NEAR(foil.camberLine()[i].y, midpoint.y, 1e-15) << name << " at " << i;
    }
  }
}

// This is the check that catches the common shortcut of offsetting the
// thickness vertically instead of normal to the camber line. For a symmetric
// section the two are identical, so it only bites on cambered ones.
TEST(Airfoil, ThicknessIsAppliedPerpendicularToTheCamberLine) {
  for (const std::string& name : {"2412", "4412", "6409"}) {
    const Airfoil foil = build(name);
    const auto parsed = parseNaca4Digit(name);
    ASSERT_TRUE(parsed);
    const double m = parsed.value().maxCamber();
    const double p = parsed.value().maxCamberPosition();

    for (std::size_t i = 1; i + 1 < foil.upper().size(); ++i) {
      const Vec2 across = foil.upper()[i] - foil.lower()[i];
      const double slope = camberSlope(foil.stations()[i], m, p);
      const Vec2 tangent{1.0, slope};

      const double alignment =
          dot(across, tangent) / (length(across) * length(tangent));
      EXPECT_NEAR(0.0, alignment, 1e-12) << name << " at index " << i;
    }
  }
}

TEST(Airfoil, LocalThicknessMatchesTheAnalyticDistribution) {
  for (const std::string& name : kSections) {
    const Airfoil foil = build(name);
    const double t = foil.designation().thickness();

    for (std::size_t i = 0; i < foil.upper().size(); ++i) {
      const double measured = distance(foil.upper()[i], foil.lower()[i]);
      const double expected =
          2.0 * thicknessDistribution(foil.stations()[i], t, foil.trailingEdgeStyle());
      EXPECT_NEAR(expected, measured, 1e-14) << name << " at index " << i;
    }
  }
}

// A known and correct consequence of the perpendicular offset: near the nose
// the thickness term grows like sqrt(x) and outruns x itself, so on a cambered
// section the upper surface reaches slightly ahead of the leading edge point.
// Documented here so it is not later mistaken for a bug.
TEST(Airfoil, CamberedUpperSurfaceReachesSlightlyAheadOfTheLeadingEdge) {
  const Airfoil foil = build("4412", AirfoilOptions{.pointsPerSurface = 401});

  double smallestX = 0.0;
  for (const Vec2& point : foil.upper()) {
    smallestX = std::min(smallestX, point.x);
  }

  EXPECT_LT(smallestX, 0.0);
  EXPECT_GT(smallestX, -0.01) << "the overhang should be a fraction of a percent of chord";
}

// ---------------------------------------------------------------------------
// Contour ordering
// ---------------------------------------------------------------------------

TEST(Airfoil, ContourIsAClosedLoop) {
  for (const std::string& name : kSections) {
    const Airfoil foil = build(name);

    ASSERT_GE(foil.contour().size(), 4u) << name;
    EXPECT_EQ(foil.contour().front(), foil.contour().back()) << name;
  }
}

TEST(Airfoil, ContourStartsAtTheTrailingEdgeAndTurnsAtTheLeadingEdge) {
  const Airfoil foil = build("2412");
  const std::size_t n = foil.upper().size();

  // Conventional ordering: trailing edge, forward over the upper surface to
  // the leading edge, then aft along the lower surface.
  EXPECT_EQ(foil.upper().back(), foil.contour().front());
  EXPECT_EQ(foil.leadingEdge(), foil.contour()[n - 1]);
  EXPECT_EQ(foil.upper()[n / 2], foil.contour()[n - 1 - n / 2]);
}

TEST(Airfoil, ContourIsCounterClockwise) {
  for (const std::string& name : kSections) {
    const Airfoil foil = build(name);
    // The shoelace area is positive exactly when the traversal is
    // counter-clockwise.
    EXPECT_GT(foil.area(), 0.0) << name;
  }
}

TEST(Airfoil, ContourHasNoRepeatedInteriorPoints) {
  for (const TrailingEdge te : {TrailingEdge::Open, TrailingEdge::Closed}) {
    const Airfoil foil = build("2412", AirfoilOptions{.trailingEdge = te});
    const std::vector<Vec2>& contour = foil.contour();

    // Only the deliberate closing repeat at the very end is allowed.
    for (std::size_t i = 0; i + 2 < contour.size(); ++i) {
      EXPECT_GT(distance(contour[i], contour[i + 1]), 0.0)
          << "duplicate at index " << i;
    }
  }
}

TEST(Airfoil, ContourPointCountFollowsTheTrailingEdgeStyle) {
  constexpr int kPoints = 101;
  const auto n = static_cast<std::size_t>(kPoints);

  const Airfoil open = build("2412", AirfoilOptions{.pointsPerSurface = kPoints,
                                                    .trailingEdge = TrailingEdge::Open});
  // Both surfaces in full, minus the shared leading edge, plus the repeated
  // first point that closes the loop.
  EXPECT_EQ(2 * n, open.contour().size());

  const Airfoil closed = build("2412", AirfoilOptions{.pointsPerSurface = kPoints,
                                                      .trailingEdge = TrailingEdge::Closed});
  // One fewer, because the trailing edge is also shared.
  EXPECT_EQ(2 * n - 1, closed.contour().size());
}

// ---------------------------------------------------------------------------
// Trailing edge
// ---------------------------------------------------------------------------

TEST(Airfoil, OpenTrailingEdgeLeavesTheStandardBluntGap) {
  const Airfoil foil = build("0012", AirfoilOptions{.trailingEdge = TrailingEdge::Open});

  // 2 * 5t * 0.0021 = 0.021*t
  EXPECT_NEAR(0.021 * 0.12, foil.trailingEdgeGap(), 1e-12);
}

TEST(Airfoil, ClosedTrailingEdgeMeetsAtASinglePoint) {
  for (const std::string& name : kSections) {
    const Airfoil foil = build(name, AirfoilOptions{.trailingEdge = TrailingEdge::Closed});

    EXPECT_NEAR(0.0, foil.trailingEdgeGap(), 1e-15) << name;
    EXPECT_EQ(foil.upper().back(), foil.lower().back()) << name;
  }
}

// ---------------------------------------------------------------------------
// Agreement with calculus
// ---------------------------------------------------------------------------

TEST(Airfoil, EnclosedAreaMatchesTheAnalyticIntegral) {
  for (const TrailingEdge te : {TrailingEdge::Open, TrailingEdge::Closed}) {
    for (const auto& [name, thickness] :
         std::vector<std::pair<std::string, double>>{{"0012", 0.12}, {"0006", 0.06}}) {
      const Airfoil foil =
          build(name, AirfoilOptions{.pointsPerSurface = 2001, .trailingEdge = te});

      EXPECT_NEAR(analyticSymmetricArea(thickness, te), foil.area(), 1e-6)
          << name << (te == TrailingEdge::Open ? " open" : " closed");
    }
  }
}

// A polygon inscribed in a smooth curve underestimates the enclosed area, with
// the error falling as the square of the spacing. Confirming that the error
// actually shrinks at the expected rate validates the discretisation itself,
// not just one particular point count.
TEST(Airfoil, AreaConvergesQuadraticallyUnderRefinement) {
  const double exact = analyticSymmetricArea(0.12, TrailingEdge::Closed);

  const auto errorFor = [&exact](int points) {
    const Airfoil foil = build("0012", AirfoilOptions{.pointsPerSurface = points,
                                                      .trailingEdge = TrailingEdge::Closed});
    return std::abs(foil.area() - exact);
  };

  const double coarse = errorFor(201);
  const double fine = errorFor(401);
  const double finer = errorFor(801);

  EXPECT_GT(coarse, 0.0);
  EXPECT_LT(fine, coarse);
  EXPECT_LT(finer, fine);
  // Halving the spacing should cut the error by roughly four; require at least
  // a factor of three to leave room for the leading edge singularity.
  EXPECT_LT(fine, coarse / 3.0);
  EXPECT_LT(finer, fine / 3.0);
}

TEST(Airfoil, PerimeterIsAtLeastTwiceTheChord) {
  const Airfoil foil = build("0012");
  // The outline runs out along one surface and back along the other, so it
  // cannot be shorter than going out and back along the chord itself.
  EXPECT_GT(foil.perimeter(), 2.0);
  EXPECT_LT(foil.perimeter(), 2.5);
}

// ---------------------------------------------------------------------------
// Scaling and options
// ---------------------------------------------------------------------------

TEST(Airfoil, ScalesLinearlyWithChord) {
  const Airfoil unit = build("2412", AirfoilOptions{.chord = 1.0});
  const Airfoil scaled = build("2412", AirfoilOptions{.chord = 2.5});

  EXPECT_NEAR(2.5, scaled.chord(), 1e-15);
  EXPECT_NEAR(2.5 * unit.maxThickness(), scaled.maxThickness(), 1e-12);
  EXPECT_NEAR(2.5 * unit.maxCamber(), scaled.maxCamber(), 1e-12);
  EXPECT_NEAR(2.5 * unit.perimeter(), scaled.perimeter(), 1e-12);
  // Area is a squared quantity.
  EXPECT_NEAR(2.5 * 2.5 * unit.area(), scaled.area(), 1e-12);

  // Positions are fractions of chord, so they do not change.
  EXPECT_NEAR(unit.maxThicknessPosition(), scaled.maxThicknessPosition(), 1e-15);
  EXPECT_NEAR(unit.maxCamberPosition(), scaled.maxCamberPosition(), 1e-15);
}

TEST(Airfoil, BoundsEncloseEveryContourPoint) {
  const Airfoil foil = build("4412");
  const auto [minimum, maximum] = foil.bounds();

  for (const Vec2& point : foil.contour()) {
    EXPECT_GE(point.x, minimum.x);
    EXPECT_LE(point.x, maximum.x);
    EXPECT_GE(point.y, minimum.y);
    EXPECT_LE(point.y, maximum.y);
  }
  // The section spans the chord, give or take the sub-millimetre overhang at
  // each end that the perpendicular offset produces on a cambered surface.
  EXPECT_NEAR(1.0, maximum.x, 1e-3);
  EXPECT_NEAR(0.0, minimum.x, 1e-2);
}

TEST(Airfoil, EveryCoordinateIsFinite) {
  for (const std::string& name : kSections) {
    const Airfoil foil = build(name);
    for (const Vec2& point : foil.contour()) {
      ASSERT_TRUE(std::isfinite(point.x)) << name;
      ASSERT_TRUE(std::isfinite(point.y)) << name;
    }
  }
}

TEST(Airfoil, RejectsUnusableDiscretisation) {
  const Naca4Digit foil{2, 4, 12};

  EXPECT_TRUE(generate(foil, AirfoilOptions{.pointsPerSurface = 2}).hasError());
  EXPECT_TRUE(generate(foil, AirfoilOptions{.pointsPerSurface = -1}).hasError());
  EXPECT_TRUE(generate(foil, AirfoilOptions{.pointsPerSurface = 10'000'000}).hasError());
  EXPECT_TRUE(generate(foil, AirfoilOptions{.chord = 0.0}).hasError());
  EXPECT_TRUE(generate(foil, AirfoilOptions{.chord = -1.0}).hasError());
  EXPECT_TRUE(
      generate(foil, AirfoilOptions{.chord = std::numeric_limits<double>::quiet_NaN()})
          .hasError());
}

TEST(Airfoil, MakeFromDesignationPropagatesParseErrors) {
  const auto result = makeNaca4Digit("NACA 24");
  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(cfd::ErrorCode::InvalidArgument, result.error().code());
}

TEST(Airfoil, RemembersItsDesignation) {
  const Airfoil foil = build("NACA 4412");

  EXPECT_EQ("4412", foil.designation().digits());
  EXPECT_EQ("NACA 4412", foil.designation().name());
  EXPECT_FALSE(foil.designation().isSymmetric());
}

}  // namespace
