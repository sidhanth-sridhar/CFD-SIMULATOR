cmake_minimum_required(VERSION 3.24)

project(cfd_simulator
        VERSION 0.1.0
        DESCRIPTION "2D RANS CFD simulator for NACA airfoils"
        LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Sanity guards
# ---------------------------------------------------------------------------
# An in-source build scatters CMake's generated files through the source tree
# and makes "delete the build directory" impossible to do safely.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
  message(FATAL_ERROR
      "In-source builds are not supported.\n"
      "Use:  cmake -S . -B build")
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(CFD_BUILD_APP "Build the graphical application shell" ON)
option(CFD_BUILD_TESTS "Build the unit test suite" ${PROJECT_IS_TOP_LEVEL})
option(CFD_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

# The solver library must stay independent of the GUI. -DCFD_BUILD_APP=OFF
# builds and tests the numerics with no window system present at all, which is
# how this will run in CI and on a headless cluster node later on.

# ---------------------------------------------------------------------------
# Global build settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Emit compile_commands.json so clangd / clang-tidy / IDEs understand the build.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Collect executables in build/bin instead of scattering them per-subdirectory.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")

# Single-config generators (Ninja, Unix Makefiles) leave CMAKE_BUILD_TYPE empty
# by default, which means no optimisation *and* no debug info. Pick a sane
# default so a plain `cmake -S . -B build` produces something useful.
get_property(_cfd_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT _cfd_multi_config AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Build configuration" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY
               STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(CompilerWarnings)
include(Dependencies)

# ---------------------------------------------------------------------------
# Shared interface targets
# ---------------------------------------------------------------------------
# An INTERFACE library carries no compiled code; it exists purely to propagate
# flags to anything that links it. This keeps the strict warning set on our own
# code and off third-party sources.
add_library(cfd_warnings INTERFACE)
add_library(cfd::warnings ALIAS cfd_warnings)
cfd_set_project_warnings(cfd_warnings)

# ---------------------------------------------------------------------------
# Generated version header
# ---------------------------------------------------------------------------
configure_file("${PROJECT_SOURCE_DIR}/cmake/Version.hpp.in"
               "${PROJECT_BINARY_DIR}/generated/cfd/core/Version.hpp"
               @ONLY)

# ---------------------------------------------------------------------------
# Subdirectories
# ---------------------------------------------------------------------------
add_subdirectory(src/core)
add_subdirectory(src/geom)
add_subdirectory(src/mesh)
add_subdirectory(src/flow)
add_subdirectory(src/solver)
add_subdirectory(src/post)

if(CFD_BUILD_APP)
  add_subdirectory(src/app)
endif()

if(CFD_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Configuration summary
# ---------------------------------------------------------------------------
message(STATUS "")
message(STATUS "${PROJECT_NAME} ${PROJECT_VERSION}")
message(STATUS "  C++ standard ......... C++${CMAKE_CXX_STANDARD}")
message(STATUS "  Compiler ............. ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
if(_cfd_multi_config)
  message(STATUS "  Build type ........... (multi-config)")
else()
  message(STATUS "  Build type ........... ${CMAKE_BUILD_TYPE}")
endif()
message(STATUS "  Application shell .... ${CFD_BUILD_APP}")
message(STATUS "  Tests ................ ${CFD_BUILD_TESTS}")
message(STATUS "  Warnings as errors ... ${CFD_WARNINGS_AS_ERRORS}")
message(STATUS "")
