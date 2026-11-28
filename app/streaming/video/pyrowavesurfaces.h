#pragma once

#include <libplacebo/renderer.h>
#include <libplacebo/vulkan.h>
#include <pyrowave.h>

extern "C" {
#include <libavutil/frame.h>
}

#include <vector>

// The renderer owns the Vulkan device. The pacer must release its frames and
// the codec must destroy its borrowed device before this pool is destroyed.
class PyrowaveSurfaces
{
public:
    static void configure(pl_vulkan_params& params);
    PyrowaveSurfaces(pl_vk_inst instance, pl_vulkan vk, size_t capacity);
    ~PyrowaveSurfaces();
    PyrowaveSurfaces(const PyrowaveSurfaces&) = delete;
    PyrowaveSurfaces& operator=(const PyrowaveSurfaces&) = delete;

    bool createDevice(pyrowave_device* device);
    enum DecodeResult
    {
        Decoded,
        PoolBusy,
        Unavailable
    };
    DecodeResult decode(pyrowave_decoder decoder, AVFrame* frame);
    bool owns(const AVFrame* frame) const;
    void map(const AVFrame* frame, pl_frame* mapped) const;

private:
    struct Surface
    {
        pl_tex planes[3] = {};
        AVBufferRef* ref = nullptr;
    };

    pl_vk_inst m_Instance;
    pl_vulkan m_Vk;
    std::vector<Surface> m_Surfaces;
    VkApplicationInfo m_AppInfo = {};
    VkInstanceCreateInfo m_InstanceInfo = {};
    float m_Priority = 1.0f;
    VkDeviceQueueCreateInfo m_QueueInfo = {};
    VkDeviceCreateInfo m_DeviceInfo = {};
    VkSemaphore m_Ready = VK_NULL_HANDLE;
    VkSemaphore m_Done = VK_NULL_HANDLE;
    uint64_t m_ReadyValue = 0;
};
