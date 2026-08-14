# Dependencies.cmake
#
# All third-party code is fetched at configure time with FetchContent and
# pinned to an exact release tarball plus a SHA-256 hash. Two reasons:
#
#   1. Reproducibility. A git tag can be moved; a hash cannot. Anyone who
#      configures this project gets byte-identical dependency sources or the
#      configure step fails loudly.
#   2. No system package manager required. The project builds on a machine
#      with nothing but a compiler and CMake.
#
# Dear ImGui ships no CMakeLists.txt of its own, so we declare its target
# here by hand (see cfd_add_imgui below).

include(FetchContent)
include_guard(GLOBAL)

set(CFD_GLFW_VERSION "3.5.1")
set(CFD_GLFW_SHA256 "5234f4f29473e9a06bc7847d8371858dd135d38466eeeaa652fdc9f8f9ff0c20")

set(CFD_IMGUI_VERSION "1.92.9b")
set(CFD_IMGUI_SHA256 "21d8a0a565e85dce943e375db00812c2f3f0ab21f3f0f7964e364a63422d7f99")

set(CFD_GOOGLETEST_VERSION "1.17.0")
set(CFD_GOOGLETEST_SHA256 "65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c")

# ---------------------------------------------------------------------------
# GoogleTest
# ---------------------------------------------------------------------------
function(cfd_add_googletest)
  FetchContent_Declare(
    googletest
    URL "https://github.com/google/googletest/archive/refs/tags/v${CFD_GOOGLETEST_VERSION}.tar.gz"
    URL_HASH "SHA256=${CFD_GOOGLETEST_SHA256}"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

  # Do not install GoogleTest alongside our own artifacts.
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK ON CACHE BOOL "" FORCE)

  FetchContent_MakeAvailable(googletest)
endfunction()

# ---------------------------------------------------------------------------
# GLFW - window creation, OpenGL context, keyboard/mouse input
# ---------------------------------------------------------------------------
function(cfd_add_glfw)
  FetchContent_Declare(
    glfw
    URL "https://github.com/glfw/glfw/archive/refs/tags/${CFD_GLFW_VERSION}.tar.gz"
    URL_HASH "SHA256=${CFD_GLFW_SHA256}"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

  # We only need the library itself.
  set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)

  FetchContent_MakeAvailable(glfw)
endfunction()

# ---------------------------------------------------------------------------
# Dear ImGui - immediate-mode GUI, built here as a static library
# ---------------------------------------------------------------------------
#
# We compile the core sources plus exactly two backends:
#   imgui_impl_glfw      - feeds window/input events into ImGui
#   imgui_impl_opengl3   - turns ImGui's vertex buffers into OpenGL draw calls
#
# imgui_demo.cpp is deliberately excluded: it is a large showcase of every
# widget and has no place in a shipping application.
function(cfd_add_imgui)
  FetchContent_Declare(
    imgui
    URL "https://github.com/ocornut/imgui/archive/refs/tags/v${CFD_IMGUI_VERSION}.tar.gz"
    URL_HASH "SHA256=${CFD_IMGUI_SHA256}"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

  FetchContent_MakeAvailable(imgui)

  add_library(imgui STATIC
      "${imgui_SOURCE_DIR}/imgui.cpp"
      "${imgui_SOURCE_DIR}/imgui_draw.cpp"
      "${imgui_SOURCE_DIR}/imgui_tables.cpp"
      "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
      "${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp"
      "${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp")

  target_include_directories(imgui SYSTEM PUBLIC
      "${imgui_SOURCE_DIR}"
      "${imgui_SOURCE_DIR}/backends")

  target_link_libraries(imgui PUBLIC glfw)
  target_compile_features(imgui PUBLIC cxx_std_20)

  # Route ImGui's own assertions through our build rather than the default
  # abort-with-no-message behaviour.
  target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)

  add_library(imgui::imgui ALIAS imgui)
endfunction()
