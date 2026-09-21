// SPDX-License-Identifier: MIT
//! Native macOS viewport transport: IOSurface-backed Metal textures and a shared announcement page.
//!
//! The producer publishes only after its Metal upload completes. The editor holds the sampled slot
//! in the shared page until its own wgpu queue has completed, so neither process can reuse storage
//! while the other GPU is touching it. The wait is bounded; a late GPU repeats the prior frame.

use std::os::fd::{AsFd, AsRawFd, FromRawFd, OwnedFd};
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::{Duration, Instant};

use cy_editor_core::problem::{Problem, Result};
use cy_editor_protocol::FrameId;
use cy_editor_viewport::state::ViewState;
use cy_editor_viewport::transport::{
    FrameImage, PresentedFrame, SharedImage, Transport, TransportKind,
};
use objc2_io_surface::IOSurfaceRef;
use objc2_metal::{
    MTLDevice, MTLPixelFormat, MTLStorageMode, MTLTextureDescriptor, MTLTextureType,
    MTLTextureUsage,
};

const MAX_BUFFERS: usize = 4;
const PROTOCOL_MAGIC: u32 = 0x4359_5650;
const PROTOCOL_VERSION: u32 = 1;
const PAGE_BYTES: usize = 4096;
const CLAIM_ATTEMPTS: usize = 8;
const GPU_PATIENCE: Duration = Duration::from_millis(2);
const HEARTBEAT_PATIENCE: Duration = Duration::from_secs(2);

/// `CLOCK_MONOTONIC` in nanoseconds, shared with the Metal publisher's frame timestamps.
#[must_use]
pub fn monotonic_nanos() -> u64 {
    let mut time = libc::timespec {
        tv_sec: 0,
        tv_nsec: 0,
    };
    // SAFETY: `clock_gettime` writes into a live, correctly typed stack value.
    unsafe { libc::clock_gettime(libc::CLOCK_MONOTONIC, &raw mut time) };
    let seconds = u64::try_from(time.tv_sec).unwrap_or(0);
    let nanos = u64::try_from(time.tv_nsec).unwrap_or(0);
    seconds * 1_000_000_000 + nanos
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct PlaneDescription {
    stride: u64,
    /// On macOS this field carries the IOSurface ID.
    surface_id: u64,
    allocation_bytes: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct Handshake {
    magic: u32,
    version: u32,
    width: u32,
    height: u32,
    fourcc: u32,
    buffer_count: u32,
    generation: u32,
    has_timelines: u32,
    modifier: u64,
    planes: [PlaneDescription; MAX_BUFFERS],
}

/// One completed frame described by the shared page.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Announcement {
    /// Monotonic identity assigned by the runtime.
    pub frame_id: u64,
    /// IOSurface ring slot containing the frame.
    pub slot: u32,
    /// Ring generation, changed on resize or restart.
    pub generation: u32,
    /// Reserved for transports with a GPU timeline.
    pub timeline_value: u64,
    /// Runtime monotonic clock at submission.
    pub submitted_nanos: u64,
}

#[repr(C)]
struct SharedState {
    sequence: AtomicU64,
    words: [AtomicU64; 4],
    heartbeat: AtomicU64,
    writing: AtomicU64,
    held: AtomicU64,
}

struct AnnouncementPage {
    _descriptor: OwnedFd,
    state: *mut SharedState,
}

impl AnnouncementPage {
    fn from_descriptor(descriptor: OwnedFd) -> Result<Self> {
        // SAFETY: maps one page from the shared-memory descriptor sent by the publisher. The
        // mapping is checked and retained until Drop.
        let state = unsafe {
            libc::mmap(
                std::ptr::null_mut(),
                PAGE_BYTES,
                libc::PROT_READ | libc::PROT_WRITE,
                libc::MAP_SHARED,
                descriptor.as_raw_fd(),
                0,
            )
        };
        if state == libc::MAP_FAILED {
            return Err(Problem::new(
                "map the runtime's viewport announcement page",
                std::io::Error::last_os_error().to_string(),
            ));
        }
        Ok(Self {
            _descriptor: descriptor,
            state: state.cast(),
        })
    }

    fn read(&self) -> Option<Announcement> {
        for _ in 0..64 {
            // SAFETY: `state` is a live shared mapping for this object's lifetime.
            let state = unsafe { &*self.state };
            let before = state.sequence.load(Ordering::Acquire);
            if before & 1 != 0 {
                continue;
            }
            let words = state
                .words
                .each_ref()
                .map(|word| word.load(Ordering::Relaxed));
            if state.sequence.load(Ordering::Acquire) != before || words[0] == 0 {
                continue;
            }
            return Some(Announcement {
                frame_id: words[0],
                slot: u32::try_from(words[1] & u64::from(u32::MAX))
                    .expect("the slot was masked to 32 bits"),
                generation: u32::try_from(words[1] >> 32).expect("the generation occupies 32 bits"),
                timeline_value: words[2],
                submitted_nanos: words[3],
            });
        }
        None
    }

    fn heartbeat(&self) -> u64 {
        // SAFETY: `state` is a live shared mapping for this object's lifetime.
        unsafe { &*self.state }.heartbeat.load(Ordering::Relaxed)
    }

    fn writing(&self) -> Option<u32> {
        // SAFETY: `state` is a live shared mapping for this object's lifetime.
        let value = unsafe { &*self.state }.writing.load(Ordering::SeqCst);
        (value != 0)
            .then(|| u32::try_from(value - 1).ok())
            .flatten()
    }

    fn hold(&self, frame: Option<Announcement>) {
        let packed = frame.map_or(0, |value| (value.frame_id << 8) | u64::from(value.slot));
        // SAFETY: `state` is a live shared mapping for this object's lifetime.
        unsafe { &*self.state }.held.store(packed, Ordering::SeqCst);
    }
}

impl Drop for AnnouncementPage {
    fn drop(&mut self) {
        self.hold(None);
        // SAFETY: this is the exact mapping created in `from_descriptor`, unmapped once.
        unsafe { libc::munmap(self.state.cast(), PAGE_BYTES) };
    }
}

/// Current health of the publisher process.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Liveness {
    /// Frames or heartbeats are arriving.
    Live,
    /// The publisher's socket reached EOF.
    Gone,
    /// The publisher remains connected but stopped advancing.
    Wedged,
}

