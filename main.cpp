#include "renderer.hpp"
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_vulkan.h>
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace {
using Clock = std::chrono::steady_clock;
class App {
public:
    ~App() {
        if (device) vkDeviceWaitIdle(device);
        renderer.reset();
        stop_ui();
        if (surface_ready) ImGui_ImplVulkanH_DestroyWindow(instance, device, &swap, nullptr);
        else if (swap.Surface) vkDestroySurfaceKHR(instance, swap.Surface, nullptr);
        if (device) vkDestroyDevice(device, nullptr);
        if (instance) vkDestroyInstance(instance, nullptr);
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
    }
    void start(bool visible) {
        if (!glfwInit()) throw std::runtime_error("GLFW initialization failed");
        check(volkInitialize());
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_VISIBLE, visible);
        window = glfwCreateWindow(1024, 640, "Vulkan visibility", nullptr, nullptr);
        if (!window) throw std::runtime_error("could not create GLFW window");
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "Visibility benchmark";
        app.apiVersion = VK_API_VERSION_1_2;
        VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        info.pApplicationInfo = &app;
        info.ppEnabledExtensionNames = glfwGetRequiredInstanceExtensions(&info.enabledExtensionCount);
        if (!info.ppEnabledExtensionNames) throw std::runtime_error("missing platform surface extensions");
        check(vkCreateInstance(&info, nullptr, &instance));
        volkLoadInstance(instance);
        check(glfwCreateWindowSurface(instance, window, nullptr, &swap.Surface));
        select_device();
        float priority = 1;
        VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                  nullptr, 0, family, 1, &priority};
        const char* extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        VkDeviceCreateInfo d{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        d.queueCreateInfoCount = 1;
        d.pQueueCreateInfos = &q;
        d.enabledExtensionCount = 1;
        d.ppEnabledExtensionNames = extensions;
        check(vkCreateDevice(physical, &d, nullptr, &device));
        volkLoadDevice(device);
        vkGetDeviceQueue(device, family, 0, &queue);
        const VkFormat formats[] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM};
        const VkPresentModeKHR modes[] = {VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_FIFO_KHR};
        swap.SurfaceFormat = ImGui_ImplVulkanH_SelectSurfaceFormat(
            physical, swap.Surface, formats, 2, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR);
        swap.PresentMode = ImGui_ImplVulkanH_SelectPresentMode(
            physical, swap.Surface, visible ? modes + 1 : modes, visible ? 1 : 2);
        swap.ClearValue.color = {{0.025f, 0.04f, 0.062f, 1}};
        int width, height;
        glfwGetFramebufferSize(window, &width, &height);
        resize(width, height);
        renderer = std::make_unique<Renderer>();
        renderer->init(instance, physical, device, swap.RenderPass, timestamp_bits);
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physical, &properties);
        std::cerr << properties.deviceName << " | " << swap.Width << 'x' << swap.Height << '\n';
    }
    void interactive() {
        bool gpu = true, orbit = true;
        float angle = .43f;
        int count = 65536;
        Timing timing{};
        const auto start = Clock::now();
        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();
            int width, height;
            glfwGetFramebufferSize(window, &width, &height);
            if (!width || !height) { glfwWaitEvents(); continue; }
            if (rebuild || width != swap.Width || height != swap.Height) resize(width, height);
            ImGui_ImplVulkan_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();
            ImGui::SetNextWindowPos({16, 16}, ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize({345, 178}, ImGuiCond_FirstUseEver);
            ImGui::Begin("Visibility benchmark");
            ImGui::Checkbox("Compute culling", &gpu);
            ImGui::Checkbox("Orbit camera", &orbit);
            ImGui::SliderInt("Scene objects", &count, 1024, max_objects, "%d", ImGuiSliderFlags_Logarithmic);
            if (orbit) angle = std::chrono::duration<float>(Clock::now() - start).count() * .22f + .43f;
            ImGui::Text("Visible: %u", timing.visible);
            ImGui::Text("CPU work: %.1f us", timing.cpu_us);
            ImGui::Text("GPU cull phase: %.1f us", timing.gpu_cull_us);
            ImGui::Text("GPU through scene draw: %.1f us", timing.gpu_total_us);
            ImGui::End();
            ImGui::Render();
            if (auto current = frame(camera(angle, count), gpu, ImGui::GetDrawData())) timing = *current;
        }
    }
    void benchmark(bool smoke) {
        std::cout << "objects,path,visible,cpu_us,gpu_cull_us,gpu_total_us\n";
        for (std::uint32_t count : {4096u, 65536u, max_objects}) {
            const auto view = camera(.43f, count);
            const auto expected = renderer->reference_count(view);
            const int samples = smoke ? 2 : 120;
            const int warmup = smoke ? 0 : 12;
            for (bool gpu : {false, true}) {
                std::vector<double> cpu, cull, total;
                for (int i = 0; i < samples + warmup; ++i) {
                    glfwPollEvents();
                    if (rebuild) {
                        int width, height;
                        glfwGetFramebufferSize(window, &width, &height);
                        if (!width || !height) throw std::runtime_error("framebuffer unavailable");
                        resize(width, height);
                    }
                    auto result = frame(view, gpu, nullptr);
                    if (!result) { --i; continue; }
                    if (result->visible != expected)
                        throw std::runtime_error("CPU/GPU visible count disagreed");
                    if (i >= warmup) {
                        cpu.push_back(result->cpu_us);
                        cull.push_back(result->gpu_cull_us);
                        total.push_back(result->gpu_total_us);
                    }
                }
                auto median = [](std::vector<double>& values) {
                    std::sort(values.begin(), values.end());
                    return (values[values.size() / 2] + values[(values.size() - 1) / 2]) / 2;
                };
                std::cout << count << ',' << (gpu ? "gpu" : "cpu") << ',' << expected << ','
                          << std::fixed << std::setprecision(2) << median(cpu) << ','
                          << median(cull) << ',' << median(total) << '\n';
            }
        }
    }
