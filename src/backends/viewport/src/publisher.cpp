// The viewport transport's pixel half, on the engine's side. See cy/backends/viewport/publisher.h.
//
// THE ONLY FILE IN THIS MODULE THAT NAMES VULKAN. Everything that is a decision rather than a
// device call is in wire.h, so the seqlock, the reservation and the slot rule are tested without a
// GPU and this file is only the part that needs one.
//
// --- VOLK, AND THE ONE THING THAT WOULD HAVE BEEN A VERY EXPENSIVE BUG ---------------------------
//
// `src/backends/rhi/vulkan/` also uses volk, and it calls `volkLoadInstanceOnly` and
// `volkLoadDevice`, which write volk's PROCESS-WIDE dispatch tables. This module creates a SECOND
// instance and a SECOND device in the same process. If it called either of those functions, the
// last call would win and the renderer's calls would be dispatched through this module's device —
// which works by accident on a driver whose device functions do not vary by device, and does not on
// one with a layer chain.
//
// So this file calls neither. Instance-level entry points are resolved by hand into `Instance`
// below, and device-level ones into a `VolkDeviceTable` that belongs to this device alone.
// `volkInitialize()` is idempotent and loads only the global entry points, so it is safe to call.

#include <cy/backends/viewport/publisher.h>

#include <cy/core/base/assert.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/system_allocator.h>

#include "wire.h"

#define VK_NO_PROTOTYPES
#include <volk.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <new>

// clang-tidy: Vulkan's create-info structures are ZERO-INITIALISED and then filled in, which is how
// every Vulkan program in existence is written and what the API's `pNext` chaining assumes. Several
// of their members are enumerations with no zero-valued enumerator, so `{}` is flagged —
// truthfully, and with nothing useful to do about it: designated initialisers value-initialise the
// members they omit in exactly the same way, and the alternative is a memset with a cast.
// Suppressed here, once, with the reason, rather than at eleven call sites.
//
// NOLINTBEGIN(bugprone-invalid-enum-default-initialization)

namespace cy::viewport {
namespace {

/// The image format, and the one the editor imports as `Rgba8Unorm`.
constexpr VkFormat kFormat = VK_FORMAT_R8G8B8A8_UNORM;

/// How many frames are pipelined inside this module: one command buffer, one fence and one staging
/// buffer each. UNRELATED to the ring, and conflating the two is how a "synchronisation bug" turns
/// out to be a staging buffer rewritten while the GPU was still reading it.
constexpr u32 kInFlight = 3;

constexpr VkImageUsageFlags kUsage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

constexpr VkImageSubresourceRange kColourRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

/// The instance-level entry points this module uses, resolved into a table of its own.
struct InstanceTable {
    PFN_vkDestroyInstance destroy_instance = nullptr;
    PFN_vkEnumeratePhysicalDevices enumerate_physical_devices = nullptr;
    PFN_vkGetPhysicalDeviceProperties get_properties = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties get_queue_families = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties get_memory_properties = nullptr;
    PFN_vkGetPhysicalDeviceFormatProperties2 get_format_properties2 = nullptr;
    PFN_vkGetPhysicalDeviceImageFormatProperties2 get_image_format_properties2 = nullptr;
    PFN_vkCreateDevice create_device = nullptr;
    PFN_vkGetDeviceProcAddr get_device_proc_addr = nullptr;

    [[nodiscard]] bool complete() const noexcept {
        return destroy_instance != nullptr && enumerate_physical_devices != nullptr &&
               get_properties != nullptr && get_queue_families != nullptr &&
               get_memory_properties != nullptr && get_format_properties2 != nullptr &&
               get_image_format_properties2 != nullptr && create_device != nullptr &&
               get_device_proc_addr != nullptr;
    }
};

template <class Function>
void resolve(VkInstance instance, const char* name, Function& out) noexcept {
    out = reinterpret_cast<Function>(vkGetInstanceProcAddr(instance, name));
}

/// One image of the ring: its allocation, its exported descriptor, and how the driver laid it out.
struct SharedImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    PlaneDescription plane;
    int descriptor = -1;
};

/// One frame in flight: what it is recorded into and what it is staged through.
struct InFlight {
    VkCommandBuffer commands = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;
    u8* mapped = nullptr;
};

[[nodiscard]] Error vulkan_error(VkResult result, const char* what) noexcept {
    return Error{ErrorCode::Unavailable, what, static_cast<i64>(result)};
}

}  // namespace

/// The publisher's whole implementation, hidden behind `Publisher`'s opaque pointer.
struct Publisher::Impl {
    ~Impl() { teardown(); }

    [[nodiscard]] Status open(const PublisherOptions& options) noexcept;

    void service() noexcept;
    [[nodiscard]] bool has_client() const noexcept { return client_ >= 0; }
    [[nodiscard]] FrameStaging begin_frame() noexcept;
    [[nodiscard]] Expected<u64, Error> publish() noexcept;
    [[nodiscard]] const char* adapter_name() const noexcept { return adapter_name_; }
    [[nodiscard]] u64 modifier() const noexcept { return handshake_.modifier; }
    [[nodiscard]] const PublisherStatistics& statistics() const noexcept { return statistics_; }

