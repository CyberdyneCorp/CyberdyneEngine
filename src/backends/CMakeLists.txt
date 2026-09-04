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

# The shader system: Slang in, SPIR-V out, and the caching, permutation and reflection-driven
# binding built around somebody else's compiler. Read after rhi/ because cy::shader consumes the
# RHI's descriptor and pipeline vocabulary — reflection produces rhi::DescriptorBinding directly,
# which is what stops a second description of a layout existing to drift from the first.
add_subdirectory(shader)
