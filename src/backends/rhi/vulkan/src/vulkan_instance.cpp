// Bringing a device up: instance, validation, physical device, queues, allocator, frames, the
// bindless table and the breadcrumb buffer. Tasks 2.3.1 and 2.3.3.
//
// EVERY DECISION HERE IS A CAPABILITY QUESTION, NOT A DEVICE-MODEL ONE. Which queues exist, whether
// bindless is usable, whether the driver reports a memory budget, whether debug markers are
// available — each is asked and recorded, and the renderer above reads the answer.
// `rhi-and-render-graph`: "The renderer SHALL branch on capabilities, never on backend identity."

#include <cy/backends/rhi/backend.h>
#include <cy/core/memory/pressure.h>

#include <cy/backends/rhi/vulkan/vulkan_backend.h>

#include "vulkan_device.h"

#include <cstdio>
#include <cstring>
#include <new>

namespace cy::rhi::vulkan {
namespace {

/// The frames-in-flight the description asked for, bounded by what a fixed-size per-frame array can
/// hold. Zero means "the default" rather than "no frames".
u32 clamp_frames_in_flight(u32 requested) noexcept {
    if (requested == 0) {
        return kDefaultFramesInFlight;
    }
    return requested > kMaxFramesInFlight ? kMaxFramesInFlight : requested;
}

bool has_extension(const VkExtensionProperties* properties, u32 count, const char* name) noexcept {
    for (u32 index = 0; index < count; ++index) {
        if (std::strcmp(properties[index].extensionName, name) == 0) {
            return true;
        }
    }
    return false;
}

bool has_layer(const VkLayerProperties* properties, u32 count, const char* name) noexcept {
    for (u32 index = 0; index < count; ++index) {
        if (std::strcmp(properties[index].layerName, name) == 0) {
            return true;
        }
    }
    return false;
}

VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                              VkDebugUtilsMessageTypeFlagsEXT /*types*/,
                                              const VkDebugUtilsMessengerCallbackDataEXT* data,
                                              void* user) {
    auto* device = static_cast<VulkanDevice*>(user);
    if (device == nullptr || data == nullptr) {
        return VK_FALSE;
    }
    ValidationSeverity mapped = ValidationSeverity::Info;
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
        mapped = ValidationSeverity::Error;
    } else if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0) {
        mapped = ValidationSeverity::Warning;
    }
    device->report_validation(mapped, data->pMessage != nullptr ? data->pMessage : "");
    // Always VK_FALSE: returning VK_TRUE aborts the offending call, which turns a diagnostic into a
    // second, different failure.
    return VK_FALSE;
}

/// True when volk could find a loader. Answered before a device is created so that selection can
/// skip this backend rather than reporting its creation failure as the reason nothing rendered.
bool vulkan_available() noexcept {
    static const bool available = volkInitialize() == VK_SUCCESS;
    return available;
}

}  // namespace

void StoredName::assign(const char* source) noexcept {
    if (source == nullptr) {
        text[0] = '\0';
        return;
    }
    usize index = 0;
    while (index + 1 < sizeof(text) && source[index] != '\0') {
        text[index] = source[index];
        ++index;
    }
    text[index] = '\0';
}

VulkanDevice::VulkanDevice(Allocator& allocator, const DeviceDescription& desc) noexcept
    : allocator_(&allocator),
      buffers_(MemoryDomain::Gpu, "rhi.vulkan.buffers"),
      textures_(MemoryDomain::Gpu, "rhi.vulkan.textures"),
      views_(MemoryDomain::Gpu, "rhi.vulkan.views"),
      samplers_(MemoryDomain::Gpu, "rhi.vulkan.samplers"),
      shaders_(MemoryDomain::Gpu, "rhi.vulkan.shaders"),
      set_layouts_(MemoryDomain::Gpu, "rhi.vulkan.set-layouts"),
      pipeline_layouts_(MemoryDomain::Gpu, "rhi.vulkan.pipeline-layouts"),
      descriptor_sets_(MemoryDomain::Gpu, "rhi.vulkan.descriptor-sets"),
      graphics_pipelines_(MemoryDomain::Gpu, "rhi.vulkan.graphics-pipelines"),
      compute_pipelines_(MemoryDomain::Gpu, "rhi.vulkan.compute-pipelines"),
      query_pools_(MemoryDomain::Gpu, "rhi.vulkan.queries"),
      fences_(MemoryDomain::Gpu, "rhi.vulkan.fences"),
      semaphores_(MemoryDomain::Gpu, "rhi.vulkan.semaphores"),
      command_buffers_(MemoryDomain::Gpu, "rhi.vulkan.command-buffers"),
      swapchains_(MemoryDomain::Gpu, "rhi.vulkan.swapchains"),
      live_command_buffers_(allocator),
      live_transient_textures_(allocator),
      live_transient_buffers_(allocator),
      retirements_(allocator),
      bindless_free_(allocator),
      barriers_(this),
      break_on_validation_error_(desc.break_on_validation_error) {
    frames_in_flight_ = clamp_frames_in_flight(desc.frames_in_flight);
}

