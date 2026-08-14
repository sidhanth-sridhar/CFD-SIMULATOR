// BuildInfo.hpp - facts about how this binary was built.
//
// Reproducing a CFD result requires knowing which build produced it. Compiler
// and optimisation level are not incidental details here: floating-point
// results can differ between an unoptimised and an optimised build, so a
// residual history is only meaningful alongside the configuration that made
// it. This is reported by --version and shown in the application's About
// panel, and will eventually be stamped into result files.

#pragma once

#include <string>
#include <string_view>

namespace cfd {

/// All values are baked in at compile time; nothing here reads the filesystem.
struct BuildInfo {
  /// Semantic version, e.g. "0.1.0". Single source of truth is project(...).
  [[nodiscard]] static std::string_view version() noexcept;

  [[nodiscard]] static std::string_view projectName() noexcept;
  [[nodiscard]] static std::string_view description() noexcept;

  /// CMake configuration: Debug, Release, RelWithDebInfo, MinSizeRel.
  /// Supplied through a generator expression, so it is correct for
  /// multi-config generators too.
  [[nodiscard]] static std::string_view buildType() noexcept;

  /// e.g. "AppleClang 17.0.0.17000013"
  [[nodiscard]] static std::string_view compiler() noexcept;

  /// e.g. "C++20", derived from the __cplusplus the code actually saw.
  [[nodiscard]] static std::string_view cxxStandard() noexcept;

  /// Multi-line human-readable block used by --version.
  [[nodiscard]] static std::string summary();
};

}  // namespace cfd
