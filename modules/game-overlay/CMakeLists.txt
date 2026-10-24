cmake_minimum_required(VERSION 3.24)
project(shroudforge_overlay LANGUAGES CXX)

include(FetchContent)
set(VULKAN_HEADERS_ENABLE_MODULE OFF CACHE BOOL "" FORCE)
FetchContent_Declare(vulkan_headers
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
    GIT_TAG v1.4.304
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(vulkan_headers)

add_library(shroudforge_overlay SHARED src/overlay_layer.cpp)
target_link_libraries(shroudforge_overlay PRIVATE Vulkan::Headers)
target_compile_features(shroudforge_overlay PRIVATE cxx_std_20)
target_compile_definitions(shroudforge_overlay PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(shroudforge_overlay PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded")
