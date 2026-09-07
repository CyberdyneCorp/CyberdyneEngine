# src/backends/ — layer 3: the concrete implementations of the layer-2 server interfaces, and the
# only place in the engine where a third-party graphics, physics or audio API may be named.
#
# M3 opens this layer with the RHI. Subdirectories are added as they arrive; each one is a target
# declared through cy_add_module() and each one is gated by the feature option that owns it, so a
# build with the option off contains no source from it rather than source that is never called.
#
# THE RULE THIS DIRECTORY EXISTS TO KEEP: no Vulkan, Slang or SPIR-V type appears above here — the
# same rule that keeps SDL inside platform/, and enforced the same way, by
# tools/layercheck/layercheck.py's `gpuapi` check. A header under src/backends/rhi/include/ that
# included <vulkan/vulkan.h> would fail the build, which is why the RHI's synchronisation vocabulary
# (cy/backends/rhi/types.h) is engine-owned rather than a typedef of Vulkan's.
add_subdirectory(rhi)

# The Metal seed. M7 task 10.5, delivered at M11.
#
# It is added HERE, right after rhi/, because what it is FOR is the RHI's interface rather than
# Metal: `delivery-roadmap` seeds a second backend early "to expose Vulkan-specific assumptions in
# rhi-and-render-graph while they are still cheap", and eight of them are named, priced and tested
# in this module — on a machine with no Metal, which is why the target builds unconditionally and
# only its `src/device.mm` is gated by CY_RENDERER_METAL and APPLE.
add_subdirectory(rhi-metal)

# The shader system: Slang in, SPIR-V out, and the caching, permutation and reflection-driven
# binding built around somebody else's compiler. Read after rhi/ because cy::shader consumes the
# RHI's descriptor and pipeline vocabulary — reflection produces rhi::DescriptorBinding directly,
# which is what stops a second description of a layout existing to drift from the first.
add_subdirectory(shader)

# The default audio backend: miniaudio behind cy::audio::AudioBackend. Gated by CY_AUDIO, which also
# gates the miniaudio fetch — a disabled backend downloads nothing, which is what `audio` requires of
# it. The interface, the retained null backend and every suite over them are in src/servers/audio/
# and are always built, so `-D CY_AUDIO=OFF` still mixes and still tests.
if(CY_AUDIO)
    add_subdirectory(audio-miniaudio)
endif()

# Jolt Physics behind `cy::physics::PhysicsServer`. Gated by CY_PHYSICS, which is ON by default and
# also gates the Jolt fetch — a build with the option off downloads nothing and still simulates
# through the reference backend in src/servers/physics/reference/.
#
# No JPH header appears outside this directory, which is what keeps `cy::physics`' vocabulary
# engine-owned rather than a typedef of Jolt's. Today that is held by the layer table (layer 2
# cannot include layer 3) and by `cy::dep::jolt` being a PRIVATE link; a `physicsapi` check beside
# `gpuapi` in tools/layercheck/layercheck.py would make it belt and braces, and is recommended.
if(CY_PHYSICS)
    add_subdirectory(physics-jolt)
endif()

# The viewport transport's pixel half: the engine's rendered frames, in memory the editor imports
# rather than copies. M7 task 5b.1.
#
# HERE, LIKE THE OTHERS, BECAUSE IT NAMES A GRAPHICS API. Exporting an image's memory as a dma-buf
# needs `VK_EXT_image_drm_format_modifier` and `VK_KHR_external_memory_fd` at image creation, which
# is not something `cy::rhi::Device` can be asked for — deliberately, since a renderer that could
# reach the underlying `VkImage` could branch on a backend. Its own CMakeLists.txt argues the rest,
# including why it is not folded into rhi/vulkan/ and what two Vulkan devices in one process cost.
#
# Not gated by an option of its own: it is gated by CY_RENDERER_VULKAN, which is what decides
# whether there is a device on this build at all, and it is Linux-only for the same reason the
# editor's half is.
add_subdirectory(viewport)
