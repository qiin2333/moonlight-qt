#include "pyrowavesurfaces.h"

#define PL_LIBAV_IMPLEMENTATION 0
#include <libplacebo/utils/libav.h>

void PyrowaveSurfaces::configure(pl_vulkan_params& params)
{
    static auto vk13 = [] {
        VkPhysicalDeviceVulkan13Features f = {};
        f.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        f.subgroupSizeControl = f.computeFullSubgroups = VK_TRUE;
        return f;
    }();
    static auto vk12 = [] {
        VkPhysicalDeviceVulkan12Features f = {};
        f.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        f.pNext = &vk13;
        f.storageBuffer8BitAccess = f.timelineSemaphore = f.shaderFloat16 = VK_TRUE;
        return f;
    }();
    static VkPhysicalDeviceVulkan11Features vk11 = [] {
        VkPhysicalDeviceVulkan11Features f = {};
        f.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
        f.pNext = &vk12;
        f.storageBuffer16BitAccess = VK_TRUE;
        return f;
    }();
    static const auto features = [] {
        VkPhysicalDeviceFeatures2 f = {};
        f.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        f.pNext = &vk11;
        f.features.shaderInt16 = VK_TRUE;
        return f;
    }();
    params.features = &features;
    // Submit all three plane transitions and the decode on one graphics queue.
    // The last ready signal then covers all three planes, including on reuse.
    params.async_transfer = params.async_compute = false;
    params.queue_count = 1;
}

PyrowaveSurfaces::PyrowaveSurfaces(pl_vk_inst instance, pl_vulkan vk, size_t capacity)
    : m_Instance(instance), m_Vk(vk), m_Surfaces(capacity)
{
}

bool PyrowaveSurfaces::createDevice(pyrowave_device* device)
{
    if (!m_Vk->gpu->limits.thread_safe || m_Instance->api_version < VK_API_VERSION_1_3 ||
        m_Vk->api_version < VK_API_VERSION_1_3 || m_Vk->queue_graphics.count != 1 ||
        m_Vk->queue_graphics.index != m_Vk->queue_compute.index ||
        m_Vk->queue_graphics.index != m_Vk->queue_transfer.index) {
        return false;
    }
    // libplacebo enables requested features opportunistically. Check the
    // enabled device chain before PyroWave creates its 16-bit SPIR-V modules.
    bool storage16 = false, fullSubgroups = false;
    for (auto node = static_cast<const VkBaseInStructure*>(m_Vk->features->pNext); node;
         node = node->pNext) {
        if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES) {
            storage16 = reinterpret_cast<const VkPhysicalDeviceVulkan11Features*>(node)
                            ->storageBuffer16BitAccess;
        } else if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES) {
            auto f = reinterpret_cast<const VkPhysicalDeviceVulkan13Features*>(node);
            fullSubgroups = f->subgroupSizeControl && f->computeFullSubgroups;
        }
    }
    if (!storage16 || !fullSubgroups || !m_Vk->features->features.shaderInt16) {
        return false;
    }
    pl_vulkan_sem_params sem = {};
    sem.type = VK_SEMAPHORE_TYPE_TIMELINE;
    m_Ready = pl_vulkan_sem_create(m_Vk->gpu, &sem);
    m_Done = pl_vulkan_sem_create(m_Vk->gpu, &sem);
    if (!m_Ready || !m_Done) {
        return false;
    }

    m_AppInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    m_AppInfo.apiVersion = m_Instance->api_version;
    m_InstanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    m_InstanceInfo.pApplicationInfo = &m_AppInfo;
    m_InstanceInfo.enabledExtensionCount = m_Instance->num_extensions;
    m_InstanceInfo.ppEnabledExtensionNames = m_Instance->extensions;
    m_QueueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    m_QueueInfo.queueFamilyIndex = m_Vk->queue_graphics.index;
    m_QueueInfo.queueCount = 1;
    m_QueueInfo.pQueuePriorities = &m_Priority;
#if PL_API_VER >= 365
    // vkGetDeviceQueue cannot retrieve queues created with nonzero flags.
    m_QueueInfo.flags = m_Vk->queue_graphics.flags;
    VkDeviceQueueInfo2 queueInfo = {};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2;
    queueInfo.flags = m_QueueInfo.flags;
    queueInfo.queueFamilyIndex = m_QueueInfo.queueFamilyIndex;
    pyrowave_device_create_queue_info queue = {};
    queue.familyIndex = queueInfo.queueFamilyIndex;
    auto getQueue = reinterpret_cast<PFN_vkGetDeviceQueue2>(
        m_Instance->get_proc_addr(m_Instance->instance, "vkGetDeviceQueue2"));
    if (!getQueue) {
        return false;
    }
    getQueue(m_Vk->device, &queueInfo, &queue.queue);
    if (!queue.queue) {
        return false;
    }
#endif
    m_DeviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    m_DeviceInfo.pNext = m_Vk->features;
    m_DeviceInfo.queueCreateInfoCount = 1;
    m_DeviceInfo.pQueueCreateInfos = &m_QueueInfo;
    m_DeviceInfo.enabledExtensionCount = m_Vk->num_extensions;
    m_DeviceInfo.ppEnabledExtensionNames = m_Vk->extensions;

    pyrowave_device_create_info info = {};
    info.GetInstanceProcAddr = m_Instance->get_proc_addr;
    info.instance = m_Instance->instance;
    info.physical_device = m_Vk->phys_device;
    info.device = m_Vk->device;
    info.instance_create_info = &m_InstanceInfo;
    info.device_create_info = &m_DeviceInfo;
