//! The runtime session: which of the three hosting modes is in force, and what happens when it dies.
//!
//! `editor-rust-application` makes **Hosted the production default**, and requires that "A runtime
//! failure SHALL NOT terminate the editor: the editor SHALL survive, surface the crash artefact ...
//! and offer to restart the runtime or open the reproduction."
//!
//! [`RuntimeSession`] is where those meet. It owns the protocol session when hosted, the loaded
//! library when embedded, and nothing at all when there is no runtime — and in every one of the
//! three, the editor's documents are somewhere else entirely, which is why losing it costs a
//! reconnect.
//!
//! --- WHY `NoRuntime` IS A FIRST-CLASS MODE RATHER THAN AN ERROR ------------------------------------------
//!
//! Because a large part of what an editor does needs no engine: browsing a project, reading source
//! assets, configuring a build, source control, specification tooling. Making those depend on a
//! runtime being up would mean a crashed runtime blocks work that has nothing to do with it — which
//! is the failure being out of process exists to prevent, reintroduced one layer higher.

use cy_editor_core::problem::{Problem, Result};
use cy_editor_protocol::{ApplyWhen, Message, RequestId, Session, SessionEvent, SessionState};
use cy_editor_sdk::HostingMode;

use crate::notifications::{Notification, NotificationService};

/// The editor's connection to an engine, or the considered absence of one.
pub struct RuntimeSession {
    mode: HostingMode,
    session: Option<Session>,
    /// Where to reconnect, so that "offer to restart the runtime" has something to offer.
    endpoint: Option<String>,
}

impl Default for RuntimeSession {
    /// No runtime. The state an editor starts in and returns to when one dies.
    fn default() -> Self {
        Self {
            mode: HostingMode::NoRuntime,
            session: None,
            endpoint: None,
        }
    }
}

impl RuntimeSession {
    /// No runtime.
    #[must_use]
    pub fn none() -> Self {
        Self::default()
    }

    /// Connect to a hosted runtime over a Unix domain socket.
    #[cfg(unix)]
    pub fn connect_hosted(endpoint: impl AsRef<std::path::Path>) -> Result<Self> {
        let endpoint = endpoint.as_ref();
        let session = Session::connect_unix(endpoint)?;
        session.send(&Message::Hello {
            abi_major: cy_editor_sdk::abi::MAJOR,
            abi_minor: cy_editor_sdk::abi::MINOR,
            editor: env!("CARGO_PKG_VERSION").to_string(),
        })?;
        Ok(Self {
            mode: HostingMode::Hosted,
            session: Some(session),
            endpoint: Some(endpoint.display().to_string()),
        })
    }

    /// Adopt an already-connected session. The seam a test double connects to.
    #[must_use]
    pub fn over(session: Session) -> Self {
        Self {
            mode: HostingMode::Hosted,
            session: Some(session),
            endpoint: None,
        }
    }

    /// Which mode is in force.
    #[must_use]
    pub const fn mode(&self) -> HostingMode {
        self.mode
    }

    /// Where a reconnect would go.
    #[must_use]
    pub fn endpoint(&self) -> Option<&str> {
        self.endpoint.as_deref()
    }

    /// Whether the editor can currently reach an engine.
    #[must_use]
    pub fn is_connected(&self) -> bool {
        self.session
            .as_ref()
            .is_some_and(|session| session.state().is_connected())
    }

    /// Send an encoded transaction to the hosted world.
    ///
    /// Returns the request to reconcile the editor's prediction against, or a [`Problem`] when there
    /// is no runtime — which is not an error the caller must handle loudly: an authoring edit with
    /// no runtime attached is a perfectly ordinary thing to do, and the document has already
    /// recorded it.
    pub fn apply(&self, transaction: Vec<u8>, when: ApplyWhen) -> Result<RequestId> {
        let session = self.session.as_ref().ok_or_else(|| {
            Problem::new("send a change to the runtime", "no runtime is attached")
                .with_remedy("start a runtime, or keep editing; the change is in the document")
        })?;
        session.apply(transaction, when)
    }

