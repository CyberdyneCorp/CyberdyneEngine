//! The viewport's link to the runtime's rendered image.
//!
//! --- THE DECISION THIS FILE ENFORCES --------------------------------------------------------------
//!
//! `design.md` §2: *"The viewport is the engine's frame from the first pixel. If the transport is not
//! ready, the viewport shows a message saying so — not an approximation."*
//!
//! That is a rule about what this module may return. It has exactly two answers: a texture the
//! runtime rendered, or a sentence. There is no third state in which the editor draws a grid, a
//! gradient, a checkerboard or a cube of its own, because every one of those is a second renderer
//! arriving one frame at a time — and `editor-viewport-and-gizmos` forbids a second renderer for a
//! reason that outlives this milestone: what the editor shows must be what the game will show.
//!
//! --- WHY IT IS A THIN WRAPPER AND NOT THE TRANSPORT ------------------------------------------------
//!
//! `cy-editor-viewport-transport` is the platform module: dma-buf import, Vulkan timeline
//! semaphores, `memfd`, `SCM_RIGHTS`, and the bounded host wait that keeps a bad value from wedging
//! the editor. It is a crate of its own so that the headless probe and this window run *the same*
//! import and synchronisation code. What is left here is the toolkit half — registering the imported
//! texture with egui's renderer, and re-registering it when the runtime hands back a different slot.
//!
//! --- AND WHY IT IS `cfg`-GATED ---------------------------------------------------------------------
//!
//! The mechanism is Linux's. On macOS and Windows this compiles to a structure with one variant that
//! says so, which is the honest answer rather than a stub that pretends to connect. Those two
//! platforms are unverified in this milestone in every respect; see the crate header.

use cy_editor_visual::colour::Semantic;

/// What the viewport can say about its link to the runtime.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct Condition {
    /// The sentence the viewport shows when it has no image.
    pub message: String,
    /// How to read it: informational while connecting, an error once it has failed.
    pub role: Semantic,
}

impl Condition {
    fn new(message: impl Into<String>, role: Semantic) -> Self {
        Self {
            message: message.into(),
            role,
        }
    }
}

#[cfg(target_os = "linux")]
pub use linux::ViewportLink;

#[cfg(not(target_os = "linux"))]
pub use elsewhere::ViewportLink;

#[cfg(target_os = "linux")]
mod linux {
    use std::sync::Arc;

    use cy_editor_viewport_transport::session::{Liveness, monotonic_nanos};
    use cy_editor_viewport_transport::{Gpu, ViewportSession};
    use cy_editor_visual::colour::Semantic;

    use super::Condition;

    /// The editor's side of the viewport transport, plus the texture registration egui needs.
    pub struct ViewportLink {
        gpu: Option<Arc<Gpu>>,
        session: Option<ViewportSession>,
        /// The slot currently registered with egui's renderer, and its identifier.
        registered: Option<(usize, egui::TextureId)>,
        /// Why there is no session, when there is not one.
        condition: Condition,
        /// Whether the runtime was Live the last time a frame was claimed.
        ///
        /// SEPARATE FROM `texture()` BECAUSE THE TWO ANSWER DIFFERENT QUESTIONS, and M5.5's gate
        /// found the editor conflating them. After a runtime dies the last complete frame is
        /// deliberately RETAINED — that is the surviving-a-crash behaviour `editor-rust-application`
        /// asks for — so `texture()` keeps returning `Some` and a panel that draws the condition
        /// only when there is no texture says nothing at all. The user then sees a frozen picture,
        /// counters that stop advancing, and no sentence. That is M5's failure shape, a fallback
        /// that happens in silence, in a new place.
        live: bool,
        /// The device's start-up notes, worth reading when the viewport shows nothing.
        notes: Vec<String>,
    }

    impl ViewportLink {
        /// A link that has not connected.
        ///
        /// Not connecting here is deliberate: `Gpu::create` opens a Vulkan device, and an editor
        /// that refused to start because a GPU was busy would have made the viewport's transport a
        /// precondition of having a window at all.
        #[must_use]
        pub fn idle() -> Self {
            Self {
                gpu: None,
                session: None,
                registered: None,
                condition: Condition::new(
                    "The transport is not ready: no runtime has offered a rendered frame.",
                    Semantic::SecondaryText,
                ),
                live: false,
                notes: Vec::new(),
            }
        }

