# cfd_flow - solver state and boundary conditions.
#
# Sits on cfd_mesh: the flow lives on a mesh, and the boundary conditions are
# applied to its faces. Like everything below the application, it knows nothing
# about windows, so a solve can run headless.
#
# There is no solver here yet. This phase provides the state a solver operates
# on and the boundary values it will assemble fluxes from.

add_library(cfd_flow STATIC
    BoundaryConditions.cpp
    FlowField.cpp
    Freestream.cpp)

add_library(cfd::flow ALIAS cfd_flow)

target_include_directories(cfd_flow
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>")

target_compile_features(cfd_flow PUBLIC cxx_std_20)

target_link_libraries(cfd_flow
    PUBLIC
        cfd::mesh
    PRIVATE
        cfd::warnings)
