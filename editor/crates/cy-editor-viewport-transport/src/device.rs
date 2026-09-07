//! The editor's graphics device, built so that it can import another process's semaphores.
//!
//! # The escape hatch, and why it is small
//!
//! wgpu does not enable `VK_KHR_external_semaphore_fd`, and there is no wgpu-level way to ask it
//! to. The supported way in is one layer down: [`wgpu_hal::vulkan::Adapter::open_with_callback`]
//! hands you the extension list *before* `vkCreateDevice`, so pushing what is missing and passing
//! the result to `wgpu::Adapter::create_device_from_hal` is about thirty lines. The alternative —
//! `device_from_raw` — would have us build the `VkDevice` ourselves and keep wgpu's feature and
//! extension bookkeeping in step by hand, for the same result.
//!
//! `VK_KHR_timeline_semaphore` needs no work at all: wgpu enables it unconditionally at API ≥ 1.2,
//! and wgpu's own `Fence` *is* a timeline semaphore. Timelines export and import over `OPAQUE_FD`;
//! **`SYNC_FD` is binary-only** and must not be planned on.
//!
//! # The capability check that passed by luck, and must not be copied
//!
//! The spike's first version asked whether ash's `import_semaphore_fd_khr` function pointer was
//! non-null. It answered **true with the extension disabled**, because ash installs a *panicking
//! stub* rather than a null pointer for an entry point it could not load — and calling that stub
//! aborts the process instead of returning an error.
//!
//! So the only honest question is *"was the extension enabled on this device"*, and the only honest
//! answer is the one this file computes: it is enabled if wgpu's own baseline already contained it,
//! or if we put it there ourselves. [`Gpu::external_semaphores`] is that answer and nothing in this
//! crate calls an external-semaphore entry point without consulting it first.

use ash::vk;
use cy_editor_core::problem::{Problem, Result};

/// The device extensions the transport needs, whether or not wgpu asks for them itself.
///
/// `external_semaphore_fd` is the one that is always missing. The other three are usually present
/// via `wgpu::Features::VULKAN_EXTERNAL_MEMORY_DMA_BUF`, and are listed so that a wgpu release that
/// stopped requesting one of them fails here — with a sentence — rather than at the first import.
pub const WANTED_EXTENSIONS: &[&std::ffi::CStr] = &[
    ash::khr::external_semaphore_fd::NAME,
    ash::khr::external_memory_fd::NAME,
    ash::ext::external_memory_dma_buf::NAME,
    ash::ext::image_drm_format_modifier::NAME,
];

/// The editor's device, and the Vulkan handles underneath it.
///
/// The wgpu half is public because the render crate draws with it — and because eframe accepts an
/// existing setup, which is what keeps the editor to **one** device rather than one for the
/// interface and one for the viewport.
pub struct Gpu {
    /// The wgpu instance the device was made from.
    pub instance: wgpu::Instance,
    /// The adapter chosen.
    pub adapter: wgpu::Adapter,
    /// The device, with the transport's extensions enabled.
    pub device: wgpu::Device,
    /// The device's queue.
    pub queue: wgpu::Queue,
    /// The same device, as ash sees it: the timeline waits go through this.
    pub ash_device: ash::Device,
    /// `VK_KHR_external_semaphore_fd`'s entry points, present only when the extension was enabled.
    ///
    /// An `Option` rather than a boolean beside a handle, so that "we did not enable it" and "call
    /// it anyway" cannot both be expressed.
    semaphore_fd: Option<ash::khr::external_semaphore_fd::Device>,
    /// What was enabled, what was missing, and what we added. Written to the log at start-up: when
    /// the viewport shows nothing, this is the first thing worth reading.
    pub notes: Vec<String>,
}

