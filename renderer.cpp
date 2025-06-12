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
                    VkRenderPass pass, std::uint32_t timestamp_bits) {
    device_ = device;
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical, &properties);
    timestamp_period_ = properties.limits.timestampPeriod;
    timestamp_mask_ = timestamp_bits == 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << timestamp_bits) - 1;
    VmaVulkanFunctions funcs{};
    funcs.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    funcs.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    VmaAllocatorCreateInfo create{};
    create.physicalDevice = physical;
    create.device = device;
    create.instance = instance;
    create.vulkanApiVersion = VK_API_VERSION_1_2;
    create.pVulkanFunctions = &funcs;
    check(vmaCreateAllocator(&create, &allocator_));

    objects_ = allocate(max_objects * sizeof(glm::vec4), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    indices_ = allocate(max_objects * sizeof(std::uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    command_ = allocate(sizeof(VkDrawIndirectCommand),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
    camera_ = allocate(sizeof(Camera), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    scene_.reserve(max_objects);
    std::uint32_t random = 0xf254ca17;
    auto next = [&] {
        random ^= random << 13; random ^= random >> 17; random ^= random << 5;
        return static_cast<float>(random & 65535) / 65535.0f - 0.5f;
    };
    for (std::uint32_t i = 0; i < max_objects; ++i) {
        const auto x = next(), y = next(), z = next(), r = next();
        scene_.emplace_back(x * 128, y * 48, z * 128, .095f + (r + .5f) * .085f);
    }
    std::memcpy(objects_.mapped, scene_.data(), max_objects * sizeof(glm::vec4));
    check(vmaFlushAllocation(allocator_, objects_.allocation, 0, VK_WHOLE_SIZE));

    const std::array<VkDescriptorSetLayoutBinding, 4> bindings{{
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT, nullptr}
    }};
    VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr,
                                             0, static_cast<std::uint32_t>(bindings.size()), bindings.data()};
    check(vkCreateDescriptorSetLayout(device_, &set_info, nullptr, &set_layout_));
    const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3},
                                           {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, 1, 2, sizes};
    check(vkCreateDescriptorPool(device_, &pool, nullptr, &pool_));
    VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                            nullptr, pool_, 1, &set_layout_};
    check(vkAllocateDescriptorSets(device_, &allocation, &set_));
    const VkBuffer buffers[] = {objects_.handle, indices_.handle, command_.handle, camera_.handle};
    VkDescriptorBufferInfo infos[4]{};
    VkWriteDescriptorSet writes[4]{};
    for (std::uint32_t i = 0; i < 4; ++i) {
        infos[i] = {buffers[i], 0, VK_WHOLE_SIZE};
        writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set_, i, 0, 1,
                     bindings[i].descriptorType, nullptr, &infos[i], nullptr};
    }
    vkUpdateDescriptorSets(device_, 4, writes, 0, nullptr);
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &set_layout_;
    check(vkCreatePipelineLayout(device_, &layout, nullptr, &layout_));
    const auto module = shader("cull.comp.spv");
    VkComputePipelineCreateInfo compute{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    compute.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                     VK_SHADER_STAGE_COMPUTE_BIT, module, "main", nullptr};
    compute.layout = layout_;
    const auto status = vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &compute, nullptr, &compute_);
    vkDestroyShaderModule(device_, module, nullptr);
    check(status);
    VkQueryPoolCreateInfo query{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, nullptr, 0,
                                 VK_QUERY_TYPE_TIMESTAMP, 3, 0};
    check(vkCreateQueryPool(device_, &query, nullptr, &queries_));
    make_graphics(pass);
}
void Renderer::make_graphics(VkRenderPass pass) {
    if (graphics_) { vkDestroyPipeline(device_, graphics_, nullptr); graphics_ = VK_NULL_HANDLE; }
    const auto vertex = shader("scene.vert.spv");
    VkShaderModule fragment{};
    try { fragment = shader("scene.frag.spv"); }
    catch (...) { vkDestroyShaderModule(device_, vertex, nullptr); throw; }
    const VkPipelineShaderStageCreateInfo stages[] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vertex, "main"},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fragment, "main"}};
    VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState attachment{};
    attachment.colorWriteMask = 0xf;
    attachment.blendEnable = VK_TRUE;
    attachment.srcColorBlendFactor = attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &attachment;
    const VkDynamicState states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, nullptr, 0, 2, states};
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.stageCount = 2; info.pStages = stages;
    info.pVertexInputState = &input; info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport; info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample; info.pColorBlendState = &blend; info.pDynamicState = &dynamic;
    info.layout = layout_; info.renderPass = pass;
    const auto status = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &info, nullptr, &graphics_);
    vkDestroyShaderModule(device_, fragment, nullptr);
    vkDestroyShaderModule(device_, vertex, nullptr);
    check(status);
}
std::uint32_t Renderer::reference_count(const Camera& camera) const {
    return static_cast<std::uint32_t>(std::count_if(scene_.begin(), scene_.begin() + camera.count,
                                                  [&](auto sphere) { return visible(camera, sphere); }));
}
void Renderer::record(VkCommandBuffer cmd, VkRenderPassBeginInfo pass, const Camera& camera, bool gpu) {
    using Clock = std::chrono::steady_clock;
    std::memcpy(camera_.mapped, &camera, sizeof(camera));
    check(vmaFlushAllocation(allocator_, camera_.allocation, 0, VK_WHOLE_SIZE));
    const auto start = Clock::now();
    VkDrawIndirectCommand draw{6, 0, 0, 0};
    if (!gpu) {
        auto* ids = static_cast<std::uint32_t*>(indices_.mapped);
        for (std::uint32_t i = 0; i < camera.count; ++i)
            if (visible(camera, scene_[i])) ids[draw.instanceCount++] = i;
        check(vmaFlushAllocation(allocator_, indices_.allocation, 0, VK_WHOLE_SIZE));
    }
    vkCmdResetQueryPool(cmd, queries_, 0, 3);
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries_, 0);
    vkCmdUpdateBuffer(cmd, command_.handle, 0, sizeof(draw), &draw);
    const VkAccessFlags transfer_access = gpu
        ? VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT
        : VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_HOST_READ_BIT;
    VkMemoryBarrier transfer{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_WRITE_BIT, transfer_access};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
        gpu ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_HOST_BIT,
        0, 1, &transfer, 0, nullptr, 0, nullptr);
    if (gpu) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, compute_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &set_, 0, nullptr);
        vkCmdDispatch(cmd, (camera.count + 127) / 128, 1, 1);
        VkMemoryBarrier ready{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_WRITE_BIT,
                              VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_HOST_READ_BIT};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_HOST_BIT,
            0, 1, &ready, 0, nullptr, 0, nullptr);
    }
    cpu_us_ = std::chrono::duration<double, std::micro>(Clock::now() - start).count();
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries_, 1);
    vkCmdBeginRenderPass(cmd, &pass, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport viewport{0, 0, static_cast<float>(pass.renderArea.extent.width),
                        static_cast<float>(pass.renderArea.extent.height), 0, 1};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &pass.renderArea);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, graphics_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set_, 0, nullptr);
    vkCmdDrawIndirect(cmd, command_.handle, 0, 1, sizeof(draw));
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries_, 2);
}
Timing Renderer::read() {
    std::uint64_t times[3]{};
    check(vkGetQueryPoolResults(device_, queries_, 0, 3, sizeof(times), times, sizeof(times[0]),
                                VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
    check(vmaInvalidateAllocation(allocator_, command_.allocation, 0, VK_WHOLE_SIZE));
    VkDrawIndirectCommand command{};
    std::memcpy(&command, command_.mapped, sizeof(command));
    auto duration = [&](int end) {
        return double((times[end] - times[0]) & timestamp_mask_) * timestamp_period_ / 1000.0;
    };
    return {command.instanceCount, cpu_us_, duration(1), duration(2)};
}
