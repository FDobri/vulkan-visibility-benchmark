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