impl Gpu {
    /// Create the editor's device, adding the extensions wgpu does not ask for.
    ///
    /// `adapter_name` is a substring filter, so a machine with two GPUs can be told which one the
    /// runtime is on — importing across devices does not work, and the failure is a driver error
    /// with no explanation in it.
    ///
    /// `patch` exists so that the *same binary* can be run without the extension. That is the
    /// control the spike needed and the control a bug report needs: "it works with the patch and
    /// fails without it" is a measurement, and rebuilding to find out is not.
    pub fn create(adapter_name: &str, patch: bool) -> Result<Self> {
        let instance = wgpu::Instance::new(wgpu::InstanceDescriptor {
            backends: wgpu::Backends::VULKAN,
            flags: wgpu::InstanceFlags::default(),
            memory_budget_thresholds: wgpu::MemoryBudgetThresholds::default(),
            backend_options: wgpu::BackendOptions::default(),
            display: None,
        });
        let adapter = select_adapter(&instance, adapter_name)?;
        let mut notes = vec![format!("adapter = {}", adapter.get_info().name)];

        let features = external_memory_features(&adapter);
        let limits = wgpu::Limits {
            max_texture_dimension_2d: 8192,
            ..wgpu::Limits::default()
        };
        let (device, queue, enabled) = open_device(&adapter, features, &limits, patch, &mut notes)?;

        // SAFETY: the instance and device were created by this call with the Vulkan backend, so
        // `as_hal` yields the backend they were made with; the ash handles are cloned out and kept
        // alive by the wgpu objects this struct owns.
        let (ash_instance, ash_device) = unsafe {
            let hal_instance = instance
                .as_hal::<wgpu_hal::api::Vulkan>()
                .ok_or_else(|| not_vulkan("instance"))?;
            let raw_instance = hal_instance.shared_instance().raw_instance().clone();
            let hal_device = device
                .as_hal::<wgpu_hal::api::Vulkan>()
                .ok_or_else(|| not_vulkan("device"))?;
            (raw_instance, hal_device.raw_device().clone())
        };

        // NOT a function-pointer test. See this module's header: ash installs a panicking stub, so
        // asking whether the pointer is non-null returns true without the extension and the call
        // then aborts the process. The question is whether the extension was enabled.
        let semaphore_fd = enabled
            .then(|| ash::khr::external_semaphore_fd::Device::new(&ash_instance, &ash_device));
        notes.push(format!(
            "VK_KHR_external_semaphore_fd enabled on this device: {enabled}"
        ));

        Ok(Self {
            instance,
            adapter,
            device,
            queue,
            ash_device,
            semaphore_fd,
            notes,
        })
    }

    /// Whether this device can import another process's semaphores.
    ///
    /// False is not an error: the editor still shows the newest image, without synchronising
    /// against the runtime's writes, and says so. It is an error only if something then tries to
    /// import a timeline, which [`crate::image::import_timeline`] refuses rather than attempts.
    #[must_use]
    pub fn external_semaphores(&self) -> bool {
        self.semaphore_fd.is_some()
    }

    /// The external-semaphore entry points, or a refusal naming the extension.
    pub fn semaphore_fd(&self) -> Result<&ash::khr::external_semaphore_fd::Device> {
        self.semaphore_fd.as_ref().ok_or_else(|| {
            Problem::new(
                "use an imported timeline semaphore",
                "VK_KHR_external_semaphore_fd was not enabled on this device",
            )
            .with_remedy(
                "create the device through Gpu::create with patching on; calling the entry point \
                 without the extension aborts the process rather than failing",
            )
        })
    }

    /// A timeline semaphore's current value, read on the editor's device.
    ///
    /// Zero when it cannot be read, which is the same answer as "not signalled yet" and leads to
    /// the same decision: keep showing the frame we have.
    #[must_use]
    pub fn timeline_value(&self, semaphore: vk::Semaphore) -> u64 {
        // SAFETY: `semaphore` was created or imported on `ash_device`, which outlives this call.
        unsafe { self.ash_device.get_semaphore_counter_value(semaphore) }.unwrap_or(0)
    }