impl Liveness {
    /// User-facing explanation of the state.
    #[must_use]
    pub const fn message(self) -> &'static str {
        match self {
            Self::Live => "The runtime is delivering frames.",
            Self::Gone => "The runtime disconnected; the last complete frame is retained.",
            Self::Wedged => "The runtime stopped publishing; the last complete frame is retained.",
        }
    }
}

/// Observable transport events for diagnostics and pacing reports.
#[derive(Clone, Copy, Debug, Default)]
pub struct SessionCounters {
    /// Valid newer frames observed.
    pub announced: u64,
    /// Announcements superseded before display.
    pub skipped: u64,
    /// Bounded waits that expired while retiring a sampled surface.
    pub timed_out: u64,
    /// Frames refused because their ring generation is obsolete.
    pub wrong_generation: u64,
    /// Frames refused because their slot is outside the ring.
    pub malformed: u64,
    /// Claims retried because the publisher advanced concurrently.
    pub claim_retries: u64,
    /// Claims abandoned because the producer was writing the same slot.
    pub claim_vetoed: u64,
}

/// One editor connection to one Metal runtime's IOSurface ring.
pub struct ViewportSession {
    handshake: Handshake,
    textures: Vec<wgpu::Texture>,
    views: Vec<wgpu::TextureView>,
    _surfaces: Vec<objc2_core_foundation::CFRetained<IOSurfaceRef>>,
    stream: std::os::unix::net::UnixStream,
    page: AnnouncementPage,
    view_state: ViewState,
    latest: Option<Announcement>,
    shown: Option<Announcement>,
    holding: Option<Announcement>,
    liveness: Liveness,
    heartbeat: u64,
    heartbeat_at: Instant,
    counters: SessionCounters,
    /// Time spent receiving and importing the IOSurface ring.
    pub import_millis: f64,
}

