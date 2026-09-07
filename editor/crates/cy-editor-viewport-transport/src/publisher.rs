//! The reference publisher: the runtime's half of the viewport transport, in a real second process.
//!
//! It exists for three reasons, in the order they matter.
//!
//! **A transport with nothing on the other end has not been run.** Everything the editor's side
//! does — importing a ring, waiting on a timeline, claiming a slot, releasing it — is only true if
//! something across a process boundary does the other half, and a fixture in the same process
//! cannot prove it because it shares an address space, a device and a scheduler.
//!
//! **It is the executable description of what a publisher must do.** The engine's render server
//! will grow a publisher of its own against this same wire format; this file is what it has to
//! match, including the ordering that a paragraph would state and a compiler would not check.
//!
//! **It is what can be killed.** Six SIGKILL runs of the spike's publisher were survived by the
//! editor. Reproducing that needs a process to kill.
//!
//! Usage:
//!   cy-viewport-publisher [--socket PATH] [--width W] [--height H] [--buffers N]
//!                         [--frames N] [--seconds S] [--rate HZ] [--heavy K]
//!                         [--on-full drop|block] [--adapter SUBSTRING]

use std::os::fd::{AsRawFd, FromRawFd, OwnedFd, RawFd};
use std::sync::atomic::{AtomicBool, Ordering};

use crate::announce::{Announcement, AnnouncementPage};
use crate::ring::{FullRingPolicy, PREFERRED_BUFFERS, Ring, claim_names};
use crate::session::monotonic_nanos;
use crate::wire::{
    FOURCC_ABGR8888, Handshake, PlaneDescription, default_socket_path, peer_closed,
    send_descriptors,
};
use ash::{Device, Entry, Instance, vk};

/// The image format, and the one the editor imports as `Rgba8Unorm`.
const FORMAT: vk::Format = vk::Format::R8G8B8A8_UNORM;
/// How many command buffers are in flight. Unrelated to the ring: this is the publisher's own
/// pipelining, and conflating the two is how a "synchronisation bug" turns out to be a staging
/// buffer rewritten while the GPU was still reading it.
const IN_FLIGHT: usize = 4;
/// The same number, for the modulo that chooses this frame's command buffer.
const IN_FLIGHT_FRAMES: u64 = 4;
/// The moving bar's width in pixels.
const BAR_WIDTH: u32 = 96;

const USAGE: vk::ImageUsageFlags = vk::ImageUsageFlags::from_raw(
    vk::ImageUsageFlags::TRANSFER_SRC.as_raw()
        | vk::ImageUsageFlags::TRANSFER_DST.as_raw()
        | vk::ImageUsageFlags::SAMPLED.as_raw()
        | vk::ImageUsageFlags::COLOR_ATTACHMENT.as_raw(),
);

static STOP: AtomicBool = AtomicBool::new(false);

extern "C" fn on_signal(_: libc::c_int) {
    STOP.store(true, Ordering::Relaxed);
}

struct Options {
    socket: String,
    adapter: String,
    width: u32,
    height: u32,
    buffers: usize,
    frames: u64,
    seconds: f64,
    rate: f64,
    heavy: u32,
    policy: FullRingPolicy,
}

/// [`PREFERRED_BUFFERS`] as a number a command line can carry.
fn preferred_buffers() -> u32 {
    u32::try_from(PREFERRED_BUFFERS).expect("a ring holds at most four images")
}

fn parse_options() -> Options {
    let arguments: Vec<String> = std::env::args().collect();
    let value = |key: &str| {
        arguments
            .iter()
            .position(|argument| argument == key)
            .and_then(|index| arguments.get(index + 1))
            .cloned()
    };
    let number = |key: &str, fallback: f64| {
        value(key)
            .and_then(|text| text.parse::<f64>().ok())
            .unwrap_or(fallback)
    };
    #[allow(
        clippy::cast_possible_truncation,
        clippy::cast_sign_loss,
        reason = "command-line numbers, clamped where they matter"
    )]
    Options {
        socket: value("--socket").unwrap_or_else(default_socket_path),
        adapter: value("--adapter").unwrap_or_default(),
        width: number("--width", 1920.0) as u32,
        height: number("--height", 1080.0) as u32,
        buffers: (number("--buffers", f64::from(preferred_buffers())) as usize).clamp(1, 4),
        frames: number("--frames", 0.0) as u64,
        seconds: number("--seconds", 0.0),
        rate: number("--rate", 0.0),
        heavy: number("--heavy", 0.0) as u32,
        policy: if value("--on-full").as_deref() == Some("block") {
            FullRingPolicy::Block
        } else {
            FullRingPolicy::Drop
        },
    }
}

struct Context {
    _entry: Entry,
    instance: Instance,
    physical: vk::PhysicalDevice,
    device: Device,
    queue: vk::Queue,
    queue_family: u32,
    memory: vk::PhysicalDeviceMemoryProperties,
    semaphore_fd: ash::khr::external_semaphore_fd::Device,
    width: u32,
    height: u32,
}