    /// Wait on the editor's own thread, for at most `timeout_nanos`, for a timeline to reach a
    /// value. `true` when it did.
    ///
    /// **This is the wait the editor uses, and the reason is in [`crate::session::WaitPolicy`]:**
    /// a wait staged on the queue instead is one bad value away from an editor that renders nothing
    /// and cannot be closed. This one is bounded, is on the CPU, and leaves nothing behind when it
    /// expires.
    #[must_use]
    pub fn wait_timeline(&self, semaphore: vk::Semaphore, value: u64, timeout_nanos: u64) -> bool {
        let semaphores = [semaphore];
        let values = [value];
        let info = vk::SemaphoreWaitInfo::default()
            .semaphores(&semaphores)
            .values(&values);
        // SAFETY: `semaphore` was imported on this device and outlives the call; the info's slices
        // outlive it too. A timeout is a `VK_TIMEOUT` result, not undefined behaviour.
        unsafe { self.ash_device.wait_semaphores(&info, timeout_nanos) }.is_ok()
    }
}

impl std::fmt::Debug for Gpu {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_struct("Gpu")
            .field("adapter", &self.adapter.get_info().name)
            .field("external_semaphores", &self.external_semaphores())
            .finish_non_exhaustive()
    }
}

fn not_vulkan(what: &str) -> Problem {
    Problem::new(
        "reach the Vulkan handles under wgpu",
        format!("the {what} is not a Vulkan one"),
    )
    .with_remedy("the editor's viewport transport is Vulkan-only; it is dma-buf and OPAQUE_FD")
}

fn select_adapter(instance: &wgpu::Instance, name: &str) -> Result<wgpu::Adapter> {
    let adapters = pollster::block_on(instance.enumerate_adapters(wgpu::Backends::VULKAN));
    adapters
        .iter()
        .find(|adapter| adapter.get_info().name.contains(name))
        .or_else(|| adapters.first())
        .cloned()
        .ok_or_else(|| {
            Problem::new(
                "choose a graphics adapter for the editor",
                "this machine reports no Vulkan adapter",
            )
            .with_remedy("the editor's viewport needs a Vulkan driver; check vulkaninfo")
        })
}

/// The external-memory features the adapter actually offers, so that an adapter without them fails
/// at the import with a name rather than at device creation with a feature list.
fn external_memory_features(adapter: &wgpu::Adapter) -> wgpu::Features {
    let mut features = wgpu::Features::empty();
    for wanted in [
        wgpu::Features::VULKAN_EXTERNAL_MEMORY_DMA_BUF,
        wgpu::Features::VULKAN_EXTERNAL_MEMORY_FD,
    ] {
        if adapter.features().contains(wanted) {
            features |= wanted;
        }
    }
    features
}

