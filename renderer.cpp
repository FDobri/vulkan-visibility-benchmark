#include "renderer.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

void check(VkResult result) {
    if (result != VK_SUCCESS) throw std::runtime_error("Vulkan result " + std::to_string(result));
}
Camera make_camera(float angle, float aspect, std::uint32_t count) {
    Camera c{};
    const glm::vec3 eye = {std::sin(angle) * 45, 12, std::cos(angle) * 45};
    const glm::vec3 forward = glm::normalize(glm::vec3(0, 0, 0) - eye);
    const glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0, 1, 0)));
    const glm::vec3 up = glm::cross(right, forward);
    auto projection = glm::perspective(glm::radians(49.0f), aspect, 0.5f, 110.0f);
    projection[1][1] *= -1;
    c.vp = projection * glm::lookAt(eye, eye + forward, up);
    const auto t = glm::transpose(c.vp);
    c.planes = {t[3] + t[0], t[3] - t[0], t[3] + t[1], t[3] - t[1], t[2], t[3] - t[2]};
    for (auto& plane : c.planes) plane /= glm::length(glm::vec3(plane));
    c.right = glm::vec4(right, 0);
    c.up = glm::vec4(up, 0);
    c.count = count;
    return c;
}
namespace {
bool visible(const Camera& camera, glm::vec4 sphere) {
    const auto center = glm::vec4(glm::vec3(sphere), 1);
    return std::all_of(camera.planes.begin(), camera.planes.end(), [&](auto p) {
        return glm::dot(p, center) >= -sphere.w;
    });
}
}

Renderer::~Renderer() {
    if (!device_) return;
    if (queries_) vkDestroyQueryPool(device_, queries_, nullptr);
    if (graphics_) vkDestroyPipeline(device_, graphics_, nullptr);
    if (compute_) vkDestroyPipeline(device_, compute_, nullptr);
    if (layout_) vkDestroyPipelineLayout(device_, layout_, nullptr);
    if (pool_) vkDestroyDescriptorPool(device_, pool_, nullptr);
    if (set_layout_) vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr);
    if (allocator_) {
        for (auto b : {objects_, indices_, command_, camera_})
            if (b.handle) vmaDestroyBuffer(allocator_, b.handle, b.allocation);
        vmaDestroyAllocator(allocator_);
    }
}
Renderer::Buffer Renderer::allocate(VkDeviceSize bytes, VkBufferUsageFlags usage, bool readback) {
    VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer.size = bytes;
    buffer.usage = usage;
    VmaAllocationCreateInfo memory{};
    memory.usage = VMA_MEMORY_USAGE_AUTO;
    memory.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
        (readback ? VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT :
                    VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
    VmaAllocationInfo info{};
    Buffer result{};
    check(vmaCreateBuffer(allocator_, &buffer, &memory, &result.handle, &result.allocation, &info));
    result.mapped = info.pMappedData;
    return result;
}
VkShaderModule Renderer::shader(const char* file) {
    const auto path = std::filesystem::path(VIS_SHADER_DIR) / file;
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    const auto end = stream.tellg();
    if (end <= 0 || end % 4 != 0) throw std::runtime_error("invalid SPIR-V: " + path.string());
    std::vector<std::uint32_t> words(static_cast<std::size_t>(end) / 4);
    stream.seekg(0);
    if (!stream.read(reinterpret_cast<char*>(words.data()), end))
        throw std::runtime_error("could not read SPIR-V");
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = words.size() * sizeof(words[0]);
    info.pCode = words.data();
    VkShaderModule module{};
    check(vkCreateShaderModule(device_, &info, nullptr, &module));
    return module;
}
void Renderer::init(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