Status VulkanDevice::initialise(const DeviceDescription& desc) noexcept {
    if (!vulkan_available()) {
        return fail(ErrorCode::Unavailable,
                    "no Vulkan loader on this machine; volkInitialize() failed");
    }
    if (Status status = create_instance(desc); !status) {
        return status;
    }
    if (Status status = select_physical_device(); !status) {
        return status;
    }
    if (Status status = create_logical_device(desc); !status) {
        return status;
    }
    if (Status status = create_allocator(); !status) {
        return status;
    }
    if (Status status = create_frames(desc); !status) {
        return status;
    }
    if (Status status = create_bindless_table(); !status) {
        return status;
    }
    return create_breadcrumbs();
}

// --- Instance
// --------------------------------------------------------------------------------------

Status VulkanDevice::create_instance(const DeviceDescription& desc) noexcept {
    VkApplicationInfo application{};
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.pApplicationName = desc.application_name;
    application.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    application.pEngineName = "CyberdyneEngine";
    application.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    // 1.3 and not more: everything M3 needs is core 1.2 or 1.3, and this project's development
    // machine has a 1.3 loader in front of a 1.4 driver. (Spike gotcha 6g.)
    application.apiVersion = kInstanceApiVersion;

    u32 extension_count = 0;
    (void)vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, nullptr);
    Array<VkExtensionProperties> extensions(*allocator_);
    if (Status sized = extensions.resize(extension_count); !sized) {
        return sized;
    }
    (void)vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, extensions.data());

    u32 layer_count = 0;
    (void)vkEnumerateInstanceLayerProperties(&layer_count, nullptr);
    Array<VkLayerProperties> layers(*allocator_);
    if (Status sized = layers.resize(layer_count); !sized) {
        return sized;
    }
    (void)vkEnumerateInstanceLayerProperties(&layer_count, layers.data());

    Array<const char*> wanted_extensions(*allocator_);
    Array<const char*> wanted_layers(*allocator_);

    // The surface extensions. The RHI never calls a window system — DisplayServer produces the
    // VkSurfaceKHR — but the INSTANCE has to have been created with the extensions that make one
    // possible, and it is created before any window exists. So they are requested where present.
    for (const char* name :
         {VK_KHR_SURFACE_EXTENSION_NAME, "VK_KHR_xlib_surface", "VK_KHR_xcb_surface",
          "VK_KHR_wayland_surface", "VK_KHR_win32_surface", "VK_EXT_metal_surface"}) {
        if (has_extension(extensions.data(), extension_count, name)) {
            if (Status pushed = wanted_extensions.push_back(name); !pushed) {
                return pushed;
            }
        }
    }

    const bool debug_utils =
        has_extension(extensions.data(), extension_count, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (desc.enable_validation && debug_utils) {
        if (Status pushed = wanted_extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            !pushed) {
            return pushed;
        }
    }
    debug_markers_ = debug_utils;

    const bool have_validation_layer =
        has_layer(layers.data(), layer_count, "VK_LAYER_KHRONOS_validation");
    if (desc.enable_validation && have_validation_layer) {
        if (Status pushed = wanted_layers.push_back("VK_LAYER_KHRONOS_validation"); !pushed) {
            return pushed;
        }
    }

    VkInstanceCreateInfo create{};
    create.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create.pApplicationInfo = &application;
    create.enabledExtensionCount = static_cast<u32>(wanted_extensions.size());
    create.ppEnabledExtensionNames = wanted_extensions.data();
    create.enabledLayerCount = static_cast<u32>(wanted_layers.size());
    create.ppEnabledLayerNames = wanted_layers.data();

    // SYNCHRONISATION VALIDATION IS OFF BY DEFAULT EVEN WITH THE LAYERS ON. Without this chained
    // structure none of the hazard checks fire, and a graph that emits no barriers at all would
    // pass. M3's spike found that out the hard way; it is the difference between a validation run
    // that means something and one that is theatre. (Gotcha 6h.)
    const VkValidationFeatureEnableEXT enabled_features[] = {
        VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT};
    VkValidationFeaturesEXT features{};
    features.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT;
    features.enabledValidationFeatureCount = 1;
    features.pEnabledValidationFeatures = enabled_features;
    if (desc.enable_validation && desc.enable_synchronisation_validation && have_validation_layer) {
        create.pNext = &features;
    }

    CY_VK_TRY(vkCreateInstance(&create, nullptr, &instance_), "vkCreateInstance");
    volkLoadInstanceOnly(instance_);

    if (desc.enable_validation && debug_utils) {
        VkDebugUtilsMessengerCreateInfoEXT messenger{};
        messenger.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        messenger.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                    VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        messenger.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        messenger.pfnUserCallback = &debug_callback;
        messenger.pUserData = this;
        CY_VK_TRY(vkCreateDebugUtilsMessengerEXT(instance_, &messenger, nullptr, &messenger_),
                  "vkCreateDebugUtilsMessengerEXT");
    }
    return ok();
}