impl ViewportSession {
    /// Connect through `CY_VIEWPORT_SOCKET`, or the default viewport socket.
    pub fn connect(device: &wgpu::Device) -> Result<Self> {
        let path = std::env::var("CY_VIEWPORT_SOCKET")
            .unwrap_or_else(|_| "/tmp/cy-viewport.sock".to_owned());
        let stream = std::os::unix::net::UnixStream::connect(&path).map_err(|error| {
            Problem::new(
                "connect to the runtime's viewport",
                format!("{path}: {error}"),
            )
            .with_remedy("start the hosted runtime, or set CY_VIEWPORT_SOCKET")
        })?;
        Self::adopt(device, stream)
    }

    fn adopt(device: &wgpu::Device, stream: std::os::unix::net::UnixStream) -> Result<Self> {
        let started = Instant::now();
        let mut bytes = [0_u8; 1024];
        let (read, mut descriptors) = receive_descriptors(stream.as_raw_fd(), &mut bytes)?;
        if read < std::mem::size_of::<Handshake>() {
            return Err(Problem::new(
                "read the runtime's viewport handshake",
                format!(
                    "{read} bytes arrived; {} required",
                    std::mem::size_of::<Handshake>()
                ),
            ));
        }
        let handshake = unsafe { std::ptr::read_unaligned(bytes.as_ptr().cast::<Handshake>()) };
        validate_handshake(handshake)?;
        if descriptors.len() != 1 {
            return Err(Problem::new(
                "read the runtime's viewport handshake",
                format!(
                    "macOS expects one shared-page descriptor; {} arrived",
                    descriptors.len()
                ),
            ));
        }
        let page = AnnouncementPage::from_descriptor(descriptors.pop().expect("one descriptor"))?;
        let (textures, surfaces) = import_ring(device, &handshake)?;
        let views = textures
            .iter()
            .map(|texture| texture.create_view(&wgpu::TextureViewDescriptor::default()))
            .collect();
        stream.set_nonblocking(true).map_err(|error| {
            Problem::new("watch the runtime's viewport socket", error.to_string())
        })?;
        Ok(Self {
            handshake,
            textures,
            views,
            _surfaces: surfaces,
            stream,
            page,
            view_state: ViewState::new(),
            latest: None,
            shown: None,
            holding: None,
            liveness: Liveness::Live,
            heartbeat: 0,
            heartbeat_at: Instant::now(),
            counters: SessionCounters::default(),
            import_millis: started.elapsed().as_secs_f64() * 1e3,
        })
    }

    /// Texture view for a ring slot.
    #[must_use]
    pub fn view(&self, slot: usize) -> Option<&wgpu::TextureView> {
        self.views.get(slot)
    }

    /// Imported texture for a ring slot, used by the headless conformance probe.
    #[must_use]
    pub fn texture(&self, slot: usize) -> Option<&wgpu::Texture> {
        self.textures.get(slot)
    }

    /// Current publisher health.
    #[must_use]
    pub const fn liveness(&self) -> Liveness {
        self.liveness
    }

    /// Diagnostic counters accumulated by this session.
    #[must_use]
    pub const fn counters(&self) -> SessionCounters {
        self.counters
    }

    /// Associate subsequent presented frames with the view the editor requested.
    pub fn set_view_state(&mut self, state: ViewState) {
        self.view_state = state;
    }

