//! The live bridge's control path. Tasks 2.2 and 2.6, and the milestone's named risk.
//!
//! `editor-rust-application` makes **Hosted the production default**: the engine in a separate
//! process or on a remote device. This crate is how the editor reaches it.
//!
//! --- WHAT THE MILESTONE'S SPIKE MEASURED, AND WHAT THIS CRATE DOES ABOUT IT --------------------------
//!
//! The spike at `build/spike/latency-spike/` measured a gizmo drag's round trip across the boundary,
//! locally and through a network emulator. Four of its findings are structural here, not advisory:
//!
//! 1. **The boundary costs about 60 microseconds; the runtime's frame costs 8.6 ms.** Out of process
//!    versus in process, both correctly frame-coupled, is +0.3 to +1.3 ms at p50. So the boundary is
//!    not what makes an editor feel slow, and crash isolation is cheap.
//! 2. **Never block a UI frame on the round trip.** Blocking p50 is 9.9 ms locally but p99 is 20.2
//!    ms and one sample in 900 hit 29 ms — and past a LAN it is hopeless: 27 ms metro, 85 ms
//!    continent, 170 ms intercontinental. So [`Session`] has no blocking send. The UI draws from
//!    locally predicted state and reconciles against the runtime's echo, keyed by the frame
//!    identifier every request carries.
//! 3. **Apply on arrival when nothing is simulating.** An authoring world not in play mode should
//!    apply a command when it arrives rather than at a 60 Hz tick boundary: p50 0.059 ms against
//!    9.938 ms, a 168-fold improvement from a scheduling decision. [`Message::Apply`] therefore
//!    carries [`ApplyWhen`], so the runtime is told which it is rather than guessing.
//! 4. **`TCP_NODELAY` on any TCP path, and an application-level retransmit if control ever gets its
//!    own datagram path.** A 6 ms link with 1% loss gave p99 210 ms at `TCP_RTO_MIN` against 30 ms
//!    with a 20 ms application timeout, and a paced drag never has enough packets in flight to
//!    trigger a fast retransmit. Local transport is a Unix domain socket, where neither applies;
//!    the note is here because the day a TCP transport is added is the day it matters.
//!
//! --- WHAT IS NOT MEASURED, AND SHOULD NOT BE ASSUMED --------------------------------------------------
//!
//! The **viewport transport**. The spike measured the control path only. If the image a user is
//! dragging against is two or three frames stale, the drag feels laggy however fast the transform
//! applies. That belongs to task 4.1 and should be measured before panels are built on it.

#![forbid(unsafe_code)]

pub mod frame;
pub mod message;
pub mod server;
pub mod session;

pub use frame::{FrameId, read_frame, write_frame};
pub use message::{ApplyWhen, Message, RequestId};
pub use server::serve;
pub use session::{Session, SessionEvent, SessionState};