// --- Physical device and queues
// ------------------------------------------------------------------------

Status VulkanDevice::select_physical_device() noexcept {
    u32 count = 0;
    CY_VK_TRY(vkEnumeratePhysicalDevices(instance_, &count, nullptr), "vkEnumeratePhysicalDevices");
    if (count == 0) {
        return fail(ErrorCode::Unavailable, "the Vulkan loader reports no physical devices");
    }
    Array<VkPhysicalDevice> devices(*allocator_);
    if (Status sized = devices.resize(count); !sized) {
        return sized;
    }
    CY_VK_TRY(vkEnumeratePhysicalDevices(instance_, &count, devices.data()),
              "vkEnumeratePhysicalDevices");

    // Prefer a discrete GPU, then anything. Deliberately simple: the selection a game ships with is
    // a user-facing setting, and guessing harder here would only make the default less predictable.
    VkPhysicalDevice chosen = devices[0];
    // Not `const VkPhysicalDevice`: the Vulkan handle is itself a pointer typedef, so the const
    // would qualify the pointer rather than what it points at.
    for (VkPhysicalDevice candidate : devices) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(candidate, &properties);
        if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            chosen = candidate;
            break;
        }
    }
    physical_ = chosen;
    return ok();
}

Status VulkanDevice::create_logical_device(const DeviceDescription& desc) noexcept {
    u32 family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_, &family_count, nullptr);
    Array<VkQueueFamilyProperties> families(*allocator_);
    if (Status sized = families.resize(family_count); !sized) {
        return sized;
    }
    vkGetPhysicalDeviceQueueFamilyProperties(physical_, &family_count, families.data());

    // BY CAPABILITY, NOT BY INDEX. Graphics is the first family with GRAPHICS. Async compute is
    // "has COMPUTE and NOT GRAPHICS" — the real one, not the graphics family wearing a second hat.
    // Transfer is "has TRANSFER and neither GRAPHICS nor COMPUTE". A device with no such family
    // reports the capability false, and the render graph folds those passes onto graphics, which is
    // the same code path as async compute being switched off.
    constexpr u32 kNone = ~0U;
    u32 graphics = kNone;
    u32 compute = kNone;
    u32 transfer = kNone;
    for (u32 index = 0; index < family_count; ++index) {
        const VkQueueFlags flags = families[index].queueFlags;
        const bool has_graphics = (flags & VK_QUEUE_GRAPHICS_BIT) != 0;
        const bool has_compute = (flags & VK_QUEUE_COMPUTE_BIT) != 0;
        const bool has_transfer = (flags & VK_QUEUE_TRANSFER_BIT) != 0;
        if (graphics == kNone && has_graphics) {
            graphics = index;
        }
        if (compute == kNone && has_compute && !has_graphics) {
            compute = index;
        }
        if (transfer == kNone && has_transfer && !has_graphics && !has_compute) {
            transfer = index;
        }
    }
    if (graphics == kNone) {
        return fail(ErrorCode::Unsupported, "no queue family on this device supports graphics");
    }

    queue_families_[static_cast<u32>(QueueKind::Graphics)] = graphics;
    queue_present_[static_cast<u32>(QueueKind::Graphics)] = true;
    if (desc.request_async_compute && compute != kNone) {
        queue_families_[static_cast<u32>(QueueKind::AsyncCompute)] = compute;
        queue_present_[static_cast<u32>(QueueKind::AsyncCompute)] = true;
    } else {
        queue_families_[static_cast<u32>(QueueKind::AsyncCompute)] = graphics;
    }
    if (desc.request_transfer_queue && transfer != kNone) {
        queue_families_[static_cast<u32>(QueueKind::Transfer)] = transfer;
        queue_present_[static_cast<u32>(QueueKind::Transfer)] = true;
    } else {
        queue_families_[static_cast<u32>(QueueKind::Transfer)] = graphics;
    }

    const f32 priority = 1.0F;
    Array<VkDeviceQueueCreateInfo> queue_infos(*allocator_);
    for (u32 kind = 0; kind < kQueueKindCount; ++kind) {
        if (!queue_present_[kind]) {
            continue;
        }
        VkDeviceQueueCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        info.queueFamilyIndex = queue_families_[kind];
        info.queueCount = 1;
        info.pQueuePriorities = &priority;
        if (Status pushed = queue_infos.push_back(info); !pushed) {
            return pushed;
        }
    }

    u32 extension_count = 0;
    (void)vkEnumerateDeviceExtensionProperties(physical_, nullptr, &extension_count, nullptr);
    Array<VkExtensionProperties> available(*allocator_);
    if (Status sized = available.resize(extension_count); !sized) {
        return sized;
    }
    (void)vkEnumerateDeviceExtensionProperties(physical_, nullptr, &extension_count,
                                               available.data());

    Array<const char*> device_extensions(*allocator_);
    if (Status pushed = device_extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME); !pushed) {
        return pushed;
    }
    memory_budget_ =
        has_extension(available.data(), extension_count, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    if (memory_budget_) {
        if (Status pushed = device_extensions.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
            !pushed) {
            return pushed;
        }
    }

    // The 1.3 baseline, requested explicitly rather than assumed: dynamic rendering removes
    // VkRenderPass objects, synchronization2 is what every barrier this engine derives is expressed
    // in, and timeline semaphores are what a cross-queue dependency becomes.
    VkPhysicalDeviceVulkan13Features features13{};
    features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features13.dynamicRendering = VK_TRUE;
    features13.synchronization2 = VK_TRUE;

    VkPhysicalDeviceVulkan12Features features12{};
    features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    features12.pNext = &features13;
    features12.timelineSemaphore = VK_TRUE;
    features12.bufferDeviceAddress = VK_TRUE;

    // Bindless. Asked for, then checked: a device that refuses these gets the compatibility path
    // and the reduced GPU-driven capability is REPORTED rather than silently degraded.
    VkPhysicalDeviceVulkan12Features supported12{};
    supported12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    VkPhysicalDeviceFeatures2 supported{};
    supported.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    supported.pNext = &supported12;
    vkGetPhysicalDeviceFeatures2(physical_, &supported);

    const bool bindless = supported12.descriptorIndexing == VK_TRUE &&
                          supported12.descriptorBindingPartiallyBound == VK_TRUE &&
                          supported12.runtimeDescriptorArray == VK_TRUE &&
                          supported12.shaderSampledImageArrayNonUniformIndexing == VK_TRUE;
    if (bindless) {
        features12.descriptorIndexing = VK_TRUE;
        features12.descriptorBindingPartiallyBound = VK_TRUE;
        features12.runtimeDescriptorArray = VK_TRUE;
        features12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
        features12.descriptorBindingVariableDescriptorCount = VK_TRUE;
        features12.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
        features12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    }
    model_ = bindless ? DescriptorModel::Bindless : DescriptorModel::Compatibility;

    VkPhysicalDeviceFeatures2 features{};
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features.pNext = &features12;
    features.features.samplerAnisotropy = supported.features.samplerAnisotropy;
    features.features.multiDrawIndirect = supported.features.multiDrawIndirect;
    features.features.fillModeNonSolid = supported.features.fillModeNonSolid;
    features.features.shaderInt64 = supported.features.shaderInt64;

    VkDeviceCreateInfo create{};
    create.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    create.pNext = &features;
    create.queueCreateInfoCount = static_cast<u32>(queue_infos.size());
    create.pQueueCreateInfos = queue_infos.data();
    create.enabledExtensionCount = static_cast<u32>(device_extensions.size());
    create.ppEnabledExtensionNames = device_extensions.data();

    CY_VK_TRY(vkCreateDevice(physical_, &create, nullptr, &device_), "vkCreateDevice");
    volkLoadDevice(device_);

    for (u32 kind = 0; kind < kQueueKindCount; ++kind) {
        const u32 family = queue_families_[kind];
        vkGetDeviceQueue(device_, family, 0, &queues_[kind]);

        // ONE TIMELINE SEMAPHORE PER QUEUE. A cross-queue dependency becomes a wait on the
        // producer's timeline. Binary semaphores were implemented and tested and made no difference
        // to validation; timeline wins because it removes the "every signal must be waited exactly
        // once" bookkeeping, which is the part that breaks when a submit is culled.
        VkSemaphoreTypeCreateInfo type{};
        type.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        type.initialValue = 0;
        VkSemaphoreCreateInfo semaphore{};
        semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        semaphore.pNext = &type;
        CY_VK_TRY(vkCreateSemaphore(device_, &semaphore, nullptr, &timelines_[kind]),
                  "vkCreateSemaphore (timeline)");
    }

    fill_capabilities();

    ValidationMessage message;
    if (Status limits = validate_device_limits(capabilities_.limits(), message); !limits) {
        // A device that cannot meet the engine's hard limits is refused once, here, rather than
        // discovered a pipeline at a time six months later.
        return fail(limits.error().code, "this device does not meet the engine's hard limits");
    }
    return ok();
}