    fn poll_page(&mut self) {
        if self.liveness != Liveness::Gone && peer_closed(self.stream.as_fd()) {
            self.liveness = Liveness::Gone;
        }
        let heartbeat = self.page.heartbeat();
        if heartbeat != self.heartbeat {
            self.heartbeat = heartbeat;
            self.heartbeat_at = Instant::now();
            self.liveness = Liveness::Live;
        } else if self.liveness == Liveness::Live
            && self.heartbeat_at.elapsed() > HEARTBEAT_PATIENCE
        {
            self.liveness = Liveness::Wedged;
        }
        let Some(frame) = self.page.read() else {
            return;
        };
        if frame.generation != self.handshake.generation {
            self.counters.wrong_generation += 1;
            return;
        }
        if frame.slot >= self.handshake.buffer_count {
            self.counters.malformed += 1;
            return;
        }
        if self.latest.is_none_or(|old| frame.frame_id > old.frame_id) {
            if self.latest.is_some() && self.latest != self.shown {
                self.counters.skipped += 1;
            }
            self.counters.announced += 1;
            self.latest = Some(frame);
        }
    }

    /// Claim the newest safe surface, retaining the previous one when a bounded wait expires.
    pub fn acquire(&mut self, device: &wgpu::Device) -> Option<(usize, Announcement)> {
        self.poll_page();
        let fresh = self.latest.filter(|frame| Some(*frame) != self.shown);
        let Some(mut candidate) = fresh else {
            return self.shown.map(|frame| (frame.slot as usize, frame));
        };

        if self.holding.is_some()
            && device
                .poll(wgpu::PollType::Wait {
                    submission_index: None,
                    timeout: Some(GPU_PATIENCE),
                })
                .is_err()
        {
            self.counters.timed_out += 1;
            return self.shown.map(|frame| (frame.slot as usize, frame));
        }

        for _ in 0..CLAIM_ATTEMPTS {
            self.page.hold(Some(candidate));
            if self.page.writing() == Some(candidate.slot) {
                self.counters.claim_vetoed += 1;
                self.poll_page();
                candidate = self.latest.unwrap_or(candidate);
                continue;
            }
            self.poll_page();
            match self.latest {
                Some(newest) if newest.frame_id == candidate.frame_id => {
                    self.holding = Some(candidate);
                    self.shown = Some(candidate);
                    return Some((candidate.slot as usize, candidate));
                }
                Some(newest) => {
                    self.counters.claim_retries += 1;
                    candidate = newest;
                }
                None => break,
            }
        }
        self.page.hold(self.holding);
        self.shown.map(|frame| (frame.slot as usize, frame))
    }

    fn presented(&self, frame: Announcement) -> PresentedFrame {
        let plane = self.handshake.planes[frame.slot as usize];
        PresentedFrame::new(
            FrameId::from_raw(frame.frame_id),
            self.view_state.clone(),
            FrameImage::SharedTexture {
                handle: plane.surface_id,
                image: SharedImage {
                    width: self.handshake.width,
                    height: self.handshake.height,
                    fourcc: self.handshake.fourcc,
                    modifier: 0,
                    stride: plane.stride,
                    offset: plane.surface_id,
                    allocation_bytes: plane.allocation_bytes,
                    slot: frame.slot,
                    buffer_count: self.handshake.buffer_count,
                    generation: frame.generation,
                    timeline_value: 0,
                },
            },
            frame.submitted_nanos / 1_000,
        )
    }
}

impl Transport for ViewportSession {
    fn kind(&self) -> TransportKind {
        TransportKind::SharedTexture
    }

    fn poll(&mut self) -> Option<PresentedFrame> {
        self.shown.map(|frame| self.presented(frame))
    }
}

fn validate_handshake(handshake: Handshake) -> Result<()> {
    if handshake.magic != PROTOCOL_MAGIC || handshake.version != PROTOCOL_VERSION {
        return Err(Problem::new(
            "read the runtime's viewport handshake",
            "the runtime and editor use different viewport protocols",
        ));
    }
    let count = handshake.buffer_count as usize;
    if count == 0 || count > MAX_BUFFERS || handshake.width == 0 || handshake.height == 0 {
        return Err(Problem::new(
            "read the runtime's viewport handshake",
            "the announced IOSurface ring has an invalid size",
        ));
    }
    if handshake.has_timelines != 0 {
        return Err(Problem::new(
            "read the runtime's viewport handshake",
            "the macOS protocol synchronises through completion and ownership, not file-descriptor timelines",
        ));
    }
    for plane in handshake.planes.iter().take(count) {
        if plane.surface_id > u64::from(u32::MAX)
            || plane.stride < u64::from(handshake.width) * 4
            || plane.allocation_bytes < plane.stride * u64::from(handshake.height)
        {
            return Err(Problem::new(
                "read the runtime's viewport handshake",
                "an IOSurface description cannot hold the announced image",
            ));
        }
    }
    Ok(())
}