#if PL_API_VER >= 365
    info.queue_info = &queue;
    info.queue_info_count = 1;
#endif
    info.userdata = this;
    info.queue_lock_callback = [](void* opaque) {
        auto vk = static_cast<PyrowaveSurfaces*>(opaque)->m_Vk;
        vk->lock_queue(vk, vk->queue_graphics.index, 0);
    };
    info.queue_unlock_callback = [](void* opaque) {
        auto vk = static_cast<PyrowaveSurfaces*>(opaque)->m_Vk;
        vk->unlock_queue(vk, vk->queue_graphics.index, 0);
    };
    return pyrowave_create_device(&info, device) == PYROWAVE_SUCCESS;
}

PyrowaveSurfaces::~PyrowaveSurfaces()
{
    for (auto& surface : m_Surfaces) {
        av_buffer_unref(&surface.ref);
        for (auto& plane : surface.planes) {
            pl_tex_destroy(m_Vk->gpu, &plane);
        }
    }
    pl_gpu_finish(m_Vk->gpu);
    pl_vulkan_sem_destroy(m_Vk->gpu, &m_Done);
    pl_vulkan_sem_destroy(m_Vk->gpu, &m_Ready);
}

PyrowaveSurfaces::DecodeResult PyrowaveSurfaces::decode(pyrowave_decoder decoder, AVFrame* frame)
{
    Surface* target = nullptr;
    for (auto& surface : m_Surfaces) {
        if (!surface.ref || av_buffer_get_ref_count(surface.ref) == 1) {
            target = &surface;
            break;
        }
    }
    if (!target) {
        return PoolBusy;
    }
    bool hdr = frame->format == AV_PIX_FMT_YUV420P10;
    pl_gpu gpu = m_Vk->gpu;
    if (!target->ref) {
        pl_fmt format = pl_find_named_fmt(gpu, hdr ? "r16" : "r8");
        if (!format || !(format->caps & PL_FMT_CAP_STORABLE) ||
            !(format->caps & PL_FMT_CAP_SAMPLEABLE)) {
            return Unavailable;
        }
        for (int p = 0; p < 3; ++p) {
            pl_tex_params params = {};
            params.w = p ? frame->width / 2 : frame->width;
            params.h = p ? frame->height / 2 : frame->height;
            params.format = format;
            params.sampleable = params.storable = true;
            if (!pl_tex_recreate(gpu, &target->planes[p], &params)) {
                return Unavailable;
            }
        }
        target->ref = av_buffer_create(
            reinterpret_cast<uint8_t*>(target), sizeof(*target), [](void*, uint8_t*) {}, this,
            AV_BUFFER_FLAG_READONLY);
        if (!target->ref) {
            return Unavailable;
        }
    }

    pyrowave_gpu_buffers buffers = {};
    int held = 0;
    for (; held < 3; ++held) {
        auto tex = target->planes[held];
        pl_vulkan_hold_params hold = {};
        hold.tex = tex;
        hold.layout = VK_IMAGE_LAYOUT_GENERAL;
        hold.qf = m_Vk->queue_graphics.index;
        hold.semaphore = { m_Ready, m_ReadyValue + 1 };
        if (!pl_vulkan_hold_ex(gpu, &hold)) {
            break;
        }
        ++m_ReadyValue;
        auto& view = buffers.planes[held];
        view.image = pl_vulkan_unwrap(gpu, tex, &view.image_format, nullptr);
        view.view_format = view.image_format;
        view.width = tex->params.w;
        view.height = tex->params.h;
        view.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        view.layout = hold.layout;
    }
    pyrowave_gpu_sync_operation acquire = {}, release = {};
    acquire.sync = { m_Ready, m_ReadyValue };
    // The final plane's ready value uniquely identifies this decode on both timelines.
    release.sync = { m_Done, m_ReadyValue };
    bool decoded = held == 3 && pyrowave_decoder_decode_gpu_buffer(decoder, &acquire, &release,
                                                                   &buffers) == PYROWAVE_SUCCESS;
    for (int p = 0; p < held; ++p) {
        pl_vulkan_release_params back = {};
        back.tex = target->planes[p];
        back.layout = VK_IMAGE_LAYOUT_GENERAL;
        back.qf = m_Vk->queue_graphics.index;
        back.semaphore = { decoded ? m_Done : m_Ready, m_ReadyValue };
        pl_vulkan_release_ex(gpu, &back);
    }
    if (!decoded) {
        return Unavailable;
    }
    frame->buf[0] = av_buffer_ref(target->ref);
    if (frame->buf[0] && hdr) {
        // GPU decode produces normalized R16, while CPU P10 stores ten low bits.
        frame->format = AV_PIX_FMT_YUV420P16;
    }
    return frame->buf[0] ? Decoded : Unavailable;
}

bool PyrowaveSurfaces::owns(const AVFrame* frame) const
{
    return frame->buf[0] && av_buffer_get_opaque(frame->buf[0]) == this;
}

void PyrowaveSurfaces::map(const AVFrame* frame, pl_frame* mapped) const
{
    auto surface = reinterpret_cast<const Surface*>(frame->buf[0]->data);
    pl_frame_from_avframe(mapped, frame);
    mapped->repr.bits.sample_depth = mapped->repr.bits.color_depth;
    for (int p = 0; p < 3; ++p) {
        mapped->planes[p].texture = surface->planes[p];
    }
    pl_frame_set_chroma_location(mapped, pl_chroma_from_av(frame->chroma_location));
}