void VulkanDevice::fill_capabilities() noexcept {
    VkPhysicalDeviceSubgroupProperties subgroup{};
    subgroup.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES;
    VkPhysicalDeviceProperties2 properties{};
    properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    properties.pNext = &subgroup;
    vkGetPhysicalDeviceProperties2(physical_, &properties);

    capabilities_.set_backend(BackendKind::Vulkan);
    capabilities_.set_device_name(properties.properties.deviceName);

    char version[64] = {};
    const u32 driver = properties.properties.driverVersion;
    (void)std::snprintf(version, sizeof(version), "%u.%u.%u (api %u.%u.%u)",
                        VK_VERSION_MAJOR(driver), VK_VERSION_MINOR(driver),
                        VK_VERSION_PATCH(driver),
                        VK_VERSION_MAJOR(properties.properties.apiVersion),
                        VK_VERSION_MINOR(properties.properties.apiVersion),
                        VK_VERSION_PATCH(properties.properties.apiVersion));
    capabilities_.set_driver_version(version);

    const VkPhysicalDeviceLimits& limits = properties.properties.limits;
    DeviceLimits& out = capabilities_.limits();
    out.max_bound_descriptor_sets = limits.maxBoundDescriptorSets;
    out.max_push_constant_bytes = limits.maxPushConstantsSize;
    out.max_vertex_attributes = limits.maxVertexInputAttributes;
    out.max_color_attachments = limits.maxColorAttachments;
    out.max_texture_dimension_2d = limits.maxImageDimension2D;
    out.max_texture_array_layers = limits.maxImageArrayLayers;
    out.max_compute_workgroup_size[0] = limits.maxComputeWorkGroupSize[0];
    out.max_compute_workgroup_size[1] = limits.maxComputeWorkGroupSize[1];
    out.max_compute_workgroup_size[2] = limits.maxComputeWorkGroupSize[2];
    out.max_compute_workgroup_invocations = limits.maxComputeWorkGroupInvocations;
    out.subgroup_size = subgroup.subgroupSize;
    out.max_sampled_images_per_stage = limits.maxPerStageDescriptorSampledImages;
    out.max_storage_buffers_per_stage = limits.maxPerStageDescriptorStorageBuffers;
    out.min_uniform_buffer_offset_alignment = limits.minUniformBufferOffsetAlignment;
    out.min_storage_buffer_offset_alignment = limits.minStorageBufferOffsetAlignment;
    out.optimal_buffer_copy_offset_alignment = limits.optimalBufferCopyOffsetAlignment;
    out.non_coherent_atom_size = limits.nonCoherentAtomSize;
    out.max_sampler_anisotropy = limits.maxSamplerAnisotropy;
    out.timestamp_period_ns = static_cast<u64>(limits.timestampPeriod);

    capabilities_.set(Capability::ComputeShaders, true);
    capabilities_.set(Capability::DynamicRendering, true);
    capabilities_.set(Capability::AsyncCompute,
                      queue_present_[static_cast<u32>(QueueKind::AsyncCompute)]);
    capabilities_.set(Capability::DedicatedTransferQueue,
                      queue_present_[static_cast<u32>(QueueKind::Transfer)]);
    capabilities_.set(Capability::Bindless, model_ == DescriptorModel::Bindless);
    capabilities_.set(Capability::BindlessPartiallyBound, model_ == DescriptorModel::Bindless);
    capabilities_.set(Capability::DescriptorIndexingNonUniform,
                      model_ == DescriptorModel::Bindless);
    capabilities_.set(Capability::BufferDeviceAddress, true);
    capabilities_.set(Capability::TimestampQueries, limits.timestampComputeAndGraphics == VK_TRUE);
    capabilities_.set(Capability::MemoryBudgetReporting, memory_budget_);
    capabilities_.set(Capability::DebugMarkers, debug_markers_);
    capabilities_.set(Capability::SubgroupBallot,
                      (subgroup.supportedOperations & VK_SUBGROUP_FEATURE_BALLOT_BIT) != 0);
    capabilities_.set(Capability::SubgroupArithmetic,
                      (subgroup.supportedOperations & VK_SUBGROUP_FEATURE_ARITHMETIC_BIT) != 0);
    capabilities_.set(Capability::Multiview, true);

    // Per format, from the device rather than from a table: a device may sample R32Sfloat and
    // refuse to blend it, and `rhi-and-render-graph` requires per-format support to be queryable.
    for (u32 index = 1; index < static_cast<u32>(Format::Count); ++index) {
        const auto format = static_cast<Format>(index);
        const VkFormat vulkan = to_vulkan(format);
        if (vulkan == VK_FORMAT_UNDEFINED) {
            continue;
        }
        VkFormatProperties properties_for_format{};
        vkGetPhysicalDeviceFormatProperties(physical_, vulkan, &properties_for_format);
        const VkFormatFeatureFlags optimal = properties_for_format.optimalTilingFeatures;
        FormatFeature features = FormatFeature::None;
        if ((optimal & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0) {
            features = features | FormatFeature::SampledImage;
        }
        if ((optimal & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0) {
            features = features | FormatFeature::StorageImage;
        }
        if ((optimal & VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT) != 0) {
            features = features | FormatFeature::StorageImageAtomic;
        }
        if ((optimal & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0) {
            features = features | FormatFeature::ColorAttachment;
        }
        if ((optimal & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT) != 0) {
            features = features | FormatFeature::ColorAttachmentBlend;
        }
        if ((optimal & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0) {
            features = features | FormatFeature::DepthStencilAttachment;
        }
        if ((optimal & VK_FORMAT_FEATURE_BLIT_SRC_BIT) != 0) {
            features = features | FormatFeature::BlitSource;
        }
        if ((optimal & VK_FORMAT_FEATURE_BLIT_DST_BIT) != 0) {
            features = features | FormatFeature::BlitDestination;
        }
        capabilities_.set_format_features(format, features);
    }
}

// --- The allocator, frames, bindless and breadcrumbs
// --------------------------------------------------

Status VulkanDevice::create_allocator() noexcept {
    // volk fetched every entry point; VMA is handed the table explicitly rather than linking the
    // loader's symbols, which is what lets the engine link no Vulkan library at all.
    VmaVulkanFunctions functions{};
    functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    functions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;

    VmaAllocatorCreateInfo create{};
    create.physicalDevice = physical_;
    create.device = device_;
    create.instance = instance_;
    create.vulkanApiVersion = kInstanceApiVersion;
    create.pVulkanFunctions = &functions;
    create.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    if (memory_budget_) {
        // VK_EXT_memory_budget's heapUsage tracked M3's aliasing plan exactly (64.00 -> 8.00 MiB),
        // which is what makes it a usable permanent gate rather than an estimate.
        create.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
    }
    CY_VK_TRY(vmaCreateAllocator(&create, &vma_), "vmaCreateAllocator");

    VkPipelineCacheCreateInfo cache{};
    cache.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    CY_VK_TRY(vkCreatePipelineCache(device_, &cache, nullptr, &pipeline_cache_),
              "vkCreatePipelineCache");
    return ok();
}

Status VulkanDevice::create_frames(const DeviceDescription& desc) noexcept {
    (void)desc;
    for (u32 slot = 0; slot < frames_in_flight_; ++slot) {
        FrameContext& frame = frames_[slot];
        // Command pools are created on first use by the thread that will record into them; see
        // pool_for_this_thread(). Only slot 0 of each queue is made here, because the main thread
        // records every frame and paying for sixteen pools per queue up front would be paying for
        // parallel recording a frame may never do.

        // SAMPLED_IMAGE as well as COMBINED_IMAGE_SAMPLER: `DescriptorKind::SampledTexture` and
        // `DescriptorKind::Sampler` are separate bindings in this RHI (the standard material binds
        // a texture and a comparison sampler independently), and a pool that sizes only the
        // combined type has nothing to hand out for either.
        const VkDescriptorPoolSize sizes[] = {
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 512},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 512},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 512},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 256},
            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 512},
            {VK_DESCRIPTOR_TYPE_SAMPLER, 64},
        };
        VkDescriptorPoolCreateInfo descriptors{};
        descriptors.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        descriptors.maxSets = 512;
        descriptors.poolSizeCount = sizeof(sizes) / sizeof(sizes[0]);
        descriptors.pPoolSizes = sizes;
        CY_VK_TRY(vkCreateDescriptorPool(device_, &descriptors, nullptr, &frame.descriptor_pool),
                  "vkCreateDescriptorPool");

        VkFenceCreateInfo fence{};
        fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        CY_VK_TRY(vkCreateFence(device_, &fence, nullptr, &frame.fence), "vkCreateFence");
    }

    // AND ONE POOL THAT IS NEVER RESET. `allocate_descriptor_set(layout, per_frame = false)`
    // promises a set that outlives the frame it was allocated in; the frame pools above cannot keep
    // that promise, because each is reset the moment its slot comes round. Without this pool the
    // promise was silently broken after `frames_in_flight_` frames — see the comment on
    // `persistent_descriptor_pool_`.
    //
    // FREE_DESCRIPTOR_SET, unlike the frame pools: sets here are released one at a time when the
    // owner destroys them, never wholesale, so the pool must support individual frees.
    const VkDescriptorPoolSize persistent_sizes[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 512},         {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 512},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 512}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 256},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 512},          {VK_DESCRIPTOR_TYPE_SAMPLER, 64},
    };
    VkDescriptorPoolCreateInfo persistent{};
    persistent.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    persistent.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    persistent.maxSets = 512;
    persistent.poolSizeCount = sizeof(persistent_sizes) / sizeof(persistent_sizes[0]);
    persistent.pPoolSizes = persistent_sizes;
    CY_VK_TRY(vkCreateDescriptorPool(device_, &persistent, nullptr, &persistent_descriptor_pool_),
              "vkCreateDescriptorPool (persistent)");
    return ok();
}

