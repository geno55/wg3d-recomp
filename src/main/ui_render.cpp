// SPDX-License-Identifier: GPL-3.0-only
// Draws the launcher / settings menu (src/main/ui.cpp) over RT64's output. The UI is Dear ImGui, which
// RT64 already builds for its developer inspector; its D3D12 and Vulkan backends render through RT64's
// render hooks, the same way RT64's inspector draws (lib/rt64/src/gui/rt64_inspector.cpp).
//
// Threads: ui.cpp builds each ImGui frame on the main thread; the draw hook runs on RT64's present thread
// and renders the last finished frame. Both hold wg3d::ui::frame_mutex() while they touch ImGui. The
// backend is created on the first draw (it needs the swap chain's format) and only then does ui.cpp start
// building frames (backend_ready), so the font texture exists before any draw list refers to it.
#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "rhi/rt64_render_hooks.h"

#include "imgui/imgui.h"
// Volk must come before the ImGui Vulkan backend.
#include "plume_vulkan.h"
#include "imgui/backends/imgui_impl_vulkan.h"
#ifdef _WIN32
#include "plume_d3d12.h"
#include "imgui/backends/imgui_impl_dx12.h"
#endif

#include "wg3d_ui.h"

namespace {
    enum class Backend { None, D3D12, Vulkan, Failed };
    Backend backend = Backend::None;
    std::atomic<bool> ready{ false };
#ifdef _WIN32
    std::unique_ptr<plume::RenderDescriptorSet> d3d12_descriptor_set;
#endif
    VkDevice vk_device = VK_NULL_HANDLE;
    VkDescriptorPool vk_descriptor_pool = VK_NULL_HANDLE;

    void check_vk_result(VkResult res) {
        if (res != VK_SUCCESS) {
            std::fprintf(stderr, "[wg3d] ui: ImGui Vulkan backend error 0x%X\n", (unsigned)res);
        }
    }

    void init_backend(plume::RenderCommandList* list, plume::RenderFramebuffer* framebuffer) {
#ifdef _WIN32
        if (auto* d3d12_list = dynamic_cast<plume::D3D12CommandList*>(list)) {
            auto* fb = static_cast<plume::D3D12Framebuffer*>(framebuffer);
            plume::D3D12Device* device = fb->device;
            plume::RenderDescriptorRange range(plume::RenderDescriptorRangeType::TEXTURE, 0, 1);
            d3d12_descriptor_set = device->createDescriptorSet(plume::RenderDescriptorSetDesc(&range, 1));
            auto* set = static_cast<plume::D3D12DescriptorSet*>(d3d12_descriptor_set.get());
            const D3D12_CPU_DESCRIPTOR_HANDLE cpu = device->viewHeapAllocator->getCPUHandleAt(set->viewAllocation.offset);
            const D3D12_GPU_DESCRIPTOR_HANDLE gpu = device->viewHeapAllocator->getGPUHandleAt(set->viewAllocation.offset);
            const DXGI_FORMAT format = fb->colorTargets[0]->d3d->GetDesc().Format;
            ImGui_ImplDX12_Init(device->d3d, 2, format, device->viewHeapAllocator->heap, cpu, gpu);
            // Font texture now (uploaded on the backend's own queue), not in a later NewFrame.
            ImGui_ImplDX12_CreateDeviceObjects();
            backend = Backend::D3D12;
            std::fprintf(stderr, "[wg3d] ui: D3D12 backend ready\n");
            return;
        }
#endif
        if (auto* vk_list = dynamic_cast<plume::VulkanCommandList*>(list)) {
            auto* fb = static_cast<plume::VulkanFramebuffer*>(framebuffer);
            plume::VulkanDevice* device = fb->device;
            ImGui_ImplVulkan_LoadFunctions([](const char* name, void* instance) {
                return vkGetInstanceProcAddr(*reinterpret_cast<VkInstance*>(instance), name);
            }, &device->renderInterface->instance);
            std::unordered_map<VkDescriptorType, uint32_t> type_counts{ { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 } };
            vk_device = device->vk;
            vk_descriptor_pool = plume::VulkanDescriptorSet::createDescriptorPool(device, type_counts, false);

            ImGui_ImplVulkan_InitInfo info{};
            info.Instance = device->renderInterface->instance;
            info.PhysicalDevice = device->physicalDevice;
            info.Device = device->vk;
            info.QueueFamily = vk_list->queue->familyIndex;
            info.Queue = vk_list->queue->queue->vk;
            info.DescriptorPool = vk_descriptor_pool;
            info.RenderPass = fb->renderPass;  // the swap chain framebuffer's; pipelines stay compatible
            info.Subpass = 0;
            info.MinImageCount = 2;
            info.ImageCount = 2;
            info.CheckVkResultFn = check_vk_result;
            ImGui_ImplVulkan_Init(&info);
            {
                // The upload submits to the queue RT64 presents on.
                std::lock_guard queue_lock{ *vk_list->queue->queue->mutex };
                ImGui_ImplVulkan_CreateFontsTexture();
            }
            backend = Backend::Vulkan;
            std::fprintf(stderr, "[wg3d] ui: Vulkan backend ready\n");
            return;
        }
        std::fprintf(stderr, "[wg3d] ui: unsupported graphics API, no menus\n");
        backend = Backend::Failed;
    }

    void hook_init(plume::RenderInterface*, plume::RenderDevice*) {}

    void hook_draw(plume::RenderCommandList* list, plume::RenderFramebuffer* framebuffer) {
        std::lock_guard lock{ wg3d::ui::frame_mutex() };
        if (ImGui::GetCurrentContext() == nullptr) {
            return;  // wg3d::ui::init not called (no window yet)
        }
        if (backend == Backend::None) {
            init_backend(list, framebuffer);
            ready = backend == Backend::D3D12 || backend == Backend::Vulkan;
        }
        if (!ready || !wg3d::ui::visible()) {
            return;
        }
        ImDrawData* draw_data = ImGui::GetDrawData();
        if (draw_data == nullptr || !draw_data->Valid || draw_data->CmdListsCount == 0) {
            return;
        }
#ifdef _WIN32
        if (backend == Backend::D3D12) {
            auto* d3d12_list = static_cast<plume::D3D12CommandList*>(list);
            d3d12_list->checkDescriptorHeaps();
            ImGui_ImplDX12_RenderDrawData(draw_data, d3d12_list->d3d);
            return;
        }
#endif
        auto* vk_list = static_cast<plume::VulkanCommandList*>(list);
        vk_list->checkActiveRenderPass();
        ImGui_ImplVulkan_RenderDrawData(draw_data, vk_list->vk);
    }

    void hook_deinit() {
        std::lock_guard lock{ wg3d::ui::frame_mutex() };
        ready = false;
#ifdef _WIN32
        if (backend == Backend::D3D12) {
            ImGui_ImplDX12_Shutdown();
            d3d12_descriptor_set.reset();
        }
#endif
        if (backend == Backend::Vulkan) {
            ImGui_ImplVulkan_Shutdown();
            vkDestroyDescriptorPool(vk_device, vk_descriptor_pool, nullptr);
            vk_descriptor_pool = VK_NULL_HANDLE;
        }
        backend = Backend::None;
    }
}

void wg3d::ui::install_render_hooks() {
    RT64::SetRenderHooks(hook_init, hook_draw, hook_deinit);
}

bool wg3d::ui::backend_ready() {
    return ready.load();
}