    [[nodiscard]] Status create_instance(const PublisherOptions& options) noexcept;
    [[nodiscard]] Status create_device(const PublisherOptions& options) noexcept;
    [[nodiscard]] Status create_images() noexcept;
    [[nodiscard]] Status create_recording() noexcept;
    [[nodiscard]] Status create_timelines() noexcept;
    [[nodiscard]] Status listen(const char* path) noexcept;
    [[nodiscard]] Status hand_over(int stream) noexcept;
    [[nodiscard]] Expected<VkImage, Error> create_exported_image(SharedImage& out) noexcept;
    [[nodiscard]] Status collect_modifiers() noexcept;
    [[nodiscard]] Expected<u32, Error> memory_type(u32 bits,
                                                   VkMemoryPropertyFlags flags) const noexcept;
    [[nodiscard]] u64 timeline_value(VkSemaphore semaphore) const noexcept;
    void record(VkCommandBuffer commands, const SharedImage& target, bool initialised) noexcept;
    void teardown() noexcept;

    // Vulkan
    VkInstance instance_ = VK_NULL_HANDLE;
    InstanceTable api_{};
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memory_{};
    VkDevice device_ = VK_NULL_HANDLE;
    VolkDeviceTable device_api_{};
    VkQueue queue_ = VK_NULL_HANDLE;
    u32 queue_family_ = 0;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkSemaphore render_done_ = VK_NULL_HANDLE;
    VkSemaphore release_ = VK_NULL_HANDLE;
    char adapter_name_[256] = {};
    u64 modifiers_[64] = {};
    u32 modifier_count_ = 0;

    // The ring and its transport
    SharedImage images_[kMaxBuffers]{};
    InFlight in_flight_[kInFlight]{};
    Ring ring_;
    AnnouncementPage page_;
    Handshake handshake_{};
    int listener_ = -1;
    int client_ = -1;
    char socket_path_[108] = {};

    u32 width_ = 0;
    u32 height_ = 0;
    u32 buffers_ = 0;
    u64 frame_id_ = 0;
    u32 reserved_slot_ = Ring::kNoSlot;
    u32 reserved_in_flight_ = 0;
    PublisherStatistics statistics_{};
};

// --- Creation -----------------------------------------------------------------------------------

Status Publisher::Impl::open(const PublisherOptions& options) noexcept {
    width_ = options.width;
    height_ = options.height;
    buffers_ = options.buffers;
    if (width_ == 0 || height_ == 0) {
        return fail(ErrorCode::InvalidArgument, "a viewport publisher needs a non-empty image");
    }
    if (buffers_ == 0 || buffers_ > kMaxBuffers) {
        return fail(ErrorCode::InvalidArgument, "a ring holds one to four images");
    }
    if (Status created = create_instance(options); !created) {
        return created;
    }
    if (Status created = create_device(options); !created) {
        return created;
    }
    if (Status collected = collect_modifiers(); !collected) {
        return collected;
    }
    if (Status created = create_images(); !created) {
        return created;
    }
    if (Status created = create_timelines(); !created) {
        return created;
    }
    if (Status created = create_recording(); !created) {
        return created;
    }
    if (Status created = page_.create(); !created) {
        return created;
    }
    if (Status sized = ring_.resize(buffers_); !sized) {
        return sized;
    }
    handshake_.width = width_;
    handshake_.height = height_;
    handshake_.buffer_count = buffers_;
    handshake_.generation = ring_.generation();
    handshake_.has_timelines = 1;
    for (u32 slot = 0; slot < buffers_; ++slot) {
        handshake_.planes[slot] = images_[slot].plane;
    }
    if (Status valid = handshake_.validate(); !valid) {
        return valid;
    }
    return listen(options.socket_path);
}