Status VulkanDevice::create_bindless_table() noexcept {
    if (model_ != DescriptorModel::Bindless) {
        // The compatibility path. `rhi-and-render-graph` requires the renderer's structure to be
        // unchanged and the reduced GPU-driven capability to be reported rather than to degrade
        // silently — which is what capabilities().supports_gpu_driven() answering false does.
        return ok();
    }

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = kBindlessCapacity;
    binding.stageFlags = VK_SHADER_STAGE_ALL;

    const VkDescriptorBindingFlags binding_flags =
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
        VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT |
        VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
    VkDescriptorSetLayoutBindingFlagsCreateInfo flags{};
    flags.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    flags.bindingCount = 1;
    flags.pBindingFlags = &binding_flags;

    VkDescriptorSetLayoutCreateInfo layout{};
    layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout.pNext = &flags;
    layout.bindingCount = 1;
    layout.pBindings = &binding;
    layout.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    CY_VK_TRY(vkCreateDescriptorSetLayout(device_, &layout, nullptr, &bindless_layout_),
              "vkCreateDescriptorSetLayout (bindless)");

    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kBindlessCapacity};
    VkDescriptorPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool.maxSets = 1;
    pool.poolSizeCount = 1;
    pool.pPoolSizes = &size;
    pool.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    CY_VK_TRY(vkCreateDescriptorPool(device_, &pool, nullptr, &bindless_pool_),
              "vkCreateDescriptorPool (bindless)");

    VkDescriptorSetAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate.descriptorPool = bindless_pool_;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &bindless_layout_;
    CY_VK_TRY(vkAllocateDescriptorSets(device_, &allocate, &bindless_set_),
              "vkAllocateDescriptorSets (bindless)");
    return ok();
}

