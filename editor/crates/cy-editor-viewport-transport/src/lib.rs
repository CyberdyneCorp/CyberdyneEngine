//! The viewport's transport, on the editor's side: the engine's own rendered image, imported rather
//! than copied, and the synchronisation that makes reading it safe while another process is still
//! drawing into the ring it came from.
//!
//! `editor-viewport-and-gizmos` requires the viewport to obtain its image through an abstract
//! transport, one of whose three kinds is a **shared texture** — "the runtime is a separate process
//! on this machine and shares the texture". `cy_editor_viewport::transport` is that abstraction, is
//! headless, and names no device. This crate is the shared-texture kind, and it names one.
//!
//! `editor-rust-application` requires that a runtime failure not terminate the editor. Everything
//! here is built around that sentence rather than around throughput.
//!
//! # The five decisions, each measured rather than reasoned
//!
//! The M5.5 cross-process synchronisation spike ran these on this hardware. They are implemented
//! here, not re-derived; the numbers are in `README.md` and the reasoning is in
//! `openspec/changes/implement-m5b-operable/design.md`.
//!
//! 1. **Timeline semaphores over `OPAQUE_FD`**, imported into a wgpu device that had
//!    `VK_KHR_external_semaphore_fd` pushed into its extension list by
//!    [`wgpu_hal::vulkan::Adapter::open_with_callback`] before `vkCreateDevice`. `SYNC_FD` is
//!    binary-only and cannot carry a timeline. See [`device`].
//!
//!    The capability check is **whether the extension was enabled**, never whether a function
//!    pointer is non-null: ash installs a *panicking stub* for an entry point it could not load, so
//!    the pointer test returns true without the extension and the call then aborts the process.
//!
//! 2. **A bounded host wait, not a GPU wait.** `vkWaitSemaphores` with a 2 ms timeout on the
//!    editor's own thread. Staging the wait on the editor's queue instead is one bad value away
//!    from an editor that renders nothing and cannot be closed — measured: `submit` returns in
//!    0.19 ms because the wait is on the GPU, every later independent submission then times out,
//!    and shutdown hangs forever inside `vkDeviceWaitIdle`. The bounded wait shows 97% of the
//!    newest frames at the same latency and can never wedge anything. See [`session::WaitPolicy`].
//!
//! 3. **Three images minimum, four preferred, and the runtime drops rather than blocks.** One
//!    wedges; two throttle the runtime to the editor's refresh rate and cost a whole editor frame
//!    of latency. The editor must never throttle the runtime, so a full ring is the runtime's frame
//!    to drop. See [`ring`].
//!
//! 4. **Announce after `vkQueueSubmit`, never before.** See [`announce`], where the invariant is
//!    written into the code beside the function that would break it.
//!
//! 5. **Per-frame state goes in a shared page under a seqlock, not on the socket**, and
//!    `held = (frame_id, slot)` flows editor → runtime. See [`announce`] for both, each with the
//!    corruption it prevents.
//!
//! # What is deliberately not here
//!
//! No window, no toolkit, no panel. This crate hands back a `wgpu::Texture` and a
//! [`cy_editor_viewport::transport::PresentedFrame`]; what draws them is the single render crate,
//! and `cy-editor-app/tests/containment.rs` fails if any crate but that one and this one names a
//! graphics API.

// The one platform module of the editor's viewport: dma-buf, Vulkan and POSIX shared memory. The
// workspace forbids `unsafe` everywhere else, and `cy-editor-app/tests/safety.rs` names this crate
// with its reason so that widening the audit stays a decision rather than an edit.

// --- WHY THE WHOLE CRATE IS BEHIND ONE `cfg` ------------------------------------------------------
//
// The mechanism is Linux's: dma-buf, DRM format modifiers, `memfd_create`, `SCM_RIGHTS`, and
// `VK_KHR_external_semaphore_fd` over `OPAQUE_FD`. macOS has IOSurface and Windows has NT handles,
// and both would be a different transport rather than this one compiled elsewhere.
//
// The editor's workspace is built and tested on all three platforms, so this crate compiles to
// nothing on the other two rather than being excluded from the workspace — a member that vanishes
// per platform is a member whose tests silently stop running, which is the failure mode this
// project has already paid for. Everything above it treats "there is no transport" the way it
// treats a runtime that has not started: the viewport says so, and the editor keeps working.

/// What the binaries print where the mechanism does not exist.
pub const UNSUPPORTED: &str = "the editor's viewport transport is Linux-only: it is dma-buf, \
                               memfd and OPAQUE_FD. On macOS and Windows the viewport needs a \
                               different transport, which does not exist yet.";

#[cfg(target_os = "linux")]
pub mod announce;
#[cfg(target_os = "linux")]
pub mod device;
#[cfg(target_os = "linux")]
pub mod image;
#[cfg(target_os = "linux")]
pub mod probe;
#[cfg(target_os = "linux")]
pub mod publisher;
#[cfg(target_os = "linux")]
pub mod ring;
#[cfg(target_os = "linux")]
pub mod session;
#[cfg(target_os = "linux")]
pub mod wire;

#[cfg(target_os = "linux")]
pub use announce::{Announcement, AnnouncementPage, held_pack, held_unpack};
#[cfg(target_os = "linux")]
pub use device::{Gpu, WANTED_EXTENSIONS};
#[cfg(target_os = "linux")]
pub use image::{import_dmabuf, import_timeline};
#[cfg(target_os = "linux")]
pub use ring::{FullRingPolicy, Ring, RingSlot};
#[cfg(target_os = "linux")]
pub use session::{SessionCounters, ViewportSession, WaitPolicy};
#[cfg(target_os = "linux")]
pub use wire::{Handshake, PlaneDescription, default_socket_path};
