# cfd_geometry - airfoil shape definition.
#
# Sits directly on cfd_core and, like it, has no knowledge of windows or
# OpenGL. The geometry is what the mesher will consume in Phase 2 and what the
# solver ultimately imposes boundary conditions on, so it has to stay usable
# from a headless batch run.

add_library(cfd_geometry STATIC
    Airfoil.cpp
    Naca4.cpp)

add_library(cfd::geometry ALIAS cfd_geometry)

target_include_directories(cfd_geometry
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>")

target_compile_features(cfd_geometry PUBLIC cxx_std_20)

target_link_libraries(cfd_geometry
    PUBLIC
        cfd::core
    PRIVATE
        cfd::warnings)