    /// Ask the attached runtime to load a newly built generation of a script module.
    ///
    /// Task 3.7. Refused with a remedy when there is no runtime, for the same reason
    /// [`RuntimeSession::apply`] is: an editor with no engine attached is an ordinary state, and the
    /// answer is "start one", not an error a caller has to treat as a failure of the build.
    pub fn reload(&self, module: &str, library: &str, generation: u32) -> Result<RequestId> {
        let session = self.session.as_ref().ok_or_else(|| {
            Problem::new(
                format!("reload {module} into the running world"),
                "no runtime is attached",
            )
            .with_remedy(
                "start a runtime and connect to it; the library is built and will still be there",
            )
        })?;
        session.reload(module, library, generation)
    }

    /// Advance the editor's frame counter, which every request is keyed by.
    pub fn advance_frame(&self) {
        if let Some(session) = &self.session {
            session.advance_frame();
        }
    }

    /// Drain what has arrived, turning a lost connection into a notification.
    ///
    /// Called once per interface frame. Never blocks — see [`Session::poll`] — and the editor
    /// survives whatever it finds, which is the milestone's headline in one function.
    pub fn pump(&mut self, notifications: &mut NotificationService) -> Vec<Message> {
        let Some(session) = &self.session else {
            return Vec::new();
        };
        let mut messages = Vec::new();
        let mut announced = false;
        for event in session.poll() {
            match event {
                SessionEvent::Message(message) => messages.push(message),
                SessionEvent::Lost(problem) => {
                    notifications.post(Notification::error("The hosted runtime stopped", problem));
                    announced = true;
                }
            }
        }
        // A session that has been lost is dropped rather than kept in a broken state: the editor
        // returns to NoRuntime, which is a mode it works perfectly well in, and the notification
        // carries the offer to restart.
        //
        // THE STATE IS THE AUTHORITY AND THE EVENT IS ONLY THE FAST PATH, which is not a nicety —
        // it is the difference between the editor saying its runtime died and the editor going
        // quiet. Two interleavings lose the event, and the session is dropped here, so anything
        // the event was carrying is gone for good:
        //
        //   * `Session::lose` marks the state lost and *then* sends `SessionEvent::Lost`. A pump
        //     that lands between the two polls an empty queue, sees a state that is no longer
        //     connected, and drops the session; the event is delivered to a receiver nobody will
        //     read again. Reproduced at roughly one run in ten of
        //     `a_runtime_that_dies_becomes_a_notification_and_the_editor_returns_to_no_runtime`
        //     in the `dev` and `profiling` Cargo profiles, which is how it was found.
        //   * The writer thread calls `mark_lost` and sends NO event at all, deliberately — see
        //     `Session::spawn_writer`, which reasons that "the reader thread will notice too and
        //     is the one that reports". It does not always: a peer that stops reading while its
        //     own write end stays open fails the write and never closes the stream, so the reader
        //     blocks forever and the only record of the failure is the state.
        //
        // So the loss is announced from whichever of the two arrived, and `announced` keeps it to
        // one notification when both do.
        if let SessionState::Lost(problem) = session.state() {
            if !announced {
                notifications.post(Notification::error("The hosted runtime stopped", problem));
            }
            self.session = None;
            self.mode = HostingMode::NoRuntime;
        }
        messages
    }

    /// Why the session ended, when it has.
    #[must_use]
    pub fn failure(&self) -> Option<Problem> {
        match self.session.as_ref()?.state() {
            SessionState::Connected => None,
            SessionState::Lost(problem) => Some(problem),
        }
    }
}

#[cfg(test)]
mod tests {
    use std::time::Duration;

    use super::*;

    #[test]
    fn no_runtime_is_a_mode_rather_than_a_failure() {
        let session = RuntimeSession::none();
        assert_eq!(session.mode(), HostingMode::NoRuntime);
        assert!(!session.is_connected());

        let problem = session.apply(Vec::new(), ApplyWhen::OnArrival).unwrap_err();
        assert!(
            problem.remedy.as_deref().unwrap().contains("keep editing"),
            "an edit with no runtime attached is ordinary, and the remedy should say so"
        );
    }

