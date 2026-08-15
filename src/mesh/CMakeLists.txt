# cfd_mesh - computational domain and grid generation.
#
# Depends on cfd_geometry (it meshes around a section) and, through it, on
# cfd_core. Like both of those it has no knowledge of windows or OpenGL, so a
# mesh can be generated and checked in a headless run.

add_library(cfd_mesh STATIC
    CGrid.cpp
    Mesh.cpp)

add_library(cfd::mesh ALIAS cfd_mesh)

target_include_directories(cfd_mesh
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>")

target_compile_features(cfd_mesh PUBLIC cxx_std_20)

target_link_libraries(cfd_mesh
    PUBLIC
        cfd::geometry
    PRIVATE
        cfd::warnings)
