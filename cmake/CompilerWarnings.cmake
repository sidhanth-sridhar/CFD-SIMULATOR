# CompilerWarnings.cmake
#
# Defines cfd_set_project_warnings(<target>), which attaches a strict warning
# set to an INTERFACE target. We apply these only to first-party targets so
# that third-party code (GLFW, Dear ImGui, GoogleTest) does not drown the
# build log in warnings we cannot fix.
#
# The numeric-conversion warnings (-Wconversion, -Wsign-conversion,
# -Wdouble-promotion) matter more than usual here: a CFD solver silently
# truncating a double to a float, or mixing signed and unsigned index types,
# produces wrong numbers rather than a crash.

function(cfd_set_project_warnings target)
  set(_common
      -Wall
      -Wextra
      -Wpedantic
      -Wshadow                # a local hiding an outer name is nearly always a bug
      -Wnon-virtual-dtor      # deleting through a base pointer without a virtual dtor
      -Wold-style-cast        # force static_cast/reinterpret_cast so intent is explicit
      -Wcast-align
      -Wunused
      -Woverloaded-virtual    # accidentally hiding rather than overriding
      -Wconversion            # implicit narrowing (double -> float, long -> int)
      -Wsign-conversion       # signed/unsigned mismatches in indexing arithmetic
      -Wnull-dereference
      -Wdouble-promotion      # silent float -> double promotion in mixed expressions
      -Wformat=2
      -Wimplicit-fallthrough)

  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    list(APPEND _common
         -Wduplicated-cond
         -Wduplicated-branches
         -Wlogical-op
         -Wuseless-cast)
  endif()

  if(MSVC)
    set(_common /W4 /permissive-)
  endif()

  if(CFD_WARNINGS_AS_ERRORS)
    if(MSVC)
      list(APPEND _common /WX)
    else()
      list(APPEND _common -Werror)
    endif()
  endif()

  target_compile_options(${target} INTERFACE ${_common})
endfunction()