type ImportedRing = (
    Vec<wgpu::Texture>,
    Vec<objc2_core_foundation::CFRetained<IOSurfaceRef>>,
);

fn import_ring(device: &wgpu::Device, handshake: &Handshake) -> Result<ImportedRing> {
    let raw_device = unsafe { device.as_hal::<wgpu_hal::api::Metal>() }.ok_or_else(|| {
        Problem::new(
            "import the IOSurface ring",
            "the editor device is not Metal",
        )
    })?;
    let mut textures = Vec::with_capacity(handshake.buffer_count as usize);
    let mut surfaces = Vec::with_capacity(handshake.buffer_count as usize);
    for plane in handshake
        .planes
        .iter()
        .take(handshake.buffer_count as usize)
    {
        let surface_id = u32::try_from(plane.surface_id)
            .expect("the handshake validator bounded every IOSurface ID");
        let surface = IOSurfaceRef::lookup(surface_id).ok_or_else(|| {
            Problem::new(
                "look up the runtime's IOSurface",
                format!("surface {} is no longer available", plane.surface_id),
            )
        })?;
        let descriptor = unsafe {
            MTLTextureDescriptor::texture2DDescriptorWithPixelFormat_width_height_mipmapped(
                MTLPixelFormat::RGBA8Unorm,
                handshake.width as usize,
                handshake.height as usize,
                false,
            )
        };
        descriptor.setStorageMode(MTLStorageMode::Shared);
        descriptor.setUsage(MTLTextureUsage::ShaderRead | MTLTextureUsage::RenderTarget);
        let raw_texture = raw_device
            .raw_device()
            .newTextureWithDescriptor_iosurface_plane(&descriptor, &surface, 0)
            .ok_or_else(|| {
                Problem::new(
                    "import the runtime's IOSurface",
                    "Metal refused the texture",
                )
            })?;
        let extent = wgpu::Extent3d {
            width: handshake.width,
            height: handshake.height,
            depth_or_array_layers: 1,
        };
        let hal_texture = unsafe {
            wgpu_hal::metal::Device::texture_from_raw(
                raw_texture,
                wgpu::TextureFormat::Rgba8Unorm,
                MTLTextureType::Type2D,
                1,
                1,
                wgpu_hal::CopyExtent {
                    width: handshake.width,
                    height: handshake.height,
                    depth: 1,
                },
                None,
            )
        };
        let texture_descriptor = wgpu::TextureDescriptor {
            label: Some("engine-viewport-iosurface"),
            size: extent,
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: wgpu::TextureFormat::Rgba8Unorm,
            usage: wgpu::TextureUsages::TEXTURE_BINDING
                | wgpu::TextureUsages::COPY_SRC
                | wgpu::TextureUsages::RENDER_ATTACHMENT,
            view_formats: &[],
        };
        textures.push(unsafe {
            device.create_texture_from_hal::<wgpu_hal::api::Metal>(
                hal_texture,
                &texture_descriptor,
                wgpu_types::TextureUses::RESOURCE | wgpu_types::TextureUses::COPY_SRC,
            )
        });
        surfaces.push(surface);
    }
    Ok((textures, surfaces))
}