private:
    GLFWwindow* window{};
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    VkQueue queue{};
    std::uint32_t family{}, timestamp_bits{};
    ImGui_ImplVulkanH_Window swap;
    std::unique_ptr<Renderer> renderer;
    bool surface_ready{}, ui_glfw{}, ui_vulkan{}, rebuild{};

    void select_device() {
        std::uint32_t count{};
        check(vkEnumeratePhysicalDevices(instance, &count, nullptr));
        std::vector<VkPhysicalDevice> gpus(count);
        check(vkEnumeratePhysicalDevices(instance, &count, gpus.data()));
        int best = 0;
        for (auto gpu : gpus) {
            VkPhysicalDeviceProperties p{};
            vkGetPhysicalDeviceProperties(gpu, &p);
            if (p.apiVersion < VK_API_VERSION_1_2 || !p.limits.timestampComputeAndGraphics) continue;
            VkFormatProperties format{};
            vkGetPhysicalDeviceFormatProperties(gpu, VK_FORMAT_B8G8R8A8_UNORM, &format);
            if (!(format.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT)) continue;
            std::uint32_t queue_count{};
            vkGetPhysicalDeviceQueueFamilyProperties(gpu, &queue_count, nullptr);
            std::vector<VkQueueFamilyProperties> queues(queue_count);
            vkGetPhysicalDeviceQueueFamilyProperties(gpu, &queue_count, queues.data());
            for (std::uint32_t i = 0; i < queue_count; ++i) {
                VkBool32 present{};
                check(vkGetPhysicalDeviceSurfaceSupportKHR(gpu, i, swap.Surface, &present));
                constexpr auto flags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
                if (!present || !queues[i].timestampValidBits || (queues[i].queueFlags & flags) != flags) continue;
                const auto score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
                if (score > best) {
                    best = score;
                    physical = gpu;
                    family = i;
                    timestamp_bits = queues[i].timestampValidBits;
                }
                break;
            }
        }
        if (!physical) throw std::runtime_error("Vulkan 1.2 with graphics, compute, presentation and timestamps is required");
    }
    void stop_ui() noexcept {
        if (ui_vulkan) { ImGui_ImplVulkan_Shutdown(); ui_vulkan = false; }
        if (ui_glfw) { ImGui_ImplGlfw_Shutdown(); ui_glfw = false; }
        if (ImGui::GetCurrentContext()) ImGui::DestroyContext();
    }
    void resize(int width, int height) {
        check(vkDeviceWaitIdle(device));
        stop_ui();
        ImGui_ImplVulkanH_CreateOrResizeWindow(instance, physical, device, &swap, family,
                                               nullptr, width, height, 2);
        surface_ready = true;
        rebuild = false;
        if (renderer) renderer->make_graphics(swap.RenderPass);
        ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        ui_glfw = ImGui_ImplGlfw_InitForVulkan(window, true);
        ImGui_ImplVulkan_InitInfo init{};
        init.ApiVersion = VK_API_VERSION_1_2;
        init.Instance = instance; init.PhysicalDevice = physical; init.Device = device;
        init.QueueFamily = family; init.Queue = queue;
        init.RenderPass = swap.RenderPass;
        init.MinImageCount = 2; init.ImageCount = swap.ImageCount;
        init.DescriptorPoolSize = 16;
        init.CheckVkResultFn = check;
        ui_vulkan = ImGui_ImplVulkan_Init(&init);
        if (!ui_glfw || !ui_vulkan) throw std::runtime_error("could not initialize ImGui");
    }
    Camera camera(float angle, std::uint32_t count) const {
        return make_camera(angle, static_cast<float>(swap.Width) / swap.Height, count);
    }
    std::optional<Timing> frame(const Camera& view, bool gpu, ImDrawData* overlay) {
        const auto available = swap.FrameSemaphores[swap.SemaphoreIndex].ImageAcquiredSemaphore;
        std::uint32_t index{};
        auto result = vkAcquireNextImageKHR(device, swap.Swapchain, UINT64_MAX, available, nullptr, &index);
        if (result == VK_ERROR_OUT_OF_DATE_KHR) { rebuild = true; return std::nullopt; }
        if (result == VK_SUBOPTIMAL_KHR) rebuild = true; else check(result);
        auto& slot = swap.Frames[index];
        const auto complete = swap.FrameSemaphores[index].RenderCompleteSemaphore;
        check(vkWaitForFences(device, 1, &slot.Fence, VK_TRUE, UINT64_MAX));
        check(vkResetFences(device, 1, &slot.Fence));
        check(vkResetCommandPool(device, slot.CommandPool, 0));
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                        nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        check(vkBeginCommandBuffer(slot.CommandBuffer, &begin));
        VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, nullptr, swap.RenderPass,
                                   slot.Framebuffer, {{0, 0}, {std::uint32_t(swap.Width), std::uint32_t(swap.Height)}},
                                   1, &swap.ClearValue};
        renderer->record(slot.CommandBuffer, pass, view, gpu);
        if (overlay) ImGui_ImplVulkan_RenderDrawData(overlay, slot.CommandBuffer);
        vkCmdEndRenderPass(slot.CommandBuffer);
        check(vkEndCommandBuffer(slot.CommandBuffer));
        VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 1, &available, &wait,
                            1, &slot.CommandBuffer, 1, &complete};
        check(vkQueueSubmit(queue, 1, &submit, slot.Fence));
        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, nullptr, 1, &complete, 1,
                                 &swap.Swapchain, &index, nullptr};
        result = vkQueuePresentKHR(queue, &present);
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) rebuild = true;
        else check(result);
        swap.SemaphoreIndex = (swap.SemaphoreIndex + 1) % swap.SemaphoreCount;
        check(vkWaitForFences(device, 1, &slot.Fence, VK_TRUE, UINT64_MAX));
        return renderer->read();
    }
};
}