Status Publisher::Impl::create_instance(const PublisherOptions& options) noexcept {
    if (volkInitialize() != VK_SUCCESS) {
        return fail(ErrorCode::Unavailable,
                    "no Vulkan loader on this machine, so the engine cannot publish a frame");
    }

    // The validation layer is asked for and its absence is not fatal. External-memory and
    // DRM-modifier images are the corner of Vulkan where a wrong usage flag produces an image that
    // looks fine here and is refused by the editor's import, and the layer is what names it.
    const char* layers[] = {"VK_LAYER_KHRONOS_validation"};
    u32 layer_count = 0;
    if (options.validation) {
        u32 available = 0;
        (void)vkEnumerateInstanceLayerProperties(&available, nullptr);
        Array<VkLayerProperties> properties;
        if (Status sized = properties.resize(available); sized) {
            (void)vkEnumerateInstanceLayerProperties(&available, properties.data());
            for (const VkLayerProperties& layer : properties) {
                if (std::strcmp(layer.layerName, layers[0]) == 0) {
                    layer_count = 1;
                }
            }
        }
    }

    VkApplicationInfo application{};
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.pApplicationName = "cy-viewport-publisher";
    application.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo information{};
    information.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    information.pApplicationInfo = &application;
    information.enabledLayerCount = layer_count;
    information.ppEnabledLayerNames = layers;

    if (const VkResult result = vkCreateInstance(&information, nullptr, &instance_);
        result != VK_SUCCESS) {
        return Status{make_unexpected(vulkan_error(result, "vkCreateInstance"))};
    }

    resolve(instance_, "vkDestroyInstance", api_.destroy_instance);
    resolve(instance_, "vkEnumeratePhysicalDevices", api_.enumerate_physical_devices);
    resolve(instance_, "vkGetPhysicalDeviceProperties", api_.get_properties);
    resolve(instance_, "vkGetPhysicalDeviceQueueFamilyProperties", api_.get_queue_families);
    resolve(instance_, "vkGetPhysicalDeviceMemoryProperties", api_.get_memory_properties);
    resolve(instance_, "vkGetPhysicalDeviceFormatProperties2", api_.get_format_properties2);
    resolve(instance_, "vkGetPhysicalDeviceImageFormatProperties2",
            api_.get_image_format_properties2);
    resolve(instance_, "vkCreateDevice", api_.create_device);
    resolve(instance_, "vkGetDeviceProcAddr", api_.get_device_proc_addr);
    if (!api_.complete()) {
        return fail(ErrorCode::Unavailable,
                    "the Vulkan loader did not resolve every instance entry point this publisher "
                    "needs; it is older than 1.1");
    }
    return ok();
}

