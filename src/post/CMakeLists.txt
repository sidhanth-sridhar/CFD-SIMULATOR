# cfd_post - turning a solved field into the quantities a section is judged by.
#
# Sits on cfd_flow rather than on cfd_solver: extracting surface data needs a
# mesh and a field, not the machinery that produced them. Keeping it off the
# solver means post-processing can be run on a field loaded from anywhere.

add_library(cfd_post STATIC
    SurfaceData.cpp)

add_library(cfd::post ALIAS cfd_post)

target_include_directories(cfd_post
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>")

target_compile_features(cfd_post PUBLIC cxx_std_20)

target_link_libraries(cfd_post
    PUBLIC
        cfd::flow
    PRIVATE
        cfd::warnings)