Status VulkanDevice::create_breadcrumbs() noexcept {
    // `rhi-and-render-graph`: breadcrumb markers per pass, "so that they survive into a crash
    // artefact when the trace tail does not". Host-visible and host-coherent, so the value the GPU
    // last managed to write is readable after a device loss without a working queue.
    VkBufferCreateInfo buffer{};
    buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer.size = static_cast<VkDeviceSize>(kBreadcrumbSlots) * sizeof(u32);
    buffer.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocation{};
    allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    allocation.flags =
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VmaAllocationInfo info{};
    CY_VK_TRY(vmaCreateBuffer(vma_, &buffer, &allocation, &breadcrumb_buffer_,
                              &breadcrumb_allocation_, &info),
              "vmaCreateBuffer (breadcrumbs)");
    breadcrumb_mapped_ = static_cast<u32*>(info.pMappedData);
    if (breadcrumb_mapped_ != nullptr) {
        std::memset(breadcrumb_mapped_, 0, static_cast<usize>(buffer.size));
    }
    name_object(reinterpret_cast<u64>(breadcrumb_buffer_), VK_OBJECT_TYPE_BUFFER, "cy.breadcrumbs");
    return ok();
}

// --- Reporting
// -----------------------------------------------------------------------------------------

void VulkanDevice::report_validation(ValidationSeverity severity, const char* message) noexcept {
    if (severity == ValidationSeverity::Error) {
        ++stats_.validation_errors;
    } else if (severity == ValidationSeverity::Warning) {
        ++stats_.validation_warnings;
    }
    if (validation_callback_ != nullptr) {
        validation_callback_(severity, message, validation_user_);
    }
    if (break_on_validation_error_ && severity == ValidationSeverity::Error) {
        // `rhi-and-render-graph`: "log it with the pass name and, by configuration, break into the
        // debugger". CY_ASSERT is the engine's break: it routes through the installed handler and
        // aborts, which is what a development build wants when a frame renders but is wrong.
        CY_ASSERT_MSG(false, "a Vulkan validation error, with break-on-error configured");
    }
}