Status Publisher::Impl::create_device(const PublisherOptions& options) noexcept {
    u32 count = 0;
    if (const VkResult result = api_.enumerate_physical_devices(instance_, &count, nullptr);
        result != VK_SUCCESS || count == 0) {
        return fail(ErrorCode::Unavailable, "this machine reports no Vulkan physical device");
    }
    Array<VkPhysicalDevice> devices;
    if (Status sized = devices.resize(count); !sized) {
        return sized;
    }
    (void)api_.enumerate_physical_devices(instance_, &count, devices.data());

    // The adapter must be the one the editor picked: a dma-buf crossing two GPUs is an import the
    // driver refuses, and the failure reads as a protocol defect rather than as two devices.
    physical_ = devices[0];
    const bool wanted = options.adapter != nullptr && options.adapter[0] != '\0';
    for (VkPhysicalDevice candidate : devices) {
        VkPhysicalDeviceProperties properties{};
        api_.get_properties(candidate, &properties);
        if (wanted && std::strstr(properties.deviceName, options.adapter) != nullptr) {
            physical_ = candidate;
            break;
        }
    }
    VkPhysicalDeviceProperties properties{};
    api_.get_properties(physical_, &properties);
    std::snprintf(adapter_name_, sizeof(adapter_name_), "%s", properties.deviceName);
    api_.get_memory_properties(physical_, &memory_);

    u32 families = 0;
    api_.get_queue_families(physical_, &families, nullptr);
    Array<VkQueueFamilyProperties> family_properties;
    if (Status sized = family_properties.resize(families); !sized) {
        return sized;
    }
    api_.get_queue_families(physical_, &families, family_properties.data());
    bool found = false;
    for (u32 index = 0; index < families; ++index) {
        if ((family_properties[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0U) {
            queue_family_ = index;
            found = true;
            break;
        }
    }
    if (!found) {
        return fail(ErrorCode::Unavailable, "this device has no graphics queue family");
    }

    const f32 priority = 1.0F;
    VkDeviceQueueCreateInfo queue{};
    queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue.queueFamilyIndex = queue_family_;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;

    // Every one of these is load-bearing and each fails differently without it: external memory to
    // export the allocation, dma-buf to export it AS a dma-buf, the modifier extension because
    // linear is not a renderable tiling on this hardware, external semaphore to export the
    // timelines, and timeline semaphores themselves because OPAQUE_FD cannot carry a binary one.
    const char* extensions[] = {
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
        VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
    };

    VkPhysicalDeviceTimelineSemaphoreFeatures timeline{};
    timeline.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
    timeline.timelineSemaphore = VK_TRUE;

    VkDeviceCreateInfo information{};
    information.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    information.pNext = &timeline;
    information.queueCreateInfoCount = 1;
    information.pQueueCreateInfos = &queue;
    information.enabledExtensionCount = static_cast<u32>(std::size(extensions));
    information.ppEnabledExtensionNames = extensions;

    if (const VkResult result = api_.create_device(physical_, &information, nullptr, &device_);
        result != VK_SUCCESS) {
        return Status{make_unexpected(vulkan_error(
            result,
            "vkCreateDevice for the viewport publisher; this GPU cannot export a dma-buf image"))};
    }
    // A table of this device's own, NEVER volkLoadDevice — see this file's header for the process
    // wide dispatch this would otherwise clobber for the renderer.
    volkLoadDeviceTable(&device_api_, device_);
    device_api_.vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
    return ok();
}

Status Publisher::Impl::collect_modifiers() noexcept {
    VkDrmFormatModifierPropertiesListEXT list{};
    list.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT;
    VkFormatProperties2 format{};
    format.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
    format.pNext = &list;
    api_.get_format_properties2(physical_, kFormat, &format);

    Array<VkDrmFormatModifierPropertiesEXT> candidates;
    if (Status sized = candidates.resize(list.drmFormatModifierCount); !sized) {
        return sized;
    }
    if (!candidates.empty()) {
        list.pDrmFormatModifierProperties = candidates.data();
        api_.get_format_properties2(physical_, kFormat, &format);
    }

    constexpr VkFormatFeatureFlags kNeeded =
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
        VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;

    for (const VkDrmFormatModifierPropertiesEXT& candidate : candidates) {
        if (modifier_count_ >= std::size(modifiers_)) {
            break;
        }
        // One plane only. A multi-planar layout would need a descriptor and a stride per plane, and
        // the editor's handshake carries one of each — matching it is the whole design.
        if (candidate.drmFormatModifierPlaneCount != 1) {
            continue;
        }
        if ((candidate.drmFormatModifierTilingFeatures & kNeeded) != kNeeded) {
            continue;
        }
        VkPhysicalDeviceImageDrmFormatModifierInfoEXT drm{};
        drm.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT;
        drm.drmFormatModifier = candidate.drmFormatModifier;
        drm.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkPhysicalDeviceExternalImageFormatInfo external{};
        external.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
        external.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        external.pNext = &drm;
        VkPhysicalDeviceImageFormatInfo2 information{};
        information.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
        information.format = kFormat;
        information.type = VK_IMAGE_TYPE_2D;
        information.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
        information.usage = kUsage;
        information.pNext = &external;

        VkExternalImageFormatProperties external_properties{};
        external_properties.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;
        VkImageFormatProperties2 properties{};
        properties.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
        properties.pNext = &external_properties;
        if (api_.get_image_format_properties2(physical_, &information, &properties) != VK_SUCCESS) {
            continue;
        }
        if ((external_properties.externalMemoryProperties.externalMemoryFeatures &
             VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) == 0U) {
            continue;
        }
        modifiers_[modifier_count_++] = candidate.drmFormatModifier;
    }
    if (modifier_count_ == 0) {
        return fail(ErrorCode::Unavailable,
                    "no single-plane DRM modifier on this device can be both rendered to and "
                    "exported, so the editor cannot import the engine's frames");
    }
    return ok();
}

Expected<u32, Error> Publisher::Impl::memory_type(u32 bits,
                                                  VkMemoryPropertyFlags flags) const noexcept {
    for (u32 index = 0; index < memory_.memoryTypeCount; ++index) {
        if ((bits & (1U << index)) != 0U &&
            (memory_.memoryTypes[index].propertyFlags & flags) == flags) {
            return index;
        }
    }
    return make_unexpected(
        Error{ErrorCode::Unavailable, "no memory type with the properties this allocation needs"});
}

Expected<VkImage, Error> Publisher::Impl::create_exported_image(SharedImage& out) noexcept {
    VkImageDrmFormatModifierListCreateInfoEXT list{};
    list.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT;
    list.drmFormatModifierCount = modifier_count_;
    list.pDrmFormatModifiers = modifiers_;

    VkExternalMemoryImageCreateInfo external{};
    external.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    external.pNext = &list;

    VkImageCreateInfo information{};
    information.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    information.pNext = &external;
    information.imageType = VK_IMAGE_TYPE_2D;
    information.format = kFormat;
    information.extent = VkExtent3D{width_, height_, 1};
    information.mipLevels = 1;
    information.arrayLayers = 1;
    information.samples = VK_SAMPLE_COUNT_1_BIT;
    information.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    information.usage = kUsage;
    information.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    information.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (const VkResult result =
            device_api_.vkCreateImage(device_, &information, nullptr, &out.image);
        result != VK_SUCCESS) {
        return make_unexpected(vulkan_error(result, "creating an exportable image"));
    }

    VkMemoryRequirements requirements{};
    device_api_.vkGetImageMemoryRequirements(device_, out.image, &requirements);
    const Expected<u32, Error> index =
        memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!index) {
        return make_unexpected(index.error());
    }

    // The allocation is DEDICATED and EXPORTABLE, in that order of pNext. A dma-buf export of a
    // suballocation is not a thing the extension offers, which is one reason this module does not
    // go through the RHI's VMA-backed allocator.
    VkMemoryDedicatedAllocateInfo dedicated{};
    dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicated.image = out.image;
    VkExportMemoryAllocateInfo export_info{};
    export_info.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    export_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    export_info.pNext = &dedicated;

    VkMemoryAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.pNext = &export_info;
    // THE DRIVER'S SIZE, not `width * height * 4`. The modifier's tiling and alignment make them
    // different numbers, and the editor imports against this one.
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = *index;
    if (const VkResult result =
            device_api_.vkAllocateMemory(device_, &allocation, nullptr, &out.memory);
        result != VK_SUCCESS) {
        return make_unexpected(vulkan_error(result, "allocating exportable device memory"));
    }
    if (const VkResult result = device_api_.vkBindImageMemory(device_, out.image, out.memory, 0);
        result != VK_SUCCESS) {
        return make_unexpected(vulkan_error(result, "binding an exportable image"));
    }

    VkImageSubresource subresource{};
    subresource.aspectMask = VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT;
    VkSubresourceLayout layout{};
    device_api_.vkGetImageSubresourceLayout(device_, out.image, &subresource, &layout);

    VkMemoryGetFdInfoKHR get{};
    get.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    get.memory = out.memory;
    get.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    if (const VkResult result = device_api_.vkGetMemoryFdKHR(device_, &get, &out.descriptor);
        result != VK_SUCCESS) {
        return make_unexpected(vulkan_error(result, "exporting a dma-buf descriptor"));
    }

    out.plane.stride = layout.rowPitch;
    out.plane.offset = layout.offset;
    out.plane.allocation_bytes = requirements.size;
    return out.image;
}

Status Publisher::Impl::create_images() noexcept {
    for (u32 slot = 0; slot < buffers_; ++slot) {
        if (Expected<VkImage, Error> created = create_exported_image(images_[slot]); !created) {
            return Status{make_unexpected(created.error())};
        }
    }
    VkImageDrmFormatModifierPropertiesEXT chosen{};
    chosen.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT;
    if (device_api_.vkGetImageDrmFormatModifierPropertiesEXT(device_, images_[0].image, &chosen) ==
        VK_SUCCESS) {
        handshake_.modifier = chosen.drmFormatModifier;
    }
    return ok();
}

Status Publisher::Impl::create_timelines() noexcept {
    for (VkSemaphore* semaphore : {&render_done_, &release_}) {
        VkSemaphoreTypeCreateInfo kind{};
        kind.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
        kind.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        kind.initialValue = 0;
        // OPAQUE_FD, not SYNC_FD: SYNC_FD is binary-only and cannot carry a timeline. Asking for it
        // fails at creation, which is at least honest; assuming it works is not.
        VkExportSemaphoreCreateInfo export_info{};
        export_info.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
        export_info.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
        export_info.pNext = &kind;
        VkSemaphoreCreateInfo information{};
        information.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        information.pNext = &export_info;
        if (const VkResult result =
                device_api_.vkCreateSemaphore(device_, &information, nullptr, semaphore);
            result != VK_SUCCESS) {
            return Status{
                make_unexpected(vulkan_error(result, "creating an exportable timeline semaphore"))};
        }
    }
    return ok();
}

Status Publisher::Impl::create_recording() noexcept {
    VkCommandPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool.queueFamilyIndex = queue_family_;
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    if (const VkResult result = device_api_.vkCreateCommandPool(device_, &pool, nullptr, &pool_);
        result != VK_SUCCESS) {
        return Status{make_unexpected(vulkan_error(result, "creating a command pool"))};
    }

    const u64 bytes = static_cast<u64>(width_) * static_cast<u64>(height_) * 4ULL;
    for (InFlight& frame : in_flight_) {
        VkCommandBufferAllocateInfo allocate{};
        allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocate.commandPool = pool_;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;
        if (const VkResult result =
                device_api_.vkAllocateCommandBuffers(device_, &allocate, &frame.commands);
            result != VK_SUCCESS) {
            return Status{make_unexpected(vulkan_error(result, "allocating a command buffer"))};
        }

        VkFenceCreateInfo fence{};
        fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        // Signalled, so the first wait returns immediately rather than for ever.
        fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (const VkResult result =
                device_api_.vkCreateFence(device_, &fence, nullptr, &frame.fence);
            result != VK_SUCCESS) {
            return Status{make_unexpected(vulkan_error(result, "creating a fence"))};
        }

        VkBufferCreateInfo buffer{};
        buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer.size = bytes;
        buffer.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (const VkResult result =
                device_api_.vkCreateBuffer(device_, &buffer, nullptr, &frame.staging);
            result != VK_SUCCESS) {
            return Status{make_unexpected(vulkan_error(result, "creating a staging buffer"))};
        }
        VkMemoryRequirements requirements{};
        device_api_.vkGetBufferMemoryRequirements(device_, frame.staging, &requirements);
        const Expected<u32, Error> index =
            memory_type(requirements.memoryTypeBits,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (!index) {
            return Status{make_unexpected(index.error())};
        }
        VkMemoryAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = *index;
        if (const VkResult result =
                device_api_.vkAllocateMemory(device_, &allocation, nullptr, &frame.staging_memory);
            result != VK_SUCCESS) {
            return Status{make_unexpected(vulkan_error(result, "allocating host-visible memory"))};
        }
        if (const VkResult result =
                device_api_.vkBindBufferMemory(device_, frame.staging, frame.staging_memory, 0);
            result != VK_SUCCESS) {
            return Status{make_unexpected(vulkan_error(result, "binding a staging buffer"))};
        }
        // Mapped once and left mapped. A map and unmap per frame is a driver call per frame for
        // nothing: the memory is host-coherent, so a write is visible to the device without a
        // flush, and the fence is what orders it against the copy.
        void* mapped = nullptr;
        if (const VkResult result = device_api_.vkMapMemory(device_, frame.staging_memory, 0,
                                                            requirements.size, 0, &mapped);
            result != VK_SUCCESS) {
            return Status{make_unexpected(vulkan_error(result, "mapping a staging buffer"))};
        }
        frame.mapped = static_cast<u8*>(mapped);
    }
    return ok();
}

Status Publisher::Impl::listen(const char* path) noexcept {
    if (path == nullptr || path[0] == '\0') {
        return fail(ErrorCode::InvalidArgument, "the publisher needs a socket path");
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    // THE 108-BYTE CAP, REFUSED HERE RATHER THAN TRUNCATED. M7 task 5b.6: a socket path derived
    // from a build tree can be longer than `sun_path`, and a silently truncated one binds a
    // DIFFERENT socket that the editor will never find — which is worse than the refusal, because
    // everything appears to start and the viewport merely stays empty.
    const usize length = std::strlen(path);
    if (length >= sizeof(address.sun_path)) {
        return fail(ErrorCode::InvalidArgument,
                    "the viewport socket path is longer than a Unix socket allows (107 bytes)");
    }
    std::memcpy(address.sun_path, path, length + 1);
    std::memcpy(socket_path_, path, length + 1);

    listener_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (listener_ < 0) {
        return fail(ErrorCode::Unavailable, "creating the viewport socket", errno);
    }
    (void)::unlink(path);
    if (::bind(listener_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        return fail(ErrorCode::Unavailable, "binding the viewport socket", errno);
    }
    if (::listen(listener_, 4) != 0) {
        return fail(ErrorCode::Unavailable, "listening on the viewport socket", errno);
    }
    return ok();
}

Status Publisher::Impl::hand_over(int stream) noexcept {
    VkSemaphoreGetFdInfoKHR get{};
    get.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR;
    get.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;

    int exported[2] = {-1, -1};
    const VkSemaphore semaphores[2] = {render_done_, release_};
    for (u32 index = 0; index < 2; ++index) {
        get.semaphore = semaphores[index];
        if (const VkResult result =
                device_api_.vkGetSemaphoreFdKHR(device_, &get, &exported[index]);
            result != VK_SUCCESS) {
            for (const int open : exported) {
                if (open >= 0) {
                    (void)::close(open);
                }
            }
            return Status{make_unexpected(vulkan_error(result, "exporting a timeline semaphore"))};
        }
    }

    // THE ORDER IS THE ORDER THE EDITOR POPS THEM IN: images in slot order, then the two timelines,
    // then the announcement page. A different order shows the wrong slot for every frame.
    int descriptors[kMaxBuffers + 3] = {};
    u32 count = 0;
    for (u32 slot = 0; slot < buffers_; ++slot) {
        descriptors[count++] = images_[slot].descriptor;
    }
    descriptors[count++] = exported[0];
    descriptors[count++] = exported[1];
    descriptors[count++] = page_.descriptor();

    const Status sent =
        send_descriptors(stream, descriptors, count, &handshake_, sizeof(handshake_));
    // The exported semaphore descriptors are ours to close: `SCM_RIGHTS` duplicates them into the
    // editor, and leaving them open here would leak two per connection.
    for (const int open : exported) {
        (void)::close(open);
    }
    return sent;
}

// --- The frame ----------------------------------------------------------------------------------

void Publisher::Impl::service() noexcept {
    if (client_ < 0) {
        const int stream = ::accept4(listener_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (stream < 0) {
            return;
        }
        if (Status handed = hand_over(stream); !handed) {
            (void)::close(stream);
            return;
        }
        client_ = stream;
        statistics_.connections += 1;
        return;
    }
    if (peer_closed(client_)) {
        // A disconnected editor gets the ring back immediately. Without this a three-image ring is
        // exhausted within three frames by claims nobody will ever release.
        (void)::close(client_);
        client_ = -1;
        ring_.reclaim();
        page_.set_writing(false, 0);
    }
}

u64 Publisher::Impl::timeline_value(VkSemaphore semaphore) const noexcept {
    u64 value = 0;
    if (device_api_.vkGetSemaphoreCounterValue(device_, semaphore, &value) != VK_SUCCESS) {
        return 0;
    }
    return value;
}

FrameStaging Publisher::Impl::begin_frame() noexcept {
    CY_ASSERT_MSG(reserved_slot_ == Ring::kNoSlot, "begin_frame() twice without publish()");

    ring_.observe_held(page_.held());
    const u32 slot = ring_.pick(timeline_value(release_));
    if (slot == Ring::kNoSlot) {
        // THE EDITOR MUST NEVER THROTTLE THE ENGINE. A full ring is this frame's problem: drop it,
        // count it, and carry on at full rate.
        statistics_.dropped_full_ring += 1;
        return FrameStaging{};
    }
    page_.set_writing(true, slot);
    if (claim_names(page_.held(), slot)) {
        // The second look, and it is not redundant with `observe_held`: the editor is entitled to
        // claim a slot between the choice and the declaration, and a claim confirmed inside that
        // window is a claim on an image already being overwritten. The editor's own spike measured
        // 2.7% of frames on a three-image ring with the runtime free-running.
        page_.set_writing(false, 0);
        statistics_.vetoed += 1;
        return FrameStaging{};
    }

    reserved_slot_ = slot;
    reserved_in_flight_ = static_cast<u32>((frame_id_ + 1) % kInFlight);
    InFlight& frame = in_flight_[reserved_in_flight_];
    // The fence guards this staging buffer and this command buffer, not the ring's image.
    (void)device_api_.vkWaitForFences(device_, 1, &frame.fence, VK_TRUE, UINT64_MAX);
    (void)device_api_.vkResetFences(device_, 1, &frame.fence);

    return FrameStaging{frame.mapped, width_, height_, width_ * 4U, slot};
}

void Publisher::Impl::record(VkCommandBuffer commands, const SharedImage& target,
                             bool initialised) noexcept {
    // The editor is handed every image in SHADER_READ_ONLY_OPTIMAL, so that is what a written image
    // is transitioned back to and what an already-written one is transitioned from. An image
    // nothing has been drawn into yet has no contents to preserve, and UNDEFINED says so.
    const VkImageLayout from =
        initialised ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    (void)device_api_.vkBeginCommandBuffer(commands, &begin);

    auto barrier = [&](VkImageLayout old_layout, VkImageLayout new_layout) {
        VkImageMemoryBarrier transition{};
        transition.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        transition.oldLayout = old_layout;
        transition.newLayout = new_layout;
        transition.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        transition.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        transition.image = target.image;
        transition.subresourceRange = kColourRange;
        transition.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
        transition.dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
        device_api_.vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                                         nullptr, 1, &transition);
    };

    barrier(from, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy region{};
    region.bufferRowLength = width_;
    region.bufferImageHeight = height_;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = VkExtent3D{width_, height_, 1};
    device_api_.vkCmdCopyBufferToImage(commands, in_flight_[reserved_in_flight_].staging,
                                       target.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                       &region);
    barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    (void)device_api_.vkEndCommandBuffer(commands);
}

Expected<u64, Error> Publisher::Impl::publish() noexcept {
    if (reserved_slot_ == Ring::kNoSlot) {
        return make_unexpected(Error{ErrorCode::InvalidArgument,
                                     "publish() without a slot; begin_frame() dropped this frame"});
    }
    const u64 started = monotonic_nanos();
    const u32 slot = reserved_slot_;
    reserved_slot_ = Ring::kNoSlot;
    InFlight& frame = in_flight_[reserved_in_flight_];

    frame_id_ += 1;
    record(frame.commands, images_[slot], ring_.slot(slot).initialised);

    // Wait for the editor to have finished with whatever this slot held, then signal that this
    // frame is complete. Without the wait the engine overwrites a frame the editor is still
    // sampling; without the signal the editor has nothing to wait on and shows torn images.
    const u64 needs_release = ring_.release_requirement(slot);
    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    const u64 wait_value = needs_release;
    const u64 signal_value = frame_id_;

    VkTimelineSemaphoreSubmitInfo timeline{};
    timeline.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    timeline.waitSemaphoreValueCount = (needs_release > 0) ? 1U : 0U;
    timeline.pWaitSemaphoreValues = &wait_value;
    timeline.signalSemaphoreValueCount = 1;
    timeline.pSignalSemaphoreValues = &signal_value;

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.pNext = &timeline;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &frame.commands;
    submit.waitSemaphoreCount = (needs_release > 0) ? 1U : 0U;
    submit.pWaitSemaphores = &release_;
    submit.pWaitDstStageMask = &wait_stage;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &render_done_;

    const u64 submitted_nanos = monotonic_nanos();
    if (const VkResult result = device_api_.vkQueueSubmit(queue_, 1, &submit, frame.fence);
        result != VK_SUCCESS) {
        return make_unexpected(vulkan_error(result, "submitting the viewport frame"));
    }
    ring_.record_published(slot, frame_id_);

    // ANNOUNCE AFTER `vkQueueSubmit`, NEVER BEFORE.
    //
    // By the time the editor can see this frame identity, the work that signals `render_done` to
    // that value is already on a queue, so the driver signals it even if this process is destroyed
    // in the next instant. Announce first and a kill in between leaves the editor waiting on a
    // value nothing will ever reach — with the bounded host wait, a viewport that never updates
    // again. Six SIGKILL runs of the editor's own spike survived on exactly this ordering.
    Announcement announcement;
    announcement.frame_id = frame_id_;
    announcement.slot = slot;
    announcement.generation = ring_.generation();
    announcement.timeline_value = frame_id_;
    announcement.submitted_nanos = submitted_nanos;
    page_.publish(announcement);
    // Cleared AFTER the announcement rather than before: the slot is no longer reserved against the
    // editor, it is now the frame the editor most wants to claim, and clearing earlier would make
    // the newest frame unclaimable.
    page_.set_writing(false, 0);

    statistics_.published += 1;
    statistics_.upload_micros += (monotonic_nanos() - started) / 1000ULL;
    return frame_id_;
}

void Publisher::Impl::teardown() noexcept {
    if (device_ != VK_NULL_HANDLE) {
        (void)device_api_.vkDeviceWaitIdle(device_);
    }
    if (client_ >= 0) {
        (void)::close(client_);
        client_ = -1;
    }
    if (listener_ >= 0) {
        (void)::close(listener_);
        listener_ = -1;
        if (socket_path_[0] != '\0') {
            (void)::unlink(socket_path_);
        }
    }
    if (device_ != VK_NULL_HANDLE) {
        for (InFlight& frame : in_flight_) {
            if (frame.staging_memory != VK_NULL_HANDLE) {
                device_api_.vkUnmapMemory(device_, frame.staging_memory);
                device_api_.vkFreeMemory(device_, frame.staging_memory, nullptr);
            }
            if (frame.staging != VK_NULL_HANDLE) {
                device_api_.vkDestroyBuffer(device_, frame.staging, nullptr);
            }
            if (frame.fence != VK_NULL_HANDLE) {
                device_api_.vkDestroyFence(device_, frame.fence, nullptr);
            }
        }
        if (pool_ != VK_NULL_HANDLE) {
            device_api_.vkDestroyCommandPool(device_, pool_, nullptr);
        }
        for (VkSemaphore semaphore : {render_done_, release_}) {
            if (semaphore != VK_NULL_HANDLE) {
                device_api_.vkDestroySemaphore(device_, semaphore, nullptr);
            }
        }
        for (SharedImage& image : images_) {
            if (image.descriptor >= 0) {
                (void)::close(image.descriptor);
                image.descriptor = -1;
            }
            if (image.image != VK_NULL_HANDLE) {
                device_api_.vkDestroyImage(device_, image.image, nullptr);
                image.image = VK_NULL_HANDLE;
            }
            if (image.memory != VK_NULL_HANDLE) {
                device_api_.vkFreeMemory(device_, image.memory, nullptr);
                image.memory = VK_NULL_HANDLE;
            }
        }
        device_api_.vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    if (instance_ != VK_NULL_HANDLE && api_.destroy_instance != nullptr) {
        api_.destroy_instance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }
}

const char* ring_advisory(u32 buffers) noexcept {
    switch (buffers) {
        case 1:
            return "a one-image ring wedges: the engine and the editor want the same image at the "
                   "same time";
        case 2:
            return "a two-image ring throttles the engine to the editor's refresh rate and costs a "
                   "whole editor frame of latency";
        default:
            return nullptr;
    }
}

Publisher::~Publisher() {
    if (impl_ != nullptr) {
        Allocator& allocator = system_allocator(MemoryDomain::Renderer);
        impl_->~Impl();
        allocator.deallocate(impl_, sizeof(Impl), alignof(Impl));
        impl_ = nullptr;
    }
}

void Publisher::service() noexcept {
    impl_->service();
}
bool Publisher::has_client() const noexcept {
    return impl_->has_client();
}
FrameStaging Publisher::begin_frame() noexcept {
    return impl_->begin_frame();
}
Expected<u64, Error> Publisher::publish() noexcept {
    return impl_->publish();
}
const char* Publisher::adapter_name() const noexcept {
    return impl_->adapter_name();
}
u64 Publisher::modifier() const noexcept {
    return impl_->modifier();
}
const PublisherStatistics& Publisher::statistics() const noexcept {
    return impl_->statistics();
}

Expected<UniquePtr<Publisher>, Error> Publisher::create(const PublisherOptions& options) noexcept {
    Allocator& allocator = system_allocator(MemoryDomain::Renderer);
    void* storage = allocator.allocate(sizeof(Impl), alignof(Impl));
    if (storage == nullptr) {
        return fail(ErrorCode::OutOfMemory, "the viewport publisher's implementation");
    }
    Impl* impl = new (storage) Impl{};
    if (Status opened = impl->open(options); !opened) {
        impl->~Impl();
        allocator.deallocate(storage, sizeof(Impl), alignof(Impl));
        return make_unexpected(opened.error());
    }
    // `Publisher` is the size of one pointer, so the size the allocator is told on release is the
    // size it was told on reservation — which an abstract base with a derived implementation would
    // not have been, and the accounting in `SystemAllocator` is by size.
    Expected<UniquePtr<Publisher>, Error> owner = make_unique<Publisher>(allocator, impl);
    if (!owner) {
        impl->~Impl();
        allocator.deallocate(storage, sizeof(Impl), alignof(Impl));
    }
    return owner;
}

}  // namespace cy::viewport

// NOLINTEND(bugprone-invalid-enum-default-initialization)
