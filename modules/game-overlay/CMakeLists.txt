cmake_minimum_required(VERSION 3.24)
project(shroudforge_overlay LANGUAGES CXX)

include(FetchContent)
set(VULKAN_HEADERS_ENABLE_MODULE OFF CACHE BOOL "" FORCE)
FetchContent_Declare(vulkan_headers
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
    GIT_TAG v1.4.304
    GIT_SHALLOW TRUE)
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.92.9b
    GIT_SHALLOW TRUE)
FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG 2c980bb59875b0d32144a71867fbdebb2f77cd20)
FetchContent_MakeAvailable(vulkan_headers imgui stb)

set(JSON_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/../game-runtime/native/json/single_include)

add_library(shroudforge_overlay SHARED
    src/log.cpp
    src/layer_entry.cpp
    src/renderer.cpp
    src/scene.cpp
    src/draw_scene.cpp
    src/load_image.cpp
    src/parse_icon_path.cpp
    src/build_voxel_mesh.cpp
    src/player_options.cpp
    src/local_camera.cpp
    src/camera_probe.cpp
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp)
target_include_directories(shroudforge_overlay PRIVATE src ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends ${stb_SOURCE_DIR} ${JSON_INCLUDE})
target_link_libraries(shroudforge_overlay PRIVATE Vulkan::Headers)
target_compile_features(shroudforge_overlay PRIVATE cxx_std_20)
target_compile_definitions(shroudforge_overlay PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX VK_NO_PROTOTYPES IMGUI_IMPL_VULKAN_NO_PROTOTYPES
    IMGUI_DISABLE_DEMO_WINDOWS IMGUI_DISABLE_DEBUG_TOOLS IMGUI_DISABLE_WIN32_FUNCTIONS IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
set_target_properties(shroudforge_overlay PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded")

add_executable(overlay_present_test tests/present_test.cpp)
target_link_libraries(overlay_present_test PRIVATE Vulkan::Headers)
target_compile_features(overlay_present_test PRIVATE cxx_std_20)
target_compile_definitions(overlay_present_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
