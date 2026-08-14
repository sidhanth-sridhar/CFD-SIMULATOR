# cfd_core - foundation layer.
#
# Deliberately knows nothing about windows, OpenGL or CFD. Everything above it
# (the application shell now; geometry, mesh and solver later) may depend on
# this, and it depends on nothing but the standard library. That is what lets
# the numerics be tested headlessly.

add_library(cfd_core STATIC
    BuildInfo.cpp
    Error.cpp
    Log.cpp)

add_library(cfd::core ALIAS cfd_core)

target_include_directories(cfd_core
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
        # Version.hpp is generated at configure time and lives in the build
        # tree, so consumers need this directory on their include path too.
        "$<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/generated>")

target_compile_features(cfd_core PUBLIC cxx_std_20)

# Build facts baked into the binary. $<CONFIG> is a generator expression: it
# resolves to the configuration being built, which is only known at build time
# for multi-config generators such as Xcode and Visual Studio.
target_compile_definitions(cfd_core PRIVATE
    CFD_BUILD_TYPE="$<CONFIG>"
    CFD_COMPILER_ID="${CMAKE_CXX_COMPILER_ID}"
    CFD_COMPILER_VERSION="${CMAKE_CXX_COMPILER_VERSION}")

# PRIVATE: our warning set applies while compiling cfd_core, but is not forced
# on anything that merely links against it.
target_link_libraries(cfd_core PRIVATE cfd::warnings)

# Logging touches std::mutex and std::atomic.
find_package(Threads REQUIRED)
target_link_libraries(cfd_core PUBLIC Threads::Threads)