        /// Adopt the device the window was created with, and try to attach to a runtime.
        ///
        /// A failure is recorded as the sentence the viewport shows and is **not** fatal, which is
        /// the same rule the editor already applies to a runtime that is not listening: an editor
        /// with no runtime is a mode, not a broken state.
        pub fn attach(&mut self, gpu: Arc<Gpu>) {
            self.notes.clone_from(&gpu.notes);
            match ViewportSession::connect(Arc::clone(&gpu)) {
                Ok(session) => {
                    self.session = Some(session);
                    self.condition = Condition::new(
                        "Waiting for the runtime's first frame.",
                        Semantic::SecondaryText,
                    );
                }
                Err(problem) => {
                    self.condition = Condition::new(
                        format!(
                            "The transport is not ready: {}. {}",
                            problem.because,
                            problem
                                .remedy
                                .as_deref()
                                .unwrap_or("Start a runtime that publishes to CY_VIEWPORT_SOCKET.")
                        ),
                        Semantic::SecondaryText,
                    );
                }
            }
            self.gpu = Some(gpu);
        }

        /// The device's start-up notes.
        #[must_use]
        pub fn notes(&self) -> &[String] {
            &self.notes
        }

        /// Whether a session exists at all.
        #[must_use]
        pub fn is_attached(&self) -> bool {
            self.session.is_some()
        }

        /// What to say when there is no image.
        #[must_use]
        pub fn condition(&self) -> &Condition {
            &self.condition
        }

        /// Whether the runtime is delivering frames. False both before one attaches and after one
        /// dies, which is what a panel needs to decide whether to say something over the image.
        #[must_use]
        pub fn is_live(&self) -> bool {
            self.live
        }

        /// The texture the viewport panel draws, as of this frame's [`ViewportLink::begin_frame`].
        #[must_use]
        pub fn texture(&self) -> Option<egui::TextureId> {
            self.registered.map(|(_, id)| id)
        }

        /// Claim the newest frame, once per interface frame, before any panel is drawn.
        ///
        /// The order here is the protocol's and it is not negotiable: acquire, register, and release
        /// the claim. `release_finished` must be called before the queue is submitted — which
        /// happens after the whole interface has been built — because a slot released afterwards is
        /// one the runtime may overwrite while the editor's own commands are still reading it.
        ///
        /// It is done here rather than inside the panel because a panel is drawn only when its tab
        /// is in front, and a transport that stopped claiming frames when the viewport was behind
        /// another tab would look like a runtime that had stalled.
        pub fn begin_frame(
            &mut self,
            render_state: &egui_wgpu::RenderState,
            viewport: &mut cy_editor_viewport::viewport::Viewport,
        ) -> Option<egui::TextureId> {
            let session = self.session.as_mut()?;
            match session.liveness() {
                Liveness::Live => {
                    self.live = true;
                }
                gone => {
                    // The editor freezes on the last complete frame and says why, rather than going
                    // black. Six SIGKILL runs in the M5.5 synchronisation spike survived exactly
                    // this way.
                    self.live = false;
                    self.condition = Condition::new(gone.message(), Semantic::Warning);
                    return self.registered.map(|(_, id)| id);
                }
            }

            let (slot, _frame) = session.acquire()?;
            if self.registered.map(|(held, _)| held) != Some(slot) {
                let view = session.view(slot)?;
                let id = render_state.renderer.write().register_native_texture(
                    &render_state.device,
                    view,
                    wgpu::FilterMode::Linear,
                );
                self.registered = Some((slot, id));
            }

            // THE LINE M5.5's GATE RECORDED AS MISSING, AND WHAT IT COSTS TO LEAVE IT OUT.
            //
            // Importing the texture makes the frame VISIBLE. It does not make the frame KNOWN: the
            // viewport model learns that a frame arrived only through `Viewport::pump`, and until it
            // does, `Viewport::pick` answers `None` ("no frame has arrived yet") beside an overlay
            // reporting "announced 1016", and `interaction_view` resolves every click against the
            // camera the editor ASKED for rather than the one a frame was rendered with. That is a
            // click landing somewhere other than where it was aimed, on a moving camera, with no
            // diagnostic — and it was invisible for a whole milestone because both halves were
            // individually correct.
            //
            // `ViewportSession` implements `Transport`, and its `poll` re-enters `acquire`, which is
            // idempotent once a frame has been claimed: with nothing newer it answers the frame
            // already being shown. So this is one call per interface frame and no extra claim.
            viewport.pump(session, monotonic_nanos() / 1_000);

            session.release_finished();
            self.registered.map(|(_, id)| id)
        }

        /// Tell the transport what the editor asked to see.
        ///
        /// **Without this, every presented frame carries a default camera**, and a click resolved
        /// against "the view state of the frame shown" is resolved against a camera nobody is
        /// looking through. The runtime will echo the state it actually rendered once the engine's
        /// render server publishes frames of its own; until then this is the honest approximation —
        /// what was asked for, carried alongside the image it produced.
        pub fn publish_view_state(&mut self, state: cy_editor_viewport::state::ViewState) {
            if let Some(session) = self.session.as_mut() {
                session.set_view_state(state);
            }
        }

