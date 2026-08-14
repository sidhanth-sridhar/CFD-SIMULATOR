// Verifies that the generated version header actually tracks CMake.
//
// CFD_EXPECTED_VERSION is injected by tests/CMakeLists.txt straight from
// ${PROJECT_VERSION}. Version.hpp reaches the same value by a different route
// (configure_file substituting into a template). If the generation step ever
// silently stops running - a stale build directory, a mistyped @VAR@ - these
// two disagree and the test fails.

#include "cfd/core/BuildInfo.hpp"

#include <gtest/gtest.h>

#include <string>

namespace {

TEST(BuildInfo, VersionMatchesTheBuildSystem) {
  EXPECT_EQ(std::string{CFD_EXPECTED_VERSION}, std::string{cfd::BuildInfo::version()});
}

TEST(BuildInfo, ReportsConcreteBuildFacts) {
  EXPECT_FALSE(cfd::BuildInfo::projectName().empty());

  // "Unknown" is the fallback used when the compile definitions are missing,
  // so seeing it here means the build wiring is broken.
  EXPECT_NE("Unknown", cfd::BuildInfo::buildType());
  EXPECT_NE("Unknown", cfd::BuildInfo::compiler());

  EXPECT_EQ("C++20", cfd::BuildInfo::cxxStandard());
}

TEST(BuildInfo, SummaryMentionsNameAndVersion) {
  const std::string summary = cfd::BuildInfo::summary();

  EXPECT_NE(std::string::npos, summary.find(cfd::BuildInfo::version()));
  EXPECT_NE(std::string::npos, summary.find(cfd::BuildInfo::projectName()));
}

}  // namespace
