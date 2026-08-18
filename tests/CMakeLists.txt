# Unit tests.
#
# One executable for the whole suite: linking is faster and GoogleTest is
# compiled once. gtest_discover_tests then registers each TEST() with CTest
# individually, so `ctest` reports per-test results rather than one opaque
# pass/fail.

cfd_add_googletest()

include(GoogleTest)

add_executable(cfd_tests
    AirfoilTests.cpp
    BoundaryConditionTests.cpp
    BuildInfoTests.cpp
    CGridTests.cpp
    Camera2DTests.cpp
    ErrorTests.cpp
    ForceTests.cpp
    FlowFieldTests.cpp
    LogTests.cpp
    MeshTests.cpp
    Naca4Tests.cpp
    PolarTests.cpp
    SolverTests.cpp
    TurbulenceTests.cpp
    SurfaceDataTests.cpp
    ValidationTests.cpp)

# cfd::solver carries flow, mesh, geometry and core PUBLICly, so naming those
# again here would only make the linker warn about duplicates.
target_link_libraries(cfd_tests
    PRIVATE
        cfd::solver
        cfd::post
        cfd::warnings
        GTest::gtest_main)

# Fed to BuildInfoTests to cross-check the generated version header.
target_compile_definitions(cfd_tests PRIVATE
    CFD_EXPECTED_VERSION="${PROJECT_VERSION}")

# Most of these run in milliseconds, but the validation cases solve real flows
# to convergence and take tens of seconds - the Blasius plate around ten on an
# idle machine, more when the suite has been running long enough to warm it up.
# The timeout is there to catch a hang, not to police runtime, so it is set well
# clear of the slowest legitimate case rather than a small multiple of it.
gtest_discover_tests(cfd_tests
    # Without this, a test binary that crashes on startup produces a confusing
    # "no tests found" instead of a failure.
    DISCOVERY_MODE PRE_TEST
    PROPERTIES TIMEOUT 300)

# The application binary is also expected to work; --self-check exercises
# startup and shutdown without needing a display.
if(CFD_BUILD_APP)
  add_test(NAME app_self_check
           COMMAND cfd_sim --self-check)
  set_tests_properties(app_self_check PROPERTIES TIMEOUT 60)
endif()