#[allow(clippy::cast_ptr_alignment)]
fn receive_descriptors(socket: i32, buffer: &mut [u8]) -> Result<(usize, Vec<OwnedFd>)> {
    let mut descriptors = Vec::new();
    // SAFETY: all buffers outlive recvmsg; received descriptors are adopted exactly once.
    let read = unsafe {
        let mut iov = libc::iovec {
            iov_base: buffer.as_mut_ptr().cast(),
            iov_len: buffer.len(),
        };
        let mut control = [0_u8; 256];
        let mut message: libc::msghdr = std::mem::zeroed();
        message.msg_iov = &raw mut iov;
        message.msg_iovlen = 1;
        message.msg_control = control.as_mut_ptr().cast();
        message.msg_controllen = control
            .len()
            .try_into()
            .expect("the fixed control buffer fits socklen_t");
        let read = libc::recvmsg(socket, &raw mut message, 0);
        if read < 0 {
            return Err(Problem::new(
                "receive the viewport handshake",
                std::io::Error::last_os_error().to_string(),
            ));
        }
        let header = libc::CMSG_FIRSTHDR(&raw const message);
        if !header.is_null() && (*header).cmsg_type == libc::SCM_RIGHTS {
            let bytes = (*header).cmsg_len as usize - libc::CMSG_LEN(0) as usize;
            let count = bytes / std::mem::size_of::<i32>();
            let data = libc::CMSG_DATA(header).cast::<i32>();
            for index in 0..count {
                descriptors.push(OwnedFd::from_raw_fd(data.add(index).read_unaligned()));
            }
        }
        read
    };
    Ok((
        usize::try_from(read).expect("negative recvmsg results returned above"),
        descriptors,
    ))
}

fn peer_closed(socket: std::os::fd::BorrowedFd<'_>) -> bool {
    let mut byte = 0_u8;
    // SAFETY: one-byte non-blocking peek into live stack storage.
    unsafe {
        libc::recv(
            socket.as_raw_fd(),
            (&raw mut byte).cast(),
            1,
            libc::MSG_DONTWAIT | libc::MSG_PEEK,
        ) == 0
    }
}