    #[test]
    fn a_runtime_that_dies_becomes_a_notification_and_the_editor_returns_to_no_runtime() {
        let (editor_reader, runtime_writer) = std::io::pipe().unwrap();
        let (runtime_reader, editor_writer) = std::io::pipe().unwrap();
        let mut runtime = RuntimeSession::over(Session::over(editor_reader, editor_writer));
        assert!(runtime.is_connected());

        drop(runtime_writer);
        drop(runtime_reader);

        let mut notifications = NotificationService::new();
        let deadline = std::time::Instant::now() + Duration::from_secs(5);
        while runtime.is_connected() && std::time::Instant::now() < deadline {
            runtime.pump(&mut notifications);
            std::thread::sleep(Duration::from_millis(1));
        }
        runtime.pump(&mut notifications);

        assert_eq!(runtime.mode(), HostingMode::NoRuntime);
        let mut cursor = cy_editor_core::observe::Cursor::default();
        let posted = notifications.drain_from(&mut cursor);
        assert_eq!(posted.len(), 1);
        assert_eq!(posted[0].message, "The hosted runtime stopped");
        assert!(posted[0].problem.as_ref().unwrap().remedy.is_some());
    }

    /// A writer that always fails, so the write side of a session dies while the read side does not.
    struct RefusesToWrite;

    impl std::io::Write for RefusesToWrite {
        fn write(&mut self, _: &[u8]) -> std::io::Result<usize> {
            Err(std::io::Error::new(
                std::io::ErrorKind::BrokenPipe,
                "the runtime stopped reading",
            ))
        }

        fn flush(&mut self) -> std::io::Result<()> {
            Ok(())
        }
    }

    /// REGRESSION, and the deterministic half of the defect the racy sibling above caught.
    ///
    /// `Session::spawn_writer` marks the state lost and sends no `SessionEvent::Lost`, reasoning
    /// that the reader will notice. Here the reader cannot: the pipe's write end is held open for
    /// the whole test, so the reader blocks and the *only* record of the failure is the state.
    /// `pump` used to drop the session on that state without saying anything, which returned the
    /// editor to `NoRuntime` in silence and threw away the offer to restart —
    /// `editor-rust-application` requires a runtime failure to be surfaced, not merely survived.
    /// Before the fix this failed on every run rather than one in ten.
    #[test]
    fn a_write_side_failure_is_surfaced_even_though_the_reader_never_posts_an_event() {
        let (editor_reader, runtime_writer) = std::io::pipe().unwrap();
        let mut runtime = RuntimeSession::over(Session::over(editor_reader, RefusesToWrite));
        assert!(runtime.is_connected());
        runtime
            .apply(vec![1, 2, 3], ApplyWhen::OnArrival)
            .expect("the send is queued; the failure happens on the writer thread");

        let mut notifications = NotificationService::new();
        let deadline = std::time::Instant::now() + Duration::from_secs(5);
        while runtime.is_connected() && std::time::Instant::now() < deadline {
            runtime.pump(&mut notifications);
            std::thread::sleep(Duration::from_millis(1));
        }
        runtime.pump(&mut notifications);

        assert_eq!(runtime.mode(), HostingMode::NoRuntime);
        let mut cursor = cy_editor_core::observe::Cursor::default();
        let posted = notifications.drain_from(&mut cursor);
        assert_eq!(
            posted.len(),
            1,
            "a lost runtime is announced once, whichever half of the session noticed it"
        );
        assert_eq!(posted[0].message, "The hosted runtime stopped");
        assert!(posted[0].problem.as_ref().unwrap().remedy.is_some());

        // Held to the end on purpose: it is what keeps the reader thread blocked, which is what
        // makes this test about the writer.
        drop(runtime_writer);
    }
}
