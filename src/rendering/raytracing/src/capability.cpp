#include <cy/rendering/raytracing/capability.h>

namespace cy::rendering::rt {

AccelerationService::ServiceConfig service_config_for(const rhi::DeviceCapabilities& capabilities,
                                                      bool enabled_by_profile,
                                                      const BuildBudget& budget) noexcept {
    AccelerationService::ServiceConfig config;
    config.device_supports_ray_tracing = capabilities.has(rhi::Capability::RayTracing);
    config.enabled_by_profile = enabled_by_profile;
    config.budget = budget;
    return config;
}

const char* ray_tracing_refusal(const rhi::DeviceCapabilities& capabilities) noexcept {
    // IN THE ORDER A DEVICE FAILS THEM, so the first answer is the useful one: a device with no
    // extension has nothing to say about features, and a device with the features and no enable is
    // a mistake in this engine rather than in the driver.
    const rhi::RayTracingObservation& observed = capabilities.ray_tracing_observation();
    if (!observed.acceleration_structure_extension) {
        return "the device does not list VK_KHR_acceleration_structure";
    }
    if (!observed.deferred_host_operations_extension) {
        return "the device lists VK_KHR_acceleration_structure without "
               "VK_KHR_deferred_host_operations, so no structure can be built";
    }
    if (!observed.ray_query_extension) {
        return "the device does not list VK_KHR_ray_query";
    }
    if (!observed.acceleration_structure_feature) {
        return "the device lists the extension and does not report the accelerationStructure "
               "feature";
    }
    if (!observed.ray_query_feature) {
        return "the device lists the extension and does not report the rayQuery feature";
    }
    if (!observed.enabled_on_the_device) {
        return "the engine created the device without asking for the ray-tracing features";
    }
    return "";
}

}  // namespace cy::rendering::rt
