//! Importing the runtime's images and semaphores into the editor's device.
//!
//! Two imports, and both are the same idea: the editor creates an object of its own and binds it to
//! memory or to a synchronisation primitive that another process owns. Nothing is copied, which is
//! the entire reason the shared-texture transport exists — the alternative, measured, costs about
//! 2.0 ms a frame at 1080p and 8.9 ms at 4K, before anything is drawn.

use std::os::fd::{IntoRawFd, OwnedFd};

use ash::vk;
use cy_editor_core::problem::{Problem, Result};

use crate::device::Gpu;
use crate::wire::{Handshake, PlaneDescription};

/// Import one dma-buf as a `wgpu::Texture`.
///
/// The descriptor is consumed: Vulkan takes ownership on success, and on failure it is closed by
/// its own `Drop` rather than leaked.
///
/// # The layout the import must be told, and the one it must not guess
///
/// `modifier`, `stride` and `offset` come from the runtime's driver, not from arithmetic.
/// `DRM_FORMAT_MOD_LINEAR` is **not available on this hardware**, so an import that assumed a
/// linear layout and `width * 4` per row would read a tiled image as though it were linear — which
/// does not fail, it produces a scrambled picture that looks like a renderer defect.
pub fn import_dmabuf(
    gpu: &Gpu,
    descriptor: OwnedFd,
    width: u32,
    height: u32,
    modifier: u64,
    plane: PlaneDescription,
) -> Result<wgpu::Texture> {
    let size = wgpu::Extent3d {
        width,
        height,
        depth_or_array_layers: 1,
    };
    let hal_descriptor = wgpu_hal::TextureDescriptor {
        label: Some("engine-viewport"),
        size,
        mip_level_count: 1,
        sample_count: 1,
        dimension: wgpu::TextureDimension::D2,
        format: wgpu::TextureFormat::Rgba8Unorm,
        usage: wgpu_types::TextureUses::RESOURCE
            | wgpu_types::TextureUses::COPY_SRC
            | wgpu_types::TextureUses::COPY_DST
            | wgpu_types::TextureUses::COLOR_TARGET,
        memory_flags: wgpu_hal::MemoryFlags::empty(),
        view_formats: vec![],
    };
    // SAFETY: the device is Vulkan (it was made by `Gpu::create`), the descriptor is a dma-buf the
    // runtime exported for an image of exactly this size, format and modifier — the handshake is
    // validated before this is called — and ownership of the descriptor passes to Vulkan.
    let hal_texture = unsafe {
        gpu.device
            .as_hal::<wgpu_hal::api::Vulkan>()
            .ok_or_else(|| {
                Problem::new(
                    "import the runtime's image",
                    "the editor's device is not a Vulkan one",
                )
            })?
            .texture_from_dmabuf_fd(
                descriptor,
                &hal_descriptor,
                modifier,
                plane.stride,
                plane.offset,
            )
            .map_err(|error| {
                Problem::new(
                    "import the runtime's image",
                    format!("vkBindImageMemory over the dma-buf failed: {error:?}"),
                )
                .with_remedy(
                    "the runtime and the editor must be using the same physical device, and the \
                     image's modifier, stride and allocation size must be the driver's own",
                )
            })?
    };
    let descriptor = wgpu::TextureDescriptor {
        label: Some("engine-viewport"),
        size,
        mip_level_count: 1,
        sample_count: 1,
        dimension: wgpu::TextureDimension::D2,
        format: wgpu::TextureFormat::Rgba8Unorm,
        usage: wgpu::TextureUsages::TEXTURE_BINDING
            | wgpu::TextureUsages::COPY_SRC
            | wgpu::TextureUsages::COPY_DST
            | wgpu::TextureUsages::RENDER_ATTACHMENT,
        view_formats: &[],
    };
    // THE IMPORTED IMAGE STARTS UNDEFINED ON *THIS* DEVICE, AND SAYING OTHERWISE IS A LIE THE
    // VALIDATION LAYER CATCHES.
    //
    // An image layout belongs to a `VkImage` on one device, not to the memory behind it. This
    // `VkImage` was created a few lines above by *the editor's* device with
    // `initialLayout = VK_IMAGE_LAYOUT_UNDEFINED`, and nothing on this device has transitioned it —
    // whatever the runtime's own image, in the runtime's own device, has been transitioned to.
    //
    // Declaring the initial state as `RESOURCE` told wgpu the transition had already happened, so
    // wgpu emitted no barrier and every sample of the image was a read of an image in the wrong
    // layout. Measured by running the window and the headless probe with
    // `VK_LAYER_KHRONOS_validation` enabled: **2,759 `UNASSIGNED-CoreValidation-DrawState-
    // InvalidImageLayout` errors in a fourteen-second session**, one per sample, all of them
    // "expects VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL — instead, current layout is
    // VK_IMAGE_LAYOUT_UNDEFINED". It does not corrupt anything on this driver, which is exactly why
    // it survived: the image displays correctly and nothing says a word without the layer on.
    //
    // The argument against `UNINITIALIZED` was that an `UNDEFINED -> ...` barrier is permitted to
    // discard the contents. It is, and here it cannot cost a frame the runtime produced: the
    // transition happens once per slot, at the first sample of that slot, and the memory's tiling
    // is fixed by the **explicit DRM format modifier** the handshake carries, so there is no
    // re-tiling for a discard to be about. That is the same reasoning every dma-buf importer uses,
    // and it is measured rather than assumed — with this line changed, the probe reports
    // "240 editor frames … showed 240 distinct runtime frames of 240 announced" and
    // "TEAR CHECK: 0 of 240 sampled frames were torn … 0 older, 0 NEWER", with no validation error
    // at all. `tests/across_a_process_boundary.rs::the_editor_samples_the_runtimes_image_in_a_layout_
    // the_validation_layer_agrees_with` holds it there.
    //
    // SAFETY: `hal_texture` was created immediately above from this same device and is passed
    // exactly once; the state named is the layout this device's image is actually in.
    Ok(unsafe {
        gpu.device.create_texture_from_hal::<wgpu_hal::api::Vulkan>(
            hal_texture,
            &descriptor,
            wgpu_types::TextureUses::UNINITIALIZED,
        )
    })
}

