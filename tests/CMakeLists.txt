# Unit tests.
#
# One executable for the whole suite: linking is faster and GoogleTest is
# compiled once. gtest_discover_tests then registers each TEST() with CTest
# individually, so `ctest` reports per-test results rather than one opaque
# pass/fail.

cfd_add_googletest()

include(GoogleTest)

add_executable(cfd_tests
    BuildInfoTests.cpp
    Camera2DTests.cpp
    ErrorTests.cpp
    LogTests.cpp)

target_link_libraries(cfd_tests
    PRIVATE
        cfd::core
        cfd::warnings
        GTest::gtest_main)

# Fed to BuildInfoTests to cross-check the generated version header.
target_compile_definitions(cfd_tests PRIVATE
    CFD_EXPECTED_VERSION="${PROJECT_VERSION}")

gtest_discover_tests(cfd_tests
    # Without this, a test binary that crashes on startup produces a confusing
    # "no tests found" instead of a failure.
    DISCOVERY_MODE PRE_TEST
    PROPERTIES TIMEOUT 60)

# The application binary is also expected to work; --self-check exercises
# startup and shutdown without needing a display.
if(CFD_BUILD_APP)
  add_test(NAME app_self_check
           COMMAND cfd_sim --self-check)
  set_tests_properties(app_self_check PROPERTIES TIMEOUT 60)
endif()