struct SharedImage {
    image: vk::Image,
    _memory: vk::DeviceMemory,
    plane: PlaneDescription,
    descriptor: OwnedFd,
}

/// Run the reference publisher, as `cy-viewport-publisher` does.
pub fn main() {
    // SAFETY: installing signal handlers for a process this program owns. `on_signal` touches only
    // an atomic, which is what a handler is permitted to do.
    unsafe {
        libc::signal(libc::SIGPIPE, libc::SIG_IGN);
        libc::signal(libc::SIGTERM, on_signal as *const () as usize);
        libc::signal(libc::SIGINT, on_signal as *const () as usize);
    }
    let options = parse_options();
    let context = create_context(&options);
    let mut ring = Ring::new(options.buffers, options.policy).expect("a ring");
    if let Some(advisory) = ring.advisory() {
        eprintln!("[publisher] warning: {advisory}");
    }

    let images = create_ring_images(&context, &options);
    let handshake = describe(&context, &options, &ring, &images);
    let render_done = create_timeline(&context);
    let release = create_timeline(&context);
    let page = AnnouncementPage::create().expect("an announcement page");
    let listener = listen(&options);
    let Recording {
        commands,
        fences,
        bar,
    } = create_recording(&context);

    let mut client: Option<std::os::unix::net::UnixStream> = None;
    let mut frame_id: u64 = 0;
    let mut dropped_report_at = std::time::Instant::now();
    let started = std::time::Instant::now();
    let mut next_due = started;

    loop {
        service_client(
            &mut client,
            &listener,
            &handshake,
            &images,
            &context,
            render_done,
            release,
            &page,
            &mut ring,
        );

        pace(options.rate, &mut next_due);

        let released = timeline_value(&context, release);
        let Reservation::Slot(slot) = reserve(&mut ring, &page, released, &mut dropped_report_at)
        else {
            if should_stop(&options, started, frame_id) {
                break;
            }
            continue;
        };

        frame_id += 1;
        let in_flight = usize::try_from(frame_id % IN_FLIGHT_FRAMES).expect("a small index");
        wait_and_reset(&context, fences[in_flight]);
        record(
            &context,
            commands[in_flight],
            &images[slot],
            ring.slot(slot).is_some_and(|entry| entry.initialised),
            bar.1,
            frame_id,
            options.heavy,
        );
        let submitted_nanos = monotonic_nanos();
        submit(
            &context,
            commands[in_flight],
            fences[in_flight],
            render_done,
            release,
            ring.release_requirement(slot),
            frame_id,
        );
        ring.record_published(slot, frame_id);

        // ANNOUNCE AFTER `vkQueueSubmit`, NEVER BEFORE.
        //
        // This ordering is the whole reason a killed runtime is survivable. By the time the editor
        // can see `frame_id`, the work that signals `render_done` to that value is already on a
        // queue, so the driver will signal it even if this process is destroyed in the next
        // instant — six SIGKILL runs of the spike survived on exactly this, and the editor froze on
        // the last complete frame instead of waiting forever.
        //
        // Announce first and a kill in between leaves the editor waiting on a value nothing will
        // ever reach. With the bounded host wait that is a viewport that never updates again; with
        // a wait staged on the editor's queue it is an editor that cannot be closed.
        page.publish(Announcement {
            frame_id,
            slot: u32::try_from(slot).expect("a slot"),
            generation: ring.generation(),
            timeline_value: frame_id,
            submitted_nanos,
        });
        // The write is announced, so the slot is no longer reserved against the editor — it is now
        // the frame the editor most wants to claim. Clearing after the announcement rather than
        // before is what makes the newest frame claimable at all.
        page.set_writing(None);

        if should_stop(&options, started, frame_id) {
            break;
        }
    }

    finish(&context, &ring, &options, frame_id, started);
}

/// Wait for the device, say what happened, and take the socket away.
#[allow(
    clippy::cast_precision_loss,
    reason = "a frame count turned into a rate, for a report"
)]
fn finish(
    context: &Context,
    ring: &Ring,
    options: &Options,
    frame_id: u64,
    started: std::time::Instant,
) {
    // SAFETY: waiting for this device's own submissions to finish before its objects are dropped.
    unsafe { context.device.device_wait_idle() }.expect("idle");
    let seconds = started.elapsed().as_secs_f64();
    println!(
        "[publisher] published {frame_id} frames in {seconds:.2} s = {:.1} fps, dropped {} on a \
         full ring, {} vetoed by the editor's claim",
        frame_id as f64 / seconds,
        ring.dropped(),
        ring.vetoed()
    );
    let _ = std::fs::remove_file(&options.socket);
}

/// The ring's images, each with its own exported dma-buf descriptor.
fn create_ring_images(context: &Context, options: &Options) -> Vec<SharedImage> {
    (0..options.buffers)
        .map(|_| create_exported_image(context))
        .collect()
}