/// Open the device, pushing the missing extensions into the list before `vkCreateDevice`.
///
/// Returns the device, its queue, and whether `VK_KHR_external_semaphore_fd` ended up enabled.
fn open_device(
    adapter: &wgpu::Adapter,
    features: wgpu::Features,
    limits: &wgpu::Limits,
    patch: bool,
    notes: &mut Vec<String>,
) -> Result<(wgpu::Device, wgpu::Queue, bool)> {
    // SAFETY: `as_hal` is called on an adapter this process created with the Vulkan backend, and
    // the hal adapter is dropped before `create_device_from_hal` consumes what it produced.
    // `open_with_callback`'s callback only appends to the extension list, which is what it is for.
    let (open, enabled) = unsafe {
        let hal_adapter = adapter
            .as_hal::<wgpu_hal::api::Vulkan>()
            .ok_or_else(|| not_vulkan("adapter"))?;

        let baseline = hal_adapter.required_device_extensions(features);
        notes.push(format!("wgpu enables by itself: {}", names(&baseline)));
        let already_there = baseline.contains(&ash::khr::external_semaphore_fd::NAME);
        let missing: Vec<&'static std::ffi::CStr> = WANTED_EXTENSIONS
            .iter()
            .copied()
            .filter(|wanted| !baseline.contains(wanted))
            .collect();
        notes.push(format!("missing without patching: {}", names(&missing)));

        let callback: Option<Box<wgpu_hal::vulkan::CreateDeviceCallback<'_>>> = if patch {
            let missing = missing.clone();
            Some(Box::new(
                move |args: wgpu_hal::vulkan::CreateDeviceCallbackArgs<'_, '_, '_>| {
                    for extension in &missing {
                        args.extensions.push(extension);
                    }
                },
            ))
        } else {
            None
        };
        if patch {
            notes.push(format!("we added: {}", names(&missing)));
        }

        let open = hal_adapter
            .open_with_callback(features, limits, &wgpu::MemoryHints::default(), callback)
            .map_err(|error| {
                Problem::new(
                    "open the editor's graphics device",
                    format!("wgpu-hal refused it: {error:?}"),
                )
            })?;
        (open, patch || already_there)
    };

    // SAFETY: `open` was produced by `open_with_callback` on this same adapter, immediately above,
    // and is passed to `create_device_from_hal` exactly once.
    let (device, queue) = unsafe {
        adapter.create_device_from_hal(
            open,
            &wgpu::DeviceDescriptor {
                label: Some("cy-editor"),
                required_features: features,
                required_limits: limits.clone(),
                ..Default::default()
            },
        )
    }
    .map_err(|error| Problem::new("create the editor's graphics device", format!("{error:?}")))?;

    Ok((device, queue, enabled))
}

fn names(extensions: &[&std::ffi::CStr]) -> String {
    if extensions.is_empty() {
        return "(none)".to_string();
    }
    extensions
        .iter()
        .map(|name| name.to_string_lossy().into_owned())
        .collect::<Vec<_>>()
        .join(" ")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_wanted_extensions_are_the_four_an_import_needs() {
        // A list that grew a fifth entry by accident would enable something nobody decided to
        // enable, and a list that lost one would fail at the first import rather than here.
        assert_eq!(WANTED_EXTENSIONS.len(), 4);
        assert!(WANTED_EXTENSIONS.contains(&ash::khr::external_semaphore_fd::NAME));
        assert!(WANTED_EXTENSIONS.contains(&ash::ext::image_drm_format_modifier::NAME));
    }

    #[test]
    fn the_patch_is_what_enables_external_semaphores() {
        // Needs a Vulkan device. Where there is none it says so and stops, in the manner
        // `tests/render/` established: a skipped GPU test that prints nothing is a test nobody
        // notices has stopped running.
        let Ok(patched) = Gpu::create(&crate::session::preferred_adapter(), true) else {
            eprintln!("skipped: this machine reports no Vulkan device");
            return;
        };
        assert!(
            patched.external_semaphores(),
            "with the extension pushed into the list, the device can import a timeline: {:?}",
            patched.notes
        );
        assert!(patched.semaphore_fd().is_ok());
        assert!(
            patched
                .notes
                .iter()
                .any(|note| note.contains("VK_KHR_external_semaphore_fd enabled")),
            "the answer is written down where a bug report will find it"
        );

        // The control, in the same binary: without the patch the extension is enabled only if wgpu
        // asked for it itself, and the entry points are unreachable when it did not. This is the
        // pair of runs that makes "the patch is what does it" a measurement.
        let unpatched = Gpu::create(&crate::session::preferred_adapter(), false)
            .expect("the same adapter opens without the patch");
        assert_eq!(
            unpatched.external_semaphores(),
            unpatched.semaphore_fd().is_ok(),
            "the capability and the entry points agree, always"
        );
        if !unpatched.external_semaphores() {
            let problem = unpatched
                .semaphore_fd()
                .err()
                .expect("refused, because the extension was not enabled");
            assert!(problem.remedy.is_some(), "{problem}");
        }
    }
}
