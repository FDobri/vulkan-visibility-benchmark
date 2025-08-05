cmake_minimum_required(VERSION 3.24)
project(vulkan_visibility LANGUAGES C CXX)
include(FetchContent)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
set(VOLK_PULL_IN_VULKAN OFF CACHE BOOL "" FORCE)
FetchContent_Declare(vulkan_headers URL https://codeload.github.com/KhronosGroup/Vulkan-Headers/tar.gz/refs/tags/v1.4.309)
FetchContent_Declare(volk URL https://codeload.github.com/zeux/volk/tar.gz/refs/tags/vulkan-sdk-1.4.309.0)
FetchContent_Declare(glfw URL https://codeload.github.com/glfw/glfw/tar.gz/refs/tags/3.4)
FetchContent_Declare(glm URL https://codeload.github.com/g-truc/glm/tar.gz/refs/tags/1.0.1)
FetchContent_Declare(vma URL https://codeload.github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/tar.gz/refs/tags/v3.2.1)
FetchContent_Declare(imgui URL https://codeload.github.com/ocornut/imgui/tar.gz/refs/tags/v1.91.9b)
FetchContent_MakeAvailable(vulkan_headers volk glfw glm vma imgui)
target_link_libraries(volk PUBLIC Vulkan::Headers)

find_program(GLSLANG NAMES glslang glslangValidator HINTS "$ENV{VULKAN_SDK}/Bin" REQUIRED)
set(shader_dir "${CMAKE_CURRENT_BINARY_DIR}/shaders")
foreach(shader cull.comp scene.vert scene.frag)
    add_custom_command(OUTPUT "${shader_dir}/${shader}.spv"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${shader_dir}"
        COMMAND "${GLSLANG}" -V --target-env vulkan1.2
            -o "${shader_dir}/${shader}.spv" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}"
        DEPENDS "shaders/${shader}" VERBATIM)
    list(APPEND outputs "${shader_dir}/${shader}.spv")
endforeach()
add_custom_target(spirv DEPENDS ${outputs})
add_executable(visibility main.cpp renderer.cpp
    ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp)
add_dependencies(visibility spirv)
target_compile_definitions(visibility PRIVATE GLFW_INCLUDE_NONE IMGUI_IMPL_VULKAN_USE_VOLK
    VMA_STATIC_VULKAN_FUNCTIONS=0 VMA_DYNAMIC_VULKAN_FUNCTIONS=1 GLM_FORCE_DEPTH_ZERO_TO_ONE VIS_SHADER_DIR="${shader_dir}")
target_include_directories(visibility PRIVATE "${imgui_SOURCE_DIR}" "${vma_SOURCE_DIR}/include")
target_link_libraries(visibility PRIVATE glfw volk glm::glm)