/// The handshake that describes those images, and the line in the log that repeats it.
///
/// The numbers printed here are the ones an import failure is diagnosed with, which is why they are
/// printed at all: "stride 7680, allocation 8847360" against a 1920x1080 image says immediately
/// that the allocation is not `width * height * 4`, and that is the first thing to check.
fn describe(
    context: &Context,
    options: &Options,
    ring: &Ring,
    images: &[SharedImage],
) -> Handshake {
    let modifier = chosen_modifier(context, images[0].image);
    let mut handshake = Handshake {
        width: options.width,
        height: options.height,
        fourcc: FOURCC_ABGR8888,
        buffer_count: u32::try_from(options.buffers).expect("at most four"),
        generation: ring.generation(),
        has_timelines: 1,
        modifier,
        ..Handshake::default()
    };
    for (slot, image) in images.iter().enumerate() {
        handshake.planes[slot] = image.plane;
    }
    handshake.validate().expect("we described our own ring");
    println!(
        "[publisher] {}  {}x{}  {} image(s)  modifier 0x{modifier:x}  stride {}  allocation {} B",
        adapter_name(context),
        options.width,
        options.height,
        options.buffers,
        images[0].plane.stride,
        images[0].plane.allocation_bytes
    );
    handshake
}

fn listen(options: &Options) -> std::os::unix::net::UnixListener {
    let _ = std::fs::remove_file(&options.socket);
    let listener = std::os::unix::net::UnixListener::bind(&options.socket).expect("bind");
    listener.set_nonblocking(true).expect("non-blocking");
    println!("[publisher] listening on {}", options.socket);
    listener
}

/// The publisher's own pipelining: one command buffer and one fence per frame in flight, and the
/// bar it copies. Nothing here crosses the process boundary.
struct Recording {
    commands: Vec<vk::CommandBuffer>,
    fences: Vec<vk::Fence>,
    bar: (vk::DeviceMemory, vk::Buffer),
}

fn create_recording(context: &Context) -> Recording {
    let pool = create_pool(context);
    Recording {
        commands: (0..IN_FLIGHT)
            .map(|_| allocate_command(context, pool))
            .collect(),
        fences: (0..IN_FLIGHT).map(|_| create_fence(context)).collect(),
        bar: create_bar(context),
    }
}

/// Accept an editor, or notice that the one we had has gone.
///
/// A disconnected editor gets the ring back immediately. Without that, a three-image ring is
/// exhausted within three frames by claims nobody will ever release, and the runtime stops
/// rendering because something else stopped watching.
#[allow(
    clippy::too_many_arguments,
    reason = "the publisher's state, passed rather than gathered into a struct that would exist \
              only to satisfy this lint"
)]
fn service_client(
    client: &mut Option<std::os::unix::net::UnixStream>,
    listener: &std::os::unix::net::UnixListener,
    handshake: &Handshake,
    images: &[SharedImage],
    context: &Context,
    render_done: vk::Semaphore,
    release: vk::Semaphore,
    page: &AnnouncementPage,
    ring: &mut Ring,
) {
    match client.as_ref() {
        None => {
            if let Ok((stream, _)) = listener.accept()
                && hand_over(
                    &stream,
                    handshake,
                    images,
                    context,
                    render_done,
                    release,
                    page,
                )
            {
                println!("[publisher] handed the ring to an editor");
                stream.set_nonblocking(true).expect("non-blocking");
                *client = Some(stream);
            }
        }
        Some(stream) => {
            if peer_closed(std::os::fd::AsFd::as_fd(stream)) {
                println!("[publisher] the editor disconnected; reclaiming the ring");
                *client = None;
                ring.reclaim();
                page.set_held(0);
            }
        }
    }
}

/// Whether this frame has an image to go into.
enum Reservation {
    /// It does, and the editor has been told which one is being written.
    Slot(usize),
    /// Every image is spoken for, or the editor claimed the one chosen. Either way the frame is
    /// gone, which is the runtime's right and never the editor's problem.
    Dropped,
}

/// Choose an image for this frame and reserve it against the editor's claim.
///
/// Two looks at the editor's claim, and both are needed. The first, in `observe_held`, is what
/// keeps the chosen slot from being one the editor is reading. The second, after declaring the
/// slot, closes the window the first cannot: the editor is entitled to claim a slot between the
/// choice and the declaration, and a claim confirmed in that window is a claim on an image already
/// being overwritten — measured at 2.7% of frames on a three-image ring with the runtime
/// free-running.
fn reserve(
    ring: &mut Ring,
    page: &AnnouncementPage,
    released: u64,
    reported_at: &mut std::time::Instant,
) -> Reservation {
    ring.observe_held(page.held());
    let Some(slot) = ring.pick(released) else {
        // THE EDITOR MUST NEVER THROTTLE THE RUNTIME. A full ring is this frame's problem, not the
        // editor's: drop it, count it, and carry on at full rate.
        ring.drop_frame();
        if ring.policy() == FullRingPolicy::Block {
            std::thread::yield_now();
        }
        if reported_at.elapsed().as_secs() >= 5 {
            println!(
                "[publisher] dropped {} frames on a full ring",
                ring.dropped()
            );
            *reported_at = std::time::Instant::now();
        }
        return Reservation::Dropped;
    };
    page.set_writing(Some(u32::try_from(slot).expect("a slot")));
    if claim_names(page.held(), slot) {
        page.set_writing(None);
        ring.veto_frame();
        return Reservation::Dropped;
    }
    Reservation::Slot(slot)
}

