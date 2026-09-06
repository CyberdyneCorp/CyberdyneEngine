//! The viewport: what the editor decides should be shown, and how it asks for it. Tasks 4.1–4.5.
//!
//! `editor-viewport-and-gizmos` opens with the sentence this whole crate is arranged around:
//!
//! > **The editor decides what should be shown. The renderer decides how it is drawn.**
//!
//! Reading that as an instruction about *what may exist in this crate* gives the module list below.
//! There is no render pass here, no render target, no pipeline, no command buffer and no draw call —
//! and no dependency that could supply one, which is what makes the requirement's forbidden-pattern
//! list checkable rather than aspirational:
//!
//! | Forbidden pattern | How this crate makes it checkable |
//! |---|---|
//! | Editor code constructing render passes or issuing draws | Nothing in this crate names a device; the dependency list is core, documents and protocol |
//! | A second renderer or an editor-only shading path | [`viewmode`] mirrors `cy::render::DebugViewMode` exactly, and a test reads the engine's list |
//! | Editor-side picking that does not match what was rendered | [`picking`] sends a pixel and a frame identifier; the ray is built engine-side against that frame's draw list |
//! | Gizmo manipulation accumulating per-frame deltas | [`gizmo`] recomputes from captured drag-start state; a returning drag restores the original bits |
//! | A viewport feature that works only in-process | [`transport`]'s three kinds differ in one field, and everything else reads the fields they share |
//! | Overlays baked into an image presented as representative | [`overlay::Capture`] has no constructor that omits whether overlays were included |
//! | Blocking the interface thread on runtime frame production | Nothing here has a blocking call; [`transport::Transport::poll`] returns an `Option` |
//! | Selection stored as pointers or indices | Everything is a stable identity: `u64` from the engine, `NodeId` in the document |
//!
//! --- THE MODULES --------------------------------------------------------------------------------------
//!
//! | Module | Task | What it owns |
//! |---|---|---|
//! | [`math`] | — | The geometry manipulation intent needs, and nothing more |
//! | [`state`] | 4.1, 4.5 | A view state: pose, projection, view mode, filters, time — capturable and restorable |
//! | [`transport`] | 4.1 | How the image arrives, how old it is, and what it cost |
//! | [`navigation`] | 4.1 | Orbit, pan, zoom, fly, focus, axis snap; bindings and presets |
//! | [`picking`] | 4.2 | Pick requests against a presented frame, and the selection they produce |
//! | [`gizmo`] | 4.3 | Manipulation from captured drag-start state, as exactly one transaction |
//! | [`snapping`] | 4.3 | Grid, angle, scale and surface snapping; numeric entry with units and expressions |
//! | [`viewmode`] | 4.4 | The engine's debug views, described well enough for a palette |
//! | [`overlay`] | 4.4 | Overlays, the orientation widget, and what a capture contains |
//! | [`play`] | 4.4 | Editing while playing: what is unmistakable and what persists |
//! | [`reconcile`] | 4.3 | Local prediction and the runtime's authoritative echo, keyed by frame |
//! | [`budget`] | 4.5 | Cadence, degradation and what the user is told about it |
//! | [`viewport`] | 4.1, 4.5 | A viewport, and several of them with one focused |
//!
//! --- WHAT THE MILESTONE'S SPIKE LEFT UNMEASURED, AND WHAT WAS MEASURED HERE ----------------------------
//!
//! The control-path spike measured a gizmo drag's round trip and said plainly what it had not
//! covered: "Nothing here measures the shared-texture or encoded-stream transport. If the image the
//! user is dragging against is two or three frames stale, the drag feels laggy however fast the
//! transform applies. Measure that at task 4.1 before four panels are built on the assumption."
//!
//! `tests/frame_age.rs` measures it, across a real process boundary, and the numbers are in
//! `README.md`. The finding that changed a decision: **the queueing discipline dominates the
//! transport**. A queue between a 60 Hz producer and a consumer that occasionally stalls does not
//! smooth anything — the age of the frame being drawn over grows without bound and never recovers
//! (p50 523 ms, p99 1045 ms measured), while every per-frame measurement looks healthy. A
//! single-slot mailbox stays at one producer interval (p50 8.4 ms, p99 16.6 ms) and reports what it
//! dropped. [`transport::Mailbox`] is that decision.
//!
//! --- WHICH THE DRAG LOOP DOES: PREDICT AND RECONCILE --------------------------------------------------
//!
//! Stated plainly, because the milestone asked for it to be: **this crate predicts locally and
//! reconciles against the runtime's echo. It does not do a round trip per frame.** A drag writes to
//! the document, which is in the editor's process, so the gizmo is drawn from a correct local value
//! with no wait; the same operations go to the runtime asynchronously; the echo is matched by the
//! frame identifier the viewport transport already carries. [`reconcile`] is the mechanism and its
//! module note has the spike's numbers that make it the only workable choice.

#![forbid(unsafe_code)]

pub mod budget;
pub mod gizmo;
pub mod math;
pub mod navigation;
pub mod overlay;
pub mod picking;
pub mod play;
pub mod reconcile;
pub mod snapping;
pub mod state;
pub mod transport;
pub mod viewmode;
pub mod viewport;

pub use budget::{Cadence, ViewportBudget};
pub use gizmo::{Drag, GizmoMode, GizmoSpace, Pivot, TransformBinding};
pub use math::{Bounds, Quat, Ray, Vec3};
pub use navigation::{Bindings, NavigationPreset, Navigator, ViewAxis};
pub use picking::{PickIntent, PickRequest, PickResponse, SelectionMode};
pub use reconcile::{Divergence, Prediction, Reconciler};
pub use snapping::{SnapSettings, Unit};
pub use state::{CameraPose, Projection, ViewState, ViewportRect, VisibilityFilter};
pub use transport::{
    Degradation, FrameImage, FrameStream, Mailbox, PresentedFrame, Transport, TransportKind,
};
pub use viewmode::ViewMode;
pub use viewport::{Viewport, ViewportId, Viewports};
