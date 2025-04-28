#pragma once
#include <volk.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>
#include <array>
#include <cstdint>
#include <vector>

void check(VkResult result);
constexpr std::uint32_t max_objects = 262144;
struct alignas(16) Camera {
    glm::mat4 vp;
    std::array<glm::vec4, 6> planes;
    glm::vec4 right, up;
    alignas(16) std::uint32_t count;
};
static_assert(offsetof(Camera, count) == 192);
Camera make_camera(float angle, float aspect, std::uint32_t count);

struct Timing {
    std::uint32_t visible{};
    double cpu_us{}, gpu_cull_us{}, gpu_total_us{};
};
class Renderer {
public:
    Renderer() = default;
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    void init(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
              VkRenderPass pass, std::uint32_t timestamp_bits);
    void make_graphics(VkRenderPass pass);
    void record(VkCommandBuffer cmd, VkRenderPassBeginInfo pass, const Camera& camera, bool gpu);
    Timing read();
    std::uint32_t reference_count(const Camera& camera) const;
private:
    struct Buffer {
        VkBuffer handle{};
        VmaAllocation allocation{};
        void* mapped{};
    };
    Buffer allocate(VkDeviceSize bytes, VkBufferUsageFlags usage, bool readback = false);
    VkShaderModule shader(const char* file);
    VkDevice device_{};
    VmaAllocator allocator_{};
    Buffer objects_, indices_, command_, camera_;
    VkDescriptorSetLayout set_layout_{};
    VkDescriptorPool pool_{};
    VkDescriptorSet set_{};
    VkPipelineLayout layout_{};
    VkPipeline graphics_{}, compute_{};
    VkQueryPool queries_{};
    std::vector<glm::vec4> scene_;
    std::uint64_t timestamp_mask_{};
    float timestamp_period_{};
    double cpu_us_{};
};