/// Sleep until the next frame is due, when a rate was asked for.
///
/// A publisher with no `--rate` runs flat out, which is what the measurements want; an editor
/// session usually wants a fixed rate so that "the editor showed 97% of frames" is a statement
/// about the transport rather than about a scheduler.
fn pace(rate: f64, next_due: &mut std::time::Instant) {
    if rate <= 0.0 {
        return;
    }
    *next_due += std::time::Duration::from_secs_f64(1.0 / rate);
    let now = std::time::Instant::now();
    if *next_due > now {
        std::thread::sleep(*next_due - now);
    } else {
        *next_due = now;
    }
}

fn should_stop(options: &Options, started: std::time::Instant, frame_id: u64) -> bool {
    STOP.load(Ordering::Relaxed)
        || (options.frames > 0 && frame_id >= options.frames)
        || (options.seconds > 0.0 && started.elapsed().as_secs_f64() > options.seconds)
}

/// Send the handshake and every descriptor in one message.
fn hand_over(
    stream: &std::os::unix::net::UnixStream,
    handshake: &Handshake,
    images: &[SharedImage],
    context: &Context,
    render_done: vk::Semaphore,
    release: vk::Semaphore,
    page: &AnnouncementPage,
) -> bool {
    let exported = [
        export_semaphore(context, render_done),
        export_semaphore(context, release),
    ];
    let mut descriptors: Vec<RawFd> = images
        .iter()
        .map(|image| image.descriptor.as_raw_fd())
        .collect();
    descriptors.push(exported[0].as_raw_fd());
    descriptors.push(exported[1].as_raw_fd());
    descriptors.push(page.descriptor().as_raw_fd());
    // The order is the order the editor pops them in: images in slot order, then the two timelines,
    // then the announcement page. A different order shows the wrong slot for every frame.
    match send_descriptors(stream.as_raw_fd(), &descriptors, handshake.as_bytes()) {
        Ok(()) => true,
        Err(problem) => {
            eprintln!("[publisher] handshake failed: {problem}");
            false
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Vulkan
// ---------------------------------------------------------------------------------------------

fn create_context(options: &Options) -> Context {
    // SAFETY: the Vulkan loader, an instance, a device and a queue, each created from information
    // built immediately above it and each checked. This is the standard start-up sequence; every
    // handle is owned by this process for its whole life.
    unsafe {
        let entry = Entry::load().expect("the Vulkan loader");
        let application = vk::ApplicationInfo::default()
            .api_version(vk::API_VERSION_1_3)
            .application_name(c"cy-viewport-publisher");
        let instance = entry
            .create_instance(
                &vk::InstanceCreateInfo::default().application_info(&application),
                None,
            )
            .expect("a Vulkan instance");
        let devices = instance
            .enumerate_physical_devices()
            .expect("physical devices");
        let physical = devices
            .iter()
            .copied()
            .find(|&device| {
                instance
                    .get_physical_device_properties(device)
                    .device_name_as_c_str()
                    .is_ok_and(|name| name.to_string_lossy().contains(&options.adapter))
            })
            .unwrap_or_else(|| *devices.first().expect("a physical device"));
        let queue_family = u32::try_from(
            instance
                .get_physical_device_queue_family_properties(physical)
                .iter()
                .position(|family| family.queue_flags.contains(vk::QueueFlags::GRAPHICS))
                .expect("a graphics queue family"),
        )
        .expect("a small index");

        let priorities = [1.0_f32];
        let queues = [vk::DeviceQueueCreateInfo::default()
            .queue_family_index(queue_family)
            .queue_priorities(&priorities)];
        let extensions = [
            ash::khr::external_memory_fd::NAME.as_ptr(),
            ash::ext::external_memory_dma_buf::NAME.as_ptr(),
            ash::ext::image_drm_format_modifier::NAME.as_ptr(),
            ash::khr::external_semaphore_fd::NAME.as_ptr(),
            ash::khr::timeline_semaphore::NAME.as_ptr(),
        ];
        let mut timeline =
            vk::PhysicalDeviceTimelineSemaphoreFeatures::default().timeline_semaphore(true);
        let device = instance
            .create_device(
                physical,
                &vk::DeviceCreateInfo::default()
                    .queue_create_infos(&queues)
                    .enabled_extension_names(&extensions)
                    .push_next(&mut timeline),
                None,
            )
            .expect("a Vulkan device");
        let queue = device.get_device_queue(queue_family, 0);
        let memory = instance.get_physical_device_memory_properties(physical);
        let semaphore_fd = ash::khr::external_semaphore_fd::Device::new(&instance, &device);
        Context {
            _entry: entry,
            instance,
            physical,
            device,
            queue,
            queue_family,
            memory,
            semaphore_fd,
            width: options.width,
            height: options.height,
        }
    }
}

fn adapter_name(context: &Context) -> String {
    // SAFETY: querying properties of a physical device this process enumerated.
    unsafe {
        context
            .instance
            .get_physical_device_properties(context.physical)
    }
    .device_name_as_c_str()
    .map(|name| name.to_string_lossy().into_owned())
    .unwrap_or_default()
}

fn create_timeline(context: &Context) -> vk::Semaphore {
    let mut kind = vk::SemaphoreTypeCreateInfo::default()
        .semaphore_type(vk::SemaphoreType::TIMELINE)
        .initial_value(0);
    // OPAQUE_FD, not SYNC_FD: SYNC_FD is binary-only and cannot carry a timeline. Asking for it
    // here fails at creation, which is at least honest; assuming it works is not.
    let mut export = vk::ExportSemaphoreCreateInfo::default()
        .handle_types(vk::ExternalSemaphoreHandleTypeFlags::OPAQUE_FD);
    // SAFETY: creating a semaphore on this process's device from information that outlives the
    // call.
    unsafe {
        context.device.create_semaphore(
            &vk::SemaphoreCreateInfo::default()
                .push_next(&mut kind)
                .push_next(&mut export),
            None,
        )
    }
    .expect("an exportable timeline semaphore")
}

fn export_semaphore(context: &Context, semaphore: vk::Semaphore) -> OwnedFd {
    // SAFETY: exporting a semaphore this process created as exportable; the returned descriptor is
    // owned here and closed when the returned `OwnedFd` is dropped.
    let raw = unsafe {
        context.semaphore_fd.get_semaphore_fd(
            &vk::SemaphoreGetFdInfoKHR::default()
                .semaphore(semaphore)
                .handle_type(vk::ExternalSemaphoreHandleTypeFlags::OPAQUE_FD),
        )
    }
    .expect("vkGetSemaphoreFdKHR");
    // SAFETY: `raw` is a fresh descriptor nothing else owns.
    unsafe { OwnedFd::from_raw_fd(raw) }
}

fn timeline_value(context: &Context, semaphore: vk::Semaphore) -> u64 {
    // SAFETY: reading the counter of a timeline created on this device.
    unsafe { context.device.get_semaphore_counter_value(semaphore) }.unwrap_or(0)
}

/// Every single-plane DRM modifier this device can both render to and export.
fn usable_modifiers(context: &Context) -> Vec<u64> {
    // SAFETY: the two-pass format query the extension documents — once for the count, once for the
    // values, with a buffer sized from the first answer.
    let properties = unsafe {
        let mut list = vk::DrmFormatModifierPropertiesListEXT::default();
        let mut format = vk::FormatProperties2::default().push_next(&mut list);
        context.instance.get_physical_device_format_properties2(
            context.physical,
            FORMAT,
            &mut format,
        );
        let mut buffer = vec![
            vk::DrmFormatModifierPropertiesEXT::default();
            list.drm_format_modifier_count as usize
        ];
        let mut list = vk::DrmFormatModifierPropertiesListEXT::default()
            .drm_format_modifier_properties(&mut buffer);
        let mut format = vk::FormatProperties2::default().push_next(&mut list);
        context.instance.get_physical_device_format_properties2(
            context.physical,
            FORMAT,
            &mut format,
        );
        buffer
    };
    properties
        .iter()
        .filter(|candidate| modifier_is_usable(context, candidate))
        .map(|candidate| candidate.drm_format_modifier)
        .collect()
}

fn modifier_is_usable(context: &Context, candidate: &vk::DrmFormatModifierPropertiesEXT) -> bool {
    if candidate.drm_format_modifier_plane_count != 1 {
        return false;
    }
    let needed = vk::FormatFeatureFlags::SAMPLED_IMAGE
        | vk::FormatFeatureFlags::COLOR_ATTACHMENT
        | vk::FormatFeatureFlags::TRANSFER_SRC
        | vk::FormatFeatureFlags::TRANSFER_DST;
    if !candidate
        .drm_format_modifier_tiling_features
        .contains(needed)
    {
        return false;
    }
    let mut drm = vk::PhysicalDeviceImageDrmFormatModifierInfoEXT::default()
        .drm_format_modifier(candidate.drm_format_modifier)
        .sharing_mode(vk::SharingMode::EXCLUSIVE);
    let mut external = vk::PhysicalDeviceExternalImageFormatInfo::default()
        .handle_type(vk::ExternalMemoryHandleTypeFlags::DMA_BUF_EXT);
    let information = vk::PhysicalDeviceImageFormatInfo2::default()
        .format(FORMAT)
        .ty(vk::ImageType::TYPE_2D)
        .tiling(vk::ImageTiling::DRM_FORMAT_MODIFIER_EXT)
        .usage(USAGE)
        .push_next(&mut drm)
        .push_next(&mut external);
    let mut external_properties = vk::ExternalImageFormatProperties::default();
    let mut properties = vk::ImageFormatProperties2::default().push_next(&mut external_properties);
    // SAFETY: a query with information that outlives the call, on this process's physical device.
    let supported = unsafe {
        context
            .instance
            .get_physical_device_image_format_properties2(
                context.physical,
                &information,
                &mut properties,
            )
    }
    .is_ok();
    supported
        && external_properties
            .external_memory_properties
            .external_memory_features
            .contains(vk::ExternalMemoryFeatureFlags::EXPORTABLE)
}

fn create_exported_image(context: &Context) -> SharedImage {
    let modifiers = usable_modifiers(context);
    assert!(
        !modifiers.is_empty(),
        "no single-plane DRM modifier on this device can be rendered to and exported"
    );
    let mut list =
        vk::ImageDrmFormatModifierListCreateInfoEXT::default().drm_format_modifiers(&modifiers);
    let mut external = vk::ExternalMemoryImageCreateInfo::default()
        .handle_types(vk::ExternalMemoryHandleTypeFlags::DMA_BUF_EXT);
    let information = vk::ImageCreateInfo::default()
        .image_type(vk::ImageType::TYPE_2D)
        .format(FORMAT)
        .extent(vk::Extent3D {
            width: context.width,
            height: context.height,
            depth: 1,
        })
        .mip_levels(1)
        .array_layers(1)
        .samples(vk::SampleCountFlags::TYPE_1)
        .tiling(vk::ImageTiling::DRM_FORMAT_MODIFIER_EXT)
        .usage(USAGE)
        .sharing_mode(vk::SharingMode::EXCLUSIVE)
        .initial_layout(vk::ImageLayout::UNDEFINED)
        .push_next(&mut list)
        .push_next(&mut external);

    // SAFETY: image creation, a dedicated exportable allocation sized by the driver's own
    // requirements, the bind, the subresource layout query and the descriptor export — each on
    // handles this process owns, in the order the extension requires.
    unsafe {
        let image = context
            .device
            .create_image(&information, None)
            .expect("an exportable image");
        let requirements = context.device.get_image_memory_requirements(image);
        let index = memory_type(
            context,
            requirements.memory_type_bits,
            vk::MemoryPropertyFlags::DEVICE_LOCAL,
        );
        let mut dedicated = vk::MemoryDedicatedAllocateInfo::default().image(image);
        let mut export = vk::ExportMemoryAllocateInfo::default()
            .handle_types(vk::ExternalMemoryHandleTypeFlags::DMA_BUF_EXT);
        let memory = context
            .device
            .allocate_memory(
                &vk::MemoryAllocateInfo::default()
                    // THE ALLOCATION'S SIZE, from the driver. Not width * height * 4: the modifier's
                    // tiling and alignment make them different numbers, and the editor imports
                    // against this one.
                    .allocation_size(requirements.size)
                    .memory_type_index(index)
                    .push_next(&mut export)
                    .push_next(&mut dedicated),
                None,
            )
            .expect("exportable device memory");
        context
            .device
            .bind_image_memory(image, memory, 0)
            .expect("bound");
        let layout = context.device.get_image_subresource_layout(
            image,
            vk::ImageSubresource {
                aspect_mask: vk::ImageAspectFlags::MEMORY_PLANE_0_EXT,
                mip_level: 0,
                array_layer: 0,
            },
        );
        let memory_fd =
            ash::khr::external_memory_fd::Device::new(&context.instance, &context.device);
        let raw = memory_fd
            .get_memory_fd(
                &vk::MemoryGetFdInfoKHR::default()
                    .memory(memory)
                    .handle_type(vk::ExternalMemoryHandleTypeFlags::DMA_BUF_EXT),
            )
            .expect("a dma-buf descriptor");
        SharedImage {
            image,
            _memory: memory,
            plane: PlaneDescription {
                stride: layout.row_pitch,
                offset: layout.offset,
                allocation_bytes: requirements.size,
            },
            descriptor: OwnedFd::from_raw_fd(raw),
        }
    }
}

fn chosen_modifier(context: &Context, image: vk::Image) -> u64 {
    let device =
        ash::ext::image_drm_format_modifier::Device::new(&context.instance, &context.device);
    let mut chosen = vk::ImageDrmFormatModifierPropertiesEXT::default();
    // SAFETY: asking the driver which modifier it picked for an image this process created from a
    // list of candidates.
    unsafe { device.get_image_drm_format_modifier_properties(image, &mut chosen) }
        .expect("the chosen modifier");
    chosen.drm_format_modifier
}

fn memory_type(context: &Context, bits: u32, flags: vk::MemoryPropertyFlags) -> u32 {
    for (index, kind) in context.memory.memory_types_as_slice().iter().enumerate() {
        let index = u32::try_from(index).expect("a small index");
        if bits & (1 << index) != 0 && kind.property_flags.contains(flags) {
            return index;
        }
    }
    panic!("no memory type with {flags:?}");
}

fn create_pool(context: &Context) -> vk::CommandPool {
    // SAFETY: a command pool on this process's device.
    unsafe {
        context.device.create_command_pool(
            &vk::CommandPoolCreateInfo::default()
                .queue_family_index(context.queue_family)
                .flags(vk::CommandPoolCreateFlags::RESET_COMMAND_BUFFER),
            None,
        )
    }
    .expect("a command pool")
}

fn allocate_command(context: &Context, pool: vk::CommandPool) -> vk::CommandBuffer {
    // SAFETY: allocating one primary command buffer from a pool this process created.
    unsafe {
        context.device.allocate_command_buffers(
            &vk::CommandBufferAllocateInfo::default()
                .command_pool(pool)
                .level(vk::CommandBufferLevel::PRIMARY)
                .command_buffer_count(1),
        )
    }
    .expect("a command buffer")[0]
}

fn create_fence(context: &Context) -> vk::Fence {
    // SAFETY: a signalled fence on this process's device, so the first wait returns immediately.
    unsafe {
        context.device.create_fence(
            &vk::FenceCreateInfo::default().flags(vk::FenceCreateFlags::SIGNALED),
            None,
        )
    }
    .expect("a fence")
}

fn wait_and_reset(context: &Context, fence: vk::Fence) {
    // SAFETY: waiting on and resetting a fence this process created, before reusing the command
    // buffer it guards. This is the publisher's own pipelining and has nothing to do with the
    // editor.
    unsafe {
        context
            .device
            .wait_for_fences(&[fence], true, u64::MAX)
            .expect("the fence");
        context.device.reset_fences(&[fence]).expect("reset");
    }
}

/// A white column, uploaded once and copied to a moving position each frame, so that motion is
/// visible in a screenshot and a frozen viewport is obvious at a glance.
fn create_bar(context: &Context) -> (vk::DeviceMemory, vk::Buffer) {
    let bytes = u64::from(BAR_WIDTH) * u64::from(context.height) * 4;
    // SAFETY: a host-visible buffer created, allocated, bound, mapped, filled and unmapped, all on
    // handles this process owns; the write is exactly the mapped length.
    unsafe {
        let buffer = context
            .device
            .create_buffer(
                &vk::BufferCreateInfo::default()
                    .size(bytes)
                    .usage(vk::BufferUsageFlags::TRANSFER_SRC),
                None,
            )
            .expect("a staging buffer");
        let requirements = context.device.get_buffer_memory_requirements(buffer);
        let index = memory_type(
            context,
            requirements.memory_type_bits,
            vk::MemoryPropertyFlags::HOST_VISIBLE | vk::MemoryPropertyFlags::HOST_COHERENT,
        );
        let memory = context
            .device
            .allocate_memory(
                &vk::MemoryAllocateInfo::default()
                    .allocation_size(requirements.size)
                    .memory_type_index(index),
                None,
            )
            .expect("host-visible memory");
        context
            .device
            .bind_buffer_memory(buffer, memory, 0)
            .expect("bound");
        let mapped = context
            .device
            .map_memory(memory, 0, requirements.size, vk::MemoryMapFlags::empty())
            .expect("mapped")
            .cast::<u8>();
        std::ptr::write_bytes(mapped, 0xff, usize::try_from(bytes).expect("a small image"));
        context.device.unmap_memory(memory);
        (memory, buffer)
    }
}

/// The frame's background encodes its identity in the red and green channels, so that a reader
/// which sees two different backgrounds in one image has demonstrably read a torn frame — a
/// numeric check rather than an opinion about a screenshot.
fn tag_colour(frame_id: u64) -> vk::ClearColorValue {
    #[allow(
        clippy::cast_precision_loss,
        reason = "a byte converted to a unit float; both are exact"
    )]
    vk::ClearColorValue {
        float32: [
            (frame_id & 0xff) as f32 / 255.0,
            ((frame_id >> 8) & 0xff) as f32 / 255.0,
            64.0 / 255.0,
            1.0,
        ],
    }
}

const COLOUR_RANGE: vk::ImageSubresourceRange = vk::ImageSubresourceRange {
    aspect_mask: vk::ImageAspectFlags::COLOR,
    base_mip_level: 0,
    level_count: 1,
    base_array_layer: 0,
    layer_count: 1,
};

fn record(
    context: &Context,
    command: vk::CommandBuffer,
    target: &SharedImage,
    initialised: bool,
    bar: vk::Buffer,
    frame_id: u64,
    heavy: u32,
) {
    let from = if initialised {
        vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL
    } else {
        vk::ImageLayout::UNDEFINED
    };
    // SAFETY: recording into a command buffer this process allocated and is not submitting, against
    // an image it created. The layouts are the ones the protocol states: the editor is handed every
    // image in SHADER_READ_ONLY_OPTIMAL.
    unsafe {
        context
            .device
            .begin_command_buffer(command, &vk::CommandBufferBeginInfo::default())
            .expect("began");
        barrier(
            context,
            command,
            target.image,
            from,
            vk::ImageLayout::TRANSFER_DST_OPTIMAL,
        );
        // `--heavy K` widens the write window with K full-image clears carrying the PREVIOUS
        // frame's tag, so that an unsynchronised reader sees two valid tags in one image rather
        // than a colour no frame ever had. It is how the tear check is given something to find.
        for _ in 0..heavy {
            context.device.cmd_clear_color_image(
                command,
                target.image,
                vk::ImageLayout::TRANSFER_DST_OPTIMAL,
                &tag_colour(frame_id.saturating_sub(1)),
                std::slice::from_ref(&COLOUR_RANGE),
            );
        }
        context.device.cmd_clear_color_image(
            command,
            target.image,
            vk::ImageLayout::TRANSFER_DST_OPTIMAL,
            &tag_colour(frame_id),
            std::slice::from_ref(&COLOUR_RANGE),
        );
        let x = i32::try_from((frame_id * 13) % u64::from(context.width - BAR_WIDTH))
            .expect("inside the image");
        let region = vk::BufferImageCopy::default()
            .buffer_row_length(BAR_WIDTH)
            .buffer_image_height(context.height)
            .image_subresource(vk::ImageSubresourceLayers {
                aspect_mask: vk::ImageAspectFlags::COLOR,
                mip_level: 0,
                base_array_layer: 0,
                layer_count: 1,
            })
            .image_offset(vk::Offset3D { x, y: 0, z: 0 })
            .image_extent(vk::Extent3D {
                width: BAR_WIDTH,
                height: context.height,
                depth: 1,
            });
        context.device.cmd_copy_buffer_to_image(
            command,
            bar,
            target.image,
            vk::ImageLayout::TRANSFER_DST_OPTIMAL,
            std::slice::from_ref(&region),
        );
        barrier(
            context,
            command,
            target.image,
            vk::ImageLayout::TRANSFER_DST_OPTIMAL,
            vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL,
        );
        context.device.end_command_buffer(command).expect("ended");
    }
}

fn barrier(
    context: &Context,
    command: vk::CommandBuffer,
    image: vk::Image,
    from: vk::ImageLayout,
    to: vk::ImageLayout,
) {
    let barrier = vk::ImageMemoryBarrier::default()
        .old_layout(from)
        .new_layout(to)
        .src_queue_family_index(vk::QUEUE_FAMILY_IGNORED)
        .dst_queue_family_index(vk::QUEUE_FAMILY_IGNORED)
        .image(image)
        .subresource_range(COLOUR_RANGE)
        .src_access_mask(vk::AccessFlags::MEMORY_WRITE | vk::AccessFlags::MEMORY_READ)
        .dst_access_mask(vk::AccessFlags::MEMORY_WRITE | vk::AccessFlags::MEMORY_READ);
    // SAFETY: recording a barrier into a command buffer being recorded, against an image this
    // process owns.
    unsafe {
        context.device.cmd_pipeline_barrier(
            command,
            vk::PipelineStageFlags::ALL_COMMANDS,
            vk::PipelineStageFlags::ALL_COMMANDS,
            vk::DependencyFlags::empty(),
            &[],
            &[],
            std::slice::from_ref(&barrier),
        );
    }
}

/// Wait for the editor to have finished with whatever this slot held, then signal that this frame
/// is complete.
///
/// Both halves are needed. Without the wait, the runtime overwrites a frame the editor is still
/// sampling; without the signal, the editor has nothing to wait on and shows torn images.
fn submit(
    context: &Context,
    command: vk::CommandBuffer,
    fence: vk::Fence,
    render_done: vk::Semaphore,
    release: vk::Semaphore,
    needs_release: u64,
    frame_id: u64,
) {
    let commands = [command];
    let mut waits: Vec<vk::Semaphore> = Vec::new();
    let mut wait_values: Vec<u64> = Vec::new();
    let mut stages: Vec<vk::PipelineStageFlags> = Vec::new();
    if needs_release > 0 {
        waits.push(release);
        wait_values.push(needs_release);
        stages.push(vk::PipelineStageFlags::TRANSFER);
    }
    let signals = [render_done];
    let signal_values = [frame_id];
    let mut timeline = vk::TimelineSemaphoreSubmitInfo::default()
        .wait_semaphore_values(&wait_values)
        .signal_semaphore_values(&signal_values);
    let information = vk::SubmitInfo::default()
        .command_buffers(&commands)
        .wait_semaphores(&waits)
        .wait_dst_stage_mask(&stages)
        .signal_semaphores(&signals)
        .push_next(&mut timeline);
    // SAFETY: one submission of one command buffer this process recorded, with semaphores it owns
    // or imported, on its own queue. Every slice outlives the call.
    unsafe {
        context
            .device
            .queue_submit(context.queue, std::slice::from_ref(&information), fence)
    }
    .expect("submitted");
}
