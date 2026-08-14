# cfd_app - window, GUI shell and viewport.
#
# This is the only module that knows GLFW, OpenGL or Dear ImGui exist.
# Everything below it (cfd_core today, the solver later) stays buildable and
# testable with no window system present.

cfd_add_glfw()
cfd_add_imgui()

find_package(OpenGL REQUIRED)

add_library(cfd_app STATIC
    Application.cpp
    Panels.cpp
    Theme.cpp)

add_library(cfd::app ALIAS cfd_app)

target_include_directories(cfd_app
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
    PRIVATE
        # Theme.hpp and Panels.hpp are implementation details of this module
        # and are intentionally not on the public include path.
        "${CMAKE_CURRENT_SOURCE_DIR}")

# cfd::geometry is PRIVATE: the viewport draws sections, but Application.hpp
# exposes nothing from the geometry module, so callers do not inherit it.
target_link_libraries(cfd_app
    PUBLIC
        cfd::core
    PRIVATE
        cfd::geometry
        imgui::imgui
        glfw
        OpenGL::GL
        cfd::warnings)

# Keep GLFW from selecting an OpenGL header for us; Application.cpp includes
# the core-profile gl3.h explicitly, and including both is an error on macOS.
target_compile_definitions(cfd_app PRIVATE GLFW_INCLUDE_NONE)

# Silence Apple's OpenGL deprecation notices. OpenGL still works on macOS and
# is the only portable option here; Metal would mean a second renderer.
if(APPLE)
  target_compile_definitions(cfd_app PRIVATE GL_SILENCE_DEPRECATION)
endif()

# ---------------------------------------------------------------------------
# Executable
# ---------------------------------------------------------------------------
add_executable(cfd_sim "${PROJECT_SOURCE_DIR}/src/main.cpp")

# cfd::app propagates cfd::core publicly; listing it again would be a duplicate.
target_link_libraries(cfd_sim
    PRIVATE
        cfd::app
        cfd::warnings)