        /// The transport's counters, which is what a viewport overlay reports.
        #[must_use]
        pub fn counters(&self) -> Option<String> {
            let session = self.session.as_ref()?;
            let counters = session.counters();
            Some(format!(
                "announced {} · skipped {} · timed out {} · wrong generation {}",
                counters.announced, counters.skipped, counters.timed_out, counters.wrong_generation
            ))
        }
    }

    impl Default for ViewportLink {
        fn default() -> Self {
            Self::idle()
        }
    }
}

#[cfg(not(target_os = "linux"))]
mod elsewhere {
    use cy_editor_visual::colour::Semantic;

    use super::Condition;

    /// The link on a platform whose viewport transport does not exist yet.
    ///
    /// One variant, and it says so. A stub that reported "connecting" for ever would make an
    /// unimplemented platform look like a broken runtime.
    pub struct ViewportLink {
        condition: Condition,
    }

    impl ViewportLink {
        /// A link that cannot be made on this platform.
        #[must_use]
        pub fn idle() -> Self {
            Self {
                condition: Condition::new(
                    "The viewport transport is implemented for Linux only. This build shows no \
                     engine image, and draws no approximation of one.",
                    Semantic::Warning,
                ),
            }
        }

        /// Nothing.
        #[must_use]
        pub fn notes(&self) -> &[String] {
            &[]
        }

        /// Never.
        #[must_use]
        pub fn is_attached(&self) -> bool {
            false
        }

        /// What to say instead of an image.
        #[must_use]
        pub fn condition(&self) -> &Condition {
            &self.condition
        }

        /// Never live: this platform has no transport. A panel needs this separately from whether a
        /// texture exists — see the Linux implementation's `live` field for why.
        #[must_use]
        pub fn is_live(&self) -> bool {
            false
        }

        /// Never an image, and so never a frame for the viewport model to learn about.
        ///
        /// The signature matches the Linux one, `Viewport` included, so the window's frame loop is
        /// the same code on every platform.
        pub fn begin_frame(
            &mut self,
            _render_state: &egui_wgpu::RenderState,
            _viewport: &mut cy_editor_viewport::viewport::Viewport,
        ) -> Option<egui::TextureId> {
            None
        }

        /// Nowhere to publish it to. The signature matches the Linux one so the panel does not
        /// have to know which platform it is on.
        pub fn publish_view_state(&mut self, state: cy_editor_viewport::state::ViewState) {
            let _ = state;
        }

        /// No counters, because there is no transport.
        #[must_use]
        pub fn counters(&self) -> Option<String> {
            None
        }
    }

    impl Default for ViewportLink {
        fn default() -> Self {
            Self::idle()
        }
    }
}

#[cfg(test)]
mod tests {
    use cy_editor_viewport::picking::PickIntent;
    use cy_editor_viewport::transport::{
        FrameImage, Mailbox, MailboxTransport, PresentedFrame, TransportKind,
    };
    use cy_editor_viewport::viewport::{Viewport, ViewportId};

    use super::ViewportLink;

    /// The defect M5.5's gate recorded, and the fix, as one test.
    ///
    /// It does not need a GPU, a window or a runtime, because the failure never needed one: the
    /// image reached the screen and the MODEL never learned of it. `ViewportLink::begin_frame` now
    /// calls `Viewport::pump` on exactly this seam, over a transport of a different kind — the
    /// property is the model's, not the dma-buf path's.
    #[test]
    fn a_viewport_that_is_never_pumped_cannot_answer_a_click() {
        let mut viewport = Viewport::new(
            ViewportId::from_raw(1),
            "Perspective",
            TransportKind::SharedTexture,
        );
        let mailbox = Mailbox::new();
        mailbox.publish(PresentedFrame::new(
            cy_editor_protocol::FrameId::from_raw(1016),
            viewport.state.clone(),
            FrameImage::Surface(0),
            0,
        ));
        let mut transport = MailboxTransport::new(TransportKind::SharedTexture, mailbox);

        // A frame has been announced and nothing has consumed it. This is exactly the state the
        // window was in for the whole of M5.5: an overlay reading "announced 1016" beside a click
        // that reports "No frame has arrived yet".
        assert!(
            viewport
                .pick(PickIntent::Click { x: 8.0, y: 8.0 })
                .is_none(),
            "an unpumped viewport has no frame to resolve a click against"
        );

        viewport.pump(&mut transport, 1_000);

        let request = viewport
            .pick(PickIntent::Click { x: 8.0, y: 8.0 })
            .expect("a pick names the frame that was on screen");
        assert_eq!(
            request.frame.as_u64(),
            1016,
            "and it names the frame that arrived, not a newer one"
        );
    }

    /// A link with no runtime says so and claims nothing, on every platform.
    #[test]
    fn an_idle_link_has_no_image_and_says_why() {
        let link = ViewportLink::idle();
        assert!(!link.is_attached());
        assert!(!link.is_live());
        assert!(
            !link.condition().message.is_empty(),
            "there is always a sentence instead of an image"
        );
    }
}
