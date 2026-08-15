# cfd_solver - the numerics.
#
# Sits on cfd_flow, and like everything below the application knows nothing
# about windows: a solve must be runnable headless, which is how it is
# verified against exact solutions in the test suite.

add_library(cfd_solver STATIC
    Gradient.cpp
    LinearSystem.cpp
    SimpleSolver.cpp)

add_library(cfd::solver ALIAS cfd_solver)

target_include_directories(cfd_solver
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>")

target_compile_features(cfd_solver PUBLIC cxx_std_20)

target_link_libraries(cfd_solver
    PUBLIC
        cfd::flow
    PRIVATE
        cfd::warnings)