void VulkanDevice::name_object(u64 handle, VkObjectType type, const char* name) noexcept {
    if (!debug_markers_ || name == nullptr || vkSetDebugUtilsObjectNameEXT == nullptr) {
        return;
    }
    VkDebugUtilsObjectNameInfoEXT info{};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
    info.objectType = type;
    info.objectHandle = handle;
    info.pObjectName = name;
    (void)vkSetDebugUtilsObjectNameEXT(device_, &info);
}

void VulkanDevice::set_validation_callback(ValidationCallback callback, void* user) noexcept {
    validation_callback_ = callback;
    validation_user_ = user;
}

u32 VulkanDevice::queue_family(QueueKind queue) const noexcept {
    const auto index = static_cast<u32>(queue);
    return index < kQueueKindCount ? queue_families_[index] : 0;
}

bool VulkanDevice::has_queue(QueueKind queue) const noexcept {
    const auto index = static_cast<u32>(queue);
    return index < kQueueKindCount && queue_present_[index];
}

BarrierRecorder& VulkanDevice::barrier_recorder(const GraphBarrierKey& key) noexcept {
    (void)key;
    return barriers_;
}

// --- Registration
// ----------------------------------------------------------------------------------------

namespace {

Expected<Device*, Error> create_vulkan_device(Allocator& allocator,
                                              const DeviceDescription& desc) noexcept {
    void* storage = allocator.allocate(sizeof(VulkanDevice), alignof(VulkanDevice));
    if (storage == nullptr) {
        return fail(ErrorCode::OutOfMemory, "no memory for a Vulkan device");
    }
    auto* device = ::new (storage) VulkanDevice(allocator, desc);
    if (Status started = device->initialise(desc); !started) {
        device->~VulkanDevice();
        allocator.deallocate(storage, sizeof(VulkanDevice), alignof(VulkanDevice));
        return make_unexpected(started.error());
    }
    return static_cast<Device*>(device);
}

void destroy_vulkan_device(Allocator& allocator, Device* device) noexcept {
    if (device == nullptr) {
        return;
    }
    // The registry only ever hands back a device this factory made. -fno-rtti is in force, so this
    // is a static_cast with a precondition rather than a dynamic_cast.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
    auto* concrete = static_cast<VulkanDevice*>(device);
    concrete->~VulkanDevice();
    allocator.deallocate(concrete, sizeof(VulkanDevice), alignof(VulkanDevice));
}

}  // namespace

Status register_vulkan_backend() noexcept {
    BackendRegistration registration;
    registration.name = "vulkan";
    registration.kind = BackendKind::Vulkan;
    registration.create = &create_vulkan_device;
    registration.destroy = &destroy_vulkan_device;
    // Answered without creating a device, so selection falls back to the null backend rather than
    // reporting a driver's creation failure as the reason nothing rendered.
    registration.is_available = &vulkan_available;
    return register_backend(registration);
}

namespace {

/// Registers the backend when this translation unit is part of the link. See the null backend's
/// equivalent for why this is both an initialiser and a callable function.
[[maybe_unused]] const Status kVulkanBackendRegistered = register_vulkan_backend();

}  // namespace

}  // namespace cy::rhi::vulkan
