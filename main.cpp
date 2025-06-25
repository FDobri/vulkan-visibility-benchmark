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
};
}