/// Run the macOS headless consumer used by hardware validation and process-level tests.
#[allow(clippy::too_many_lines)]
pub fn probe_main() {
    let arguments: Vec<String> = std::env::args().collect();
    let value = |key: &str| {
        arguments
            .iter()
            .position(|argument| argument == key)
            .and_then(|index| arguments.get(index + 1))
            .cloned()
    };
    if let Some(path) = value("--socket") {
        // SAFETY: the probe owns its process environment and sets this before any transport call.
        unsafe { std::env::set_var("CY_VIEWPORT_SOCKET", path) };
    }
    let seconds = value("--seconds")
        .and_then(|text| text.parse::<f64>().ok())
        .unwrap_or(3.0);
    let expected = value("--expect-frames")
        .and_then(|text| text.parse::<u64>().ok())
        .unwrap_or(1);

    let instance = wgpu::Instance::new(wgpu::InstanceDescriptor {
        backends: wgpu::Backends::METAL,
        flags: wgpu::InstanceFlags::default(),
        memory_budget_thresholds: wgpu::MemoryBudgetThresholds::default(),
        backend_options: wgpu::BackendOptions::default(),
        display: None,
    });
    let adapter = match pollster::block_on(instance.request_adapter(&wgpu::RequestAdapterOptions {
        power_preference: wgpu::PowerPreference::HighPerformance,
        force_fallback_adapter: false,
        compatible_surface: None,
        apply_limit_buckets: false,
    })) {
        Ok(adapter) => adapter,
        Err(problem) => {
            eprintln!("[metal-probe] no adapter: {problem}");
            std::process::exit(2);
        }
    };
    let (device, queue) =
        match pollster::block_on(adapter.request_device(&wgpu::DeviceDescriptor {
            label: Some("macOS viewport probe"),
            ..Default::default()
        })) {
            Ok(pair) => pair,
            Err(problem) => {
                eprintln!("[metal-probe] no device: {problem}");
                std::process::exit(2);
            }
        };
    println!("[metal-probe] adapter = {}", adapter.get_info().name);
    let mut session = match ViewportSession::connect(&device) {
        Ok(session) => session,
        Err(problem) => {
            eprintln!("[metal-probe] no runtime: {problem}");
            std::process::exit(3);
        }
    };
    println!(
        "[metal-probe] IOSurface ring={} {}x{} import={:.3} ms",
        session.handshake.buffer_count,
        session.handshake.width,
        session.handshake.height,
        session.import_millis
    );

    let started = Instant::now();
    let mut frames = 0_u64;
    let mut last = 0_u64;
    let mut sampled_nonzero = false;
    while started.elapsed().as_secs_f64() < seconds {
        let Some((slot, frame)) = session.acquire(&device) else {
            std::thread::sleep(Duration::from_millis(1));
            continue;
        };
        if frame.frame_id == last {
            std::thread::sleep(Duration::from_millis(1));
            continue;
        }
        last = frame.frame_id;
        frames += 1;

        let readback = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("IOSurface probe pixel"),
            size: 256,
            usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
            mapped_at_creation: false,
        });
        let mut encoder = device.create_command_encoder(&wgpu::CommandEncoderDescriptor {
            label: Some("IOSurface probe copy"),
        });
        encoder.copy_texture_to_buffer(
            wgpu::TexelCopyTextureInfo {
                texture: session.texture(slot).expect("a validated slot"),
                mip_level: 0,
                origin: wgpu::Origin3d::ZERO,
                aspect: wgpu::TextureAspect::All,
            },
            wgpu::TexelCopyBufferInfo {
                buffer: &readback,
                layout: wgpu::TexelCopyBufferLayout {
                    offset: 0,
                    bytes_per_row: Some(256),
                    rows_per_image: Some(1),
                },
            },
            wgpu::Extent3d {
                width: 1,
                height: 1,
                depth_or_array_layers: 1,
            },
        );
        queue.submit([encoder.finish()]);
        let ready = std::sync::Arc::new(std::sync::atomic::AtomicBool::new(false));
        let signal = std::sync::Arc::clone(&ready);
        readback.slice(..).map_async(wgpu::MapMode::Read, move |_| {
            signal.store(true, Ordering::Release);
        });
        let _ = device.poll(wgpu::PollType::Wait {
            submission_index: None,
            timeout: Some(Duration::from_secs(2)),
        });
        if ready.load(Ordering::Acquire)
            && let Ok(bytes) = readback.slice(..4).get_mapped_range()
        {
            sampled_nonzero |= bytes.iter().any(|byte| *byte != 0);
        }
        readback.unmap();
    }

    let counters = session.counters();
    println!(
        "[metal-probe] {frames} distinct frame(s), {} announced, {} skipped, {} bounded wait timeout(s), sampled_nonzero={sampled_nonzero}",
        counters.announced, counters.skipped, counters.timed_out
    );
    if frames < expected || !sampled_nonzero {
        std::process::exit(1);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn valid_handshake() -> Handshake {
        let plane = PlaneDescription {
            stride: 640 * 4,
            surface_id: 7,
            allocation_bytes: 640 * 480 * 4,
        };
        Handshake {
            magic: PROTOCOL_MAGIC,
            version: PROTOCOL_VERSION,
            width: 640,
            height: 480,
            fourcc: u32::from_le_bytes(*b"AB24"),
            buffer_count: 4,
            generation: 1,
            has_timelines: 0,
            modifier: 0,
            planes: [plane; MAX_BUFFERS],
        }
    }

    #[test]
    fn metal_handshake_accepts_a_bounded_iosurface_ring() {
        validate_handshake(valid_handshake()).unwrap();
    }

    #[test]
    fn metal_handshake_rejects_linux_timeline_descriptors() {
        let mut handshake = valid_handshake();
        handshake.has_timelines = 1;
        let problem = validate_handshake(handshake).unwrap_err();
        assert!(problem.because.contains("completion and ownership"));
    }

    #[test]
    fn metal_handshake_rejects_an_undersized_surface() {
        let mut handshake = valid_handshake();
        handshake.planes[0].allocation_bytes -= 1;
        assert!(validate_handshake(handshake).is_err());
    }
}