/// Import every image a handshake describes, in slot order.
///
/// The descriptors are consumed in the order the publisher sent them, which is the order the
/// handshake's planes are in. A mismatch here would show the wrong slot's pixels for every frame,
/// so it is worth saying out loud that the two orders are the same order.
pub fn import_ring(
    gpu: &Gpu,
    handshake: &Handshake,
    descriptors: Vec<OwnedFd>,
) -> Result<Vec<wgpu::Texture>> {
    let count = handshake.buffer_count as usize;
    if descriptors.len() != count {
        return Err(Problem::new(
            "import the runtime's ring",
            format!(
                "the handshake announces {count} images and {} descriptors arrived",
                descriptors.len()
            ),
        ));
    }
    descriptors
        .into_iter()
        .enumerate()
        .map(|(slot, descriptor)| {
            import_dmabuf(
                gpu,
                descriptor,
                handshake.width,
                handshake.height,
                handshake.modifier,
                handshake.planes[slot],
            )
        })
        .collect()
}

/// Import a timeline semaphore another process exported over `OPAQUE_FD`.
///
/// # Why the guard comes first
///
/// Without `VK_KHR_external_semaphore_fd` enabled, ash's `import_semaphore_fd` is a panicking stub
/// that **aborts the process**. There is no recovering from calling it, so the capability is
/// checked before the call rather than after it, and the check is whether the extension was
/// enabled — never whether a function pointer is non-null, which is true either way.
pub fn import_timeline(gpu: &Gpu, descriptor: OwnedFd) -> Result<vk::Semaphore> {
    let entry_points = gpu.semaphore_fd()?;
    let mut kind = vk::SemaphoreTypeCreateInfo::default()
        .semaphore_type(vk::SemaphoreType::TIMELINE)
        .initial_value(0);
    // SAFETY: a semaphore created on the editor's own device, destroyed by the session that owns
    // it. `push_next` borrows `kind`, which outlives the call.
    let semaphore = unsafe {
        gpu.ash_device.create_semaphore(
            &vk::SemaphoreCreateInfo::default().push_next(&mut kind),
            None,
        )
    }
    .map_err(|error| {
        Problem::new(
            "create a timeline semaphore for the runtime's frames",
            error.to_string(),
        )
    })?;
    // SAFETY: the extension is enabled (checked above, by asking whether it was enabled rather than
    // whether a pointer is non-null); `semaphore` was created immediately above as a timeline; the
    // descriptor is an OPAQUE_FD export of a timeline on the same physical device, and Vulkan takes
    // ownership of it on success.
    let imported = unsafe {
        entry_points.import_semaphore_fd(
            &vk::ImportSemaphoreFdInfoKHR::default()
                .semaphore(semaphore)
                .handle_type(vk::ExternalSemaphoreHandleTypeFlags::OPAQUE_FD)
                .flags(vk::SemaphoreImportFlags::empty())
                .fd(descriptor.into_raw_fd()),
        )
    };
    if let Err(error) = imported {
        // SAFETY: destroying a semaphore this function created moments ago and no longer refers to.
        unsafe { gpu.ash_device.destroy_semaphore(semaphore, None) };
        return Err(Problem::new(
            "import the runtime's timeline semaphore",
            format!("vkImportSemaphoreFdKHR: {error}"),
        )
        .with_remedy(
            "timelines cross a process boundary over OPAQUE_FD; SYNC_FD is binary-only and cannot \
             carry one",
        ));
    }
    Ok(semaphore)
}
