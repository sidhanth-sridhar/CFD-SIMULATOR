// Tests for designation parsing and for the defining NACA equations.

#include "cfd/geom/Naca4.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <vector>

namespace {

using cfd::ErrorCode;
using cfd::geom::camberLine;
using cfd::geom::camberSlope;
using cfd::geom::cosineSpacing;
using cfd::geom::Naca4Digit;
using cfd::geom::parseNaca4Digit;
using cfd::geom::thicknessDistribution;
using cfd::geom::TrailingEdge;

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

TEST(ParseNaca4Digit, ReadsTheThreeShapeParameters) {
  const auto parsed = parseNaca4Digit("2412");
  ASSERT_TRUE(parsed) << (parsed.hasError() ? parsed.error().format() : "");

  const Naca4Digit& foil = parsed.value();
  EXPECT_EQ(2, foil.maxCamberPercent());
  EXPECT_EQ(4, foil.maxCamberPositionTenths());
  EXPECT_EQ(12, foil.thicknessPercent());

  EXPECT_DOUBLE_EQ(0.02, foil.maxCamber());
  EXPECT_DOUBLE_EQ(0.40, foil.maxCamberPosition());
  EXPECT_DOUBLE_EQ(0.12, foil.thickness());
  EXPECT_FALSE(foil.isSymmetric());
}

TEST(ParseNaca4Digit, AcceptsThePrefixAndAnyWhitespace) {
  const std::string kSpellings[] = {"2412", "NACA 2412", "naca 2412", "Naca2412",
                                    "  NACA   2412  ", "2 4 1 2"};
  for (const std::string& text : kSpellings) {
    const auto parsed = parseNaca4Digit(text);
    ASSERT_TRUE(parsed) << "failed to parse '" << text << "'";
    EXPECT_EQ("2412", parsed.value().digits()) << "for '" << text << "'";
  }
}

TEST(ParseNaca4Digit, RecognisesSymmetricSections) {
  const auto parsed = parseNaca4Digit("NACA 0012");
  ASSERT_TRUE(parsed);

  EXPECT_TRUE(parsed.value().isSymmetric());
  EXPECT_DOUBLE_EQ(0.0, parsed.value().maxCamber());
  EXPECT_DOUBLE_EQ(0.12, parsed.value().thickness());
}

TEST(ParseNaca4Digit, RejectsWrongDigitCounts) {
  for (const char* text : {"", "2", "241", "24123", "NACA", "NACA 241"}) {
    const auto parsed = parseNaca4Digit(text);
    EXPECT_TRUE(parsed.hasError()) << "'" << text << "' should not parse";
    if (parsed.hasError()) {
      EXPECT_EQ(ErrorCode::InvalidArgument, parsed.error().code());
    }
  }
}

TEST(ParseNaca4Digit, RejectsNonDigits) {
  for (const char* text : {"24a2", "twelve", "24.1", "-412"}) {
    EXPECT_TRUE(parseNaca4Digit(text).hasError()) << "'" << text << "' should not parse";
  }
}

// The camber equations divide by p^2, so camber with its maximum at the
// leading edge is not merely unusual - it is undefined.
TEST(ParseNaca4Digit, RejectsCamberedSectionWithCamberAtTheLeadingEdge) {
  const auto parsed = parseNaca4Digit("2012");
  ASSERT_TRUE(parsed.hasError());
  EXPECT_NE(std::string::npos, parsed.error().message().find("leading edge"));
}

TEST(ParseNaca4Digit, RejectsZeroCamberWithANonZeroPosition) {
  // "0412" claims a camber maximum at 40% chord while having no camber.
  EXPECT_TRUE(parseNaca4Digit("0412").hasError());
}

TEST(ParseNaca4Digit, RejectsZeroAndImplausibleThickness) {
  EXPECT_TRUE(parseNaca4Digit("2400").hasError());
  EXPECT_TRUE(parseNaca4Digit("0050").hasError());  // 50% thick
  EXPECT_TRUE(parseNaca4Digit("0099").hasError());
}

TEST(ParseNaca4Digit, FormatsItsOwnName) {
  const auto parsed = parseNaca4Digit("naca 0006");
  ASSERT_TRUE(parsed);
  EXPECT_EQ("0006", parsed.value().digits());
  EXPECT_EQ("NACA 0006", parsed.value().name());
}

// ---------------------------------------------------------------------------
// Thickness distribution
// ---------------------------------------------------------------------------

TEST(ThicknessDistribution, VanishesAtTheLeadingEdge) {
  EXPECT_DOUBLE_EQ(0.0, thicknessDistribution(0.0, 0.12));
  EXPECT_DOUBLE_EQ(0.0, thicknessDistribution(-0.5, 0.12));  // clamped, not NaN
}

// The tabulated NACA 0012 ordinate at 30% chord is 0.06002 of the chord. That
// is the standard reference value for this polynomial and a direct check that
// the coefficients are right.
TEST(ThicknessDistribution, MatchesTheTabulatedNaca0012Ordinate) {
  EXPECT_NEAR(0.06002, thicknessDistribution(0.30, 0.12), 1e-5);
}

// The polynomial's maximum is 1.0003*t rather than exactly t - a known
// property of the fit, not an error.
TEST(ThicknessDistribution, PeaksNearThirtyPercentChordAtHalfTheThickness) {
  constexpr double t = 0.12;

  double best = 0.0;
  double bestX = 0.0;
  for (int i = 0; i <= 100000; ++i) {
    const double x = static_cast<double>(i) / 100000.0;
    const double y = thicknessDistribution(x, t);
    if (y > best) {
      best = y;
      bestX = x;
    }
  }

  EXPECT_NEAR(0.30, bestX, 0.01);
  EXPECT_NEAR(0.5 * t, best, 5e-4);
  EXPECT_NEAR(1.0003 * t, 2.0 * best, 1e-4);
}

TEST(ThicknessDistribution, ScalesLinearlyWithThickness) {
  for (const double x : {0.05, 0.3, 0.6, 0.95}) {
    EXPECT_NEAR(2.0 * thicknessDistribution(x, 0.06), thicknessDistribution(x, 0.12), 1e-15);
  }
}

// The standard coefficients sum to 0.0021, leaving a small blunt base.
TEST(ThicknessDistribution, OpenTrailingEdgeLeavesTheStandardGap) {
  constexpr double t = 0.12;
  const double halfGap = thicknessDistribution(1.0, t, TrailingEdge::Open);

  EXPECT_NEAR(5.0 * t * 0.0021, halfGap, 1e-12);
  EXPECT_GT(halfGap, 0.0);
}

TEST(ThicknessDistribution, ClosedTrailingEdgeReachesExactlyZero) {
  for (const double t : {0.06, 0.12, 0.24}) {
    EXPECT_NEAR(0.0, thicknessDistribution(1.0, t, TrailingEdge::Closed), 1e-15);
  }
}

// The two forms differ only in the last coefficient, so they must agree near
// the nose and separate only towards the tail.
TEST(ThicknessDistribution, OpenAndClosedFormsAgreeNearTheLeadingEdge) {
  constexpr double t = 0.12;
  EXPECT_NEAR(thicknessDistribution(0.01, t, TrailingEdge::Open),
              thicknessDistribution(0.01, t, TrailingEdge::Closed), 1e-6);
  EXPECT_GT(thicknessDistribution(0.9, t, TrailingEdge::Open),
            thicknessDistribution(0.9, t, TrailingEdge::Closed));
}

// ---------------------------------------------------------------------------
// Camber line
// ---------------------------------------------------------------------------

TEST(CamberLine, IsIdenticallyZeroForASymmetricSection) {
  for (int i = 0; i <= 20; ++i) {
    const double x = static_cast<double>(i) / 20.0;
    EXPECT_DOUBLE_EQ(0.0, camberLine(x, 0.0, 0.0));
    EXPECT_DOUBLE_EQ(0.0, camberSlope(x, 0.0, 0.0));
  }
}

TEST(CamberLine, PassesThroughBothChordEndpoints) {
  EXPECT_NEAR(0.0, camberLine(0.0, 0.02, 0.4), 1e-15);
  EXPECT_NEAR(0.0, camberLine(1.0, 0.02, 0.4), 1e-15);
}

// y_c(p) = m exactly, from either branch. This is the definition of m and p,
// so it is an exact identity rather than an approximation.
TEST(CamberLine, ReachesExactlyTheStatedCamberAtTheStatedPosition) {
  struct Case {
    double m;
    double p;
  };
  for (const Case& c : {Case{0.02, 0.4}, Case{0.04, 0.4}, Case{0.06, 0.3}, Case{0.01, 0.9}}) {
    EXPECT_NEAR(c.m, camberLine(c.p, c.m, c.p), 1e-15)
        << "m=" << c.m << " p=" << c.p;
  }
}

TEST(CamberLine, IsContinuousAcrossTheJoin) {
  constexpr double m = 0.04;
  constexpr double p = 0.4;
  constexpr double eps = 1e-9;

  EXPECT_NEAR(camberLine(p - eps, m, p), camberLine(p + eps, m, p), 1e-12);
}

TEST(CamberLine, IsAMaximumAtTheStatedPosition) {
  constexpr double m = 0.04;
  constexpr double p = 0.4;

  for (int i = 0; i <= 1000; ++i) {
    const double x = static_cast<double>(i) / 1000.0;
    EXPECT_LE(camberLine(x, m, p), m + 1e-12) << "at x=" << x;
  }
}

TEST(CamberSlope, VanishesAtTheCamberMaximum) {
  EXPECT_NEAR(0.0, camberSlope(0.4, 0.04, 0.4), 1e-15);
  EXPECT_GT(camberSlope(0.1, 0.04, 0.4), 0.0);   // still rising
  EXPECT_LT(camberSlope(0.8, 0.04, 0.4), 0.0);   // falling away
}

// The slope must actually be the derivative of the camber line. Comparing
// against a central difference catches an algebra slip in either formula.
TEST(CamberSlope, MatchesTheNumericalDerivativeOfTheCamberLine) {
  constexpr double m = 0.04;
  constexpr double p = 0.4;
  constexpr double h = 1e-6;

  for (int i = 1; i < 100; ++i) {
    const double x = static_cast<double>(i) / 100.0;
    if (std::abs(x - p) < 1e-3) {
      continue;  // the slope has a kink at the join; skip its neighbourhood
    }
    const double numerical = (camberLine(x + h, m, p) - camberLine(x - h, m, p)) / (2.0 * h);
    EXPECT_NEAR(numerical, camberSlope(x, m, p), 1e-8) << "at x=" << x;
  }
}

// ---------------------------------------------------------------------------
// Cosine spacing
// ---------------------------------------------------------------------------

TEST(CosineSpacing, SpansTheChordExactly) {
  const std::vector<double> x = cosineSpacing(51);

  ASSERT_EQ(51u, x.size());
  EXPECT_DOUBLE_EQ(0.0, x.front());
  EXPECT_DOUBLE_EQ(1.0, x.back());
}

TEST(CosineSpacing, IsStrictlyIncreasing) {
  const std::vector<double> x = cosineSpacing(201);
  for (std::size_t i = 0; i + 1 < x.size(); ++i) {
    EXPECT_LT(x[i], x[i + 1]) << "at index " << i;
  }
}

// The whole point of cosine spacing: fine at the edges, coarse in the middle.
TEST(CosineSpacing, ClustersPointsAtBothEdges) {
  const std::vector<double> x = cosineSpacing(101);

  const double firstGap = x[1] - x[0];
  const double middleGap = x[51] - x[50];
  const double lastGap = x[100] - x[99];

  EXPECT_LT(firstGap, middleGap * 0.2) << "leading edge is not refined";
  EXPECT_LT(lastGap, middleGap * 0.2) << "trailing edge is not refined";
  EXPECT_NEAR(firstGap, lastGap, 1e-12) << "spacing should be symmetric";
}

TEST(CosineSpacing, IsSymmetricAboutMidChord) {
  const std::vector<double> x = cosineSpacing(101);
  for (std::size_t i = 0; i < x.size(); ++i) {
    EXPECT_NEAR(1.0 - x[x.size() - 1 - i], x[i], 1e-15);
  }
}

TEST(CosineSpacing, RejectsDegenerateCounts) {
  EXPECT_TRUE(cosineSpacing(1).empty());
  EXPECT_TRUE(cosineSpacing(0).empty());
  EXPECT_TRUE(cosineSpacing(-5).empty());
}

}  // namespace
