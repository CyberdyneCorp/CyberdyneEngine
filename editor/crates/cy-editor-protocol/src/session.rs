//! The editor's side of the bridge: asynchronous, and the reason a runtime crash costs a restart.
//!
//! --- THE ONE RULE THIS TYPE EXISTS TO ENFORCE ---------------------------------------------------------
//!
//! **There is no blocking send.** [`Session::send`] enqueues and returns a [`RequestId`] in
//! microseconds; [`Session::poll`] drains whatever has arrived. A UI frame therefore cannot wait on
//! the runtime, however slow the runtime or the network is, because there is no call that would wait.
//!
//! That is the spike's finding 2 turned into an API shape rather than a guideline. Blocking p50 is
//! 9.9 ms locally, p99 is 20.2 ms, and past a LAN the p50 alone is 27 to 170 ms; a design that
//! *could* block would block, in some code path, on some machine, and the symptom would be a
//! dropped frame that nobody could attribute.
//!
//! [`Session::block_until`] exists for tests and command-line tools, and is named for what it does
//! so that a reviewer seeing it on a UI path knows immediately that it is wrong.
//!
//! --- WHAT HAPPENS WHEN THE RUNTIME DIES ---------------------------------------------------------------
//!
//! `editor-rust-application`: "A runtime failure SHALL NOT terminate the editor: the editor SHALL
//! survive, surface the crash artefact, and offer to restart the runtime or open the reproduction."
//!
//! The reader thread sees the stream end, sets [`SessionState::Lost`] with a [`Problem`] that says
//! what happened and what to do, and stops. Nothing panics, nothing unwinds into the editor, and
//! every document is untouched — because the documents are in the editor's process and the session
//! holds none of them. That is the whole argument for being out of process, and it is one field.

use std::io::Write;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::mpsc::{Receiver, Sender, TryRecvError, channel};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use cy_editor_core::problem::{Problem, Result};

use crate::frame::{FrameId, read_frame, write_frame};
use crate::message::{ApplyWhen, Message, RequestId};

/// Where a session is.
#[derive(Clone, PartialEq, Eq, Debug)]
pub enum SessionState {
    /// Connected and usable.
    Connected,
    /// The runtime went away. The editor keeps running; this is what it shows.
    Lost(Problem),
}

impl SessionState {
    /// Whether the session can still carry a message.
    #[must_use]
    pub const fn is_connected(&self) -> bool {
        matches!(self, SessionState::Connected)
    }
}

/// Something that arrived from the runtime.
#[derive(Clone, PartialEq, Debug)]
pub enum SessionEvent {
    /// A message.
    Message(Message),
    /// The connection ended, with the reason to show.
    Lost(Problem),
}

/// A connection to a hosted runtime.
///
/// Owns two threads — one reading, one writing — so that neither direction can stall the other and
/// neither can stall the caller. The caller's only cost is a channel send.
pub struct Session {
    outgoing: Sender<Vec<u8>>,
    incoming: Receiver<SessionEvent>,
    state: Arc<Mutex<SessionState>>,
    next_request: AtomicU64,
    frame: AtomicU64,
}

impl Session {
    /// Connect to a hosted runtime over a Unix domain socket.
    ///
    /// A Unix domain socket because the spike measured it at p50 0.059 ms with framing and
    /// connection semantics for free. Shared memory measured 0.034 ms and is not worth the
    /// complexity for control messages — it is there if the tail ever matters.
    #[cfg(unix)]
    pub fn connect_unix(path: impl AsRef<std::path::Path>) -> Result<Self> {
        let path = path.as_ref();
        let stream = std::os::unix::net::UnixStream::connect(path).map_err(|error| {
            Problem::new(
                format!("connect to the runtime at {}", path.display()),
                error.to_string(),
            )
            .with_remedy("start the runtime, or check the socket path in the project settings")
        })?;
        let reader = stream
            .try_clone()
            .map_err(|error| Problem::new("duplicate the runtime connection", error.to_string()))?;
        Ok(Self::over(reader, stream))
    }

    /// Drive a session over any pair of streams. The seam a test double connects to.
    pub fn over(
        reader: impl std::io::Read + Send + 'static,
        writer: impl Write + Send + 'static,
    ) -> Self {
        let state = Arc::new(Mutex::new(SessionState::Connected));
        let (incoming_sender, incoming) = channel();
        let (outgoing, outgoing_receiver) = channel::<Vec<u8>>();

        Self::spawn_reader(reader, incoming_sender, Arc::clone(&state));
        Self::spawn_writer(writer, outgoing_receiver, Arc::clone(&state));

        Self {
            outgoing,
            incoming,
            state,
            next_request: AtomicU64::new(1),
            frame: AtomicU64::new(0),
        }
    }

    fn spawn_reader(
        mut reader: impl std::io::Read + Send + 'static,
        events: Sender<SessionEvent>,
        state: Arc<Mutex<SessionState>>,
    ) {
        std::thread::Builder::new()
            .name("cy-editor-bridge-read".into())
            .spawn(move || {
                loop {
                    let outcome = read_frame(&mut reader);
                    let problem = match outcome {
                        Ok(Some(payload)) => match Message::decode(&payload) {
                            Ok(message) => {
                                if events.send(SessionEvent::Message(message)).is_err() {
                                    // The editor dropped the session. Nothing to report to.
                                    return;
                                }
                                continue;
                            }
                            Err(problem) => problem,
                        },
                        Ok(None) => Problem::new("the hosted runtime", "it closed the connection")
                            .with_remedy("restart the runtime; your documents are unaffected"),
                        Err(problem) => problem
                            .with_remedy("restart the runtime; your documents are unaffected"),
                    };
                    Self::lose(&state, &events, problem);
                    return;
                }
            })
            .expect("spawning a bridge thread");
    }

    fn spawn_writer(
        mut writer: impl Write + Send + 'static,
        outgoing: Receiver<Vec<u8>>,
        state: Arc<Mutex<SessionState>>,
    ) {
        std::thread::Builder::new()
            .name("cy-editor-bridge-write".into())
            .spawn(move || {
                while let Ok(payload) = outgoing.recv() {
                    if let Err(problem) = write_frame(&mut writer, &payload) {
                        // The reader thread will notice too and is the one that reports; setting
                        // the state here as well is what makes a send-side failure visible even
                        // when the peer has not yet closed its end of the stream.
                        mark_lost(
                            &state,
                            problem
                                .with_remedy("restart the runtime; your documents are unaffected"),
                        );
                        return;
                    }
                }
            })
            .expect("spawning a bridge thread");
    }

    fn lose(state: &Arc<Mutex<SessionState>>, events: &Sender<SessionEvent>, problem: Problem) {
        mark_lost(state, problem.clone());
        let _ = events.send(SessionEvent::Lost(problem));
    }

    /// Where the session is. Cheap enough to ask once per interface frame.
    pub fn state(&self) -> SessionState {
        self.state.lock().map_or_else(
            |_| SessionState::Lost(Problem::new("the bridge", "its state lock was poisoned")),
            |state| state.clone(),
        )
    }

    /// The frame the editor is on. Advanced once per interface frame by the caller.
    #[must_use]
    pub fn frame(&self) -> FrameId {
        FrameId::from_raw(self.frame.load(Ordering::Acquire))
    }

    /// Advance to the next frame, returning it.
    pub fn advance_frame(&self) -> FrameId {
        FrameId::from_raw(self.frame.fetch_add(1, Ordering::AcqRel) + 1)
    }

    /// Enqueue a message. **Does not block and does not wait for a reply.**
    pub fn send(&self, message: &Message) -> Result<()> {
        if let SessionState::Lost(problem) = self.state() {
            return Err(problem);
        }
        self.outgoing.send(message.encode()).map_err(|_| {
            Problem::new("send to the runtime", "the connection's writer has stopped")
                .with_remedy("restart the runtime; your documents are unaffected")
        })
    }

    /// Enqueue an encoded transaction for the hosted world, returning the request to reconcile with.
    ///
    /// The caller passes [`ApplyWhen`] because the caller is what knows whether the world is
    /// playing. See [`ApplyWhen`] for the 168-fold reason that matters.
    pub fn apply(&self, transaction: Vec<u8>, when: ApplyWhen) -> Result<RequestId> {
        let request = RequestId::from_raw(self.next_request.fetch_add(1, Ordering::AcqRel));
        self.send(&Message::Apply {
            request,
            frame: self.frame(),
            when,
            transaction,
        })?;
        Ok(request)
    }

    /// Everything that has arrived since the last call. Never blocks.
    ///
    /// The call an interface frame makes. It returns what is there and nothing else, so a frame's
    /// cost is bounded by what the runtime actually sent rather than by what it might send.
    pub fn poll(&self) -> Vec<SessionEvent> {
        let mut events = Vec::new();
        loop {
            match self.incoming.try_recv() {
                Ok(event) => events.push(event),
                Err(TryRecvError::Empty | TryRecvError::Disconnected) => return events,
            }
        }
    }

    /// Block until `accept` returns a value, or `timeout` elapses.
    ///
    /// **Never call this from an interface thread.** It is named for what it does precisely so that
    /// a reviewer who sees it on a UI path knows without checking. Its purpose is tests and
    /// command-line tools, which have no frame to drop.
    pub fn block_until<T>(
        &self,
        timeout: Duration,
        mut accept: impl FnMut(&SessionEvent) -> Option<T>,
    ) -> Option<T> {
        let deadline = Instant::now() + timeout;
        loop {
            for event in self.poll() {
                if let Some(value) = accept(&event) {
                    return Some(value);
                }
            }
            if Instant::now() >= deadline {
                return None;
            }
            std::thread::sleep(Duration::from_millis(1));
        }
    }
}

/// Record the first loss and ignore every later one.
///
/// The first is the cause; the ones after it are consequences of the first — a write failing
/// because the peer already went away says less than the read that noticed it did. Both the reader
/// and the writer thread call this, and the reader is usually first.
fn mark_lost(state: &Mutex<SessionState>, problem: Problem) {
    let Ok(mut guard) = state.lock() else { return };
    if !guard.is_connected() {
        return;
    }
    *guard = SessionState::Lost(problem);
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A pipe pair, so a session can be driven with no socket and no process.
    fn pipe() -> (std::io::PipeReader, std::io::PipeWriter) {
        std::io::pipe().expect("the platform has pipes")
    }

    #[test]
    fn a_send_does_not_wait_for_a_reply() {
        let (editor_reader, runtime_writer) = pipe();
        let (runtime_reader, editor_writer) = pipe();
        let session = Session::over(editor_reader, editor_writer);

        // Nothing on the other end will ever answer. A send that waited for a reply would
        // therefore wait forever, so the fact that this line is reached at all is the proof — and
        // it is a proof about ordering rather than a race against a clock, which is what keeps it
        // from failing on a loaded machine.
        let started = Instant::now();
        session.apply(vec![1, 2, 3], ApplyWhen::OnArrival).unwrap();
        assert!(
            started.elapsed() < Duration::from_secs(30),
            "a send that waited for a reply nobody will send would be the defect this API shape \
             exists to prevent"
        );
        assert!(
            session.poll().is_empty(),
            "and nothing came back, because nothing was sent back"
        );
        drop((runtime_writer, runtime_reader));
    }

    #[test]
    fn a_reply_is_matched_to_its_request() {
        let (editor_reader, mut runtime_writer) = pipe();
        let (mut runtime_reader, editor_writer) = pipe();
        let session = Session::over(editor_reader, editor_writer);

        let request = session.apply(vec![7], ApplyWhen::OnArrival).unwrap();
        let payload = read_frame(&mut runtime_reader).unwrap().unwrap();
        match Message::decode(&payload).unwrap() {
            Message::Apply {
                request: sent,
                when,
                transaction,
                ..
            } => {
                assert_eq!(sent, request);
                assert_eq!(when, ApplyWhen::OnArrival);
                assert_eq!(transaction, vec![7]);
            }
            other => panic!("expected an Apply, got {other:?}"),
        }

        let echo = Message::Applied {
            request,
            frame: session.frame(),
            observed: vec![7],
        };
        write_frame(&mut runtime_writer, &echo.encode()).unwrap();

        let received = session
            .block_until(Duration::from_secs(2), |event| match event {
                SessionEvent::Message(message) if message.request() == Some(request) => {
                    Some(message.clone())
                }
                _ => None,
            })
            .expect("the echo arrives");
        assert_eq!(received, echo);
    }

    #[test]
    fn a_runtime_that_dies_loses_the_session_and_nothing_else() {
        let (editor_reader, runtime_writer) = pipe();
        let (runtime_reader, editor_writer) = pipe();
        let session = Session::over(editor_reader, editor_writer);
        assert!(session.state().is_connected());

        // The runtime goes away. This is exactly what killing the hosted process looks like from
        // the editor's side: the stream ends.
        drop(runtime_writer);
        drop(runtime_reader);

        let problem = session
            .block_until(Duration::from_secs(2), |event| match event {
                SessionEvent::Lost(problem) => Some(problem.clone()),
                SessionEvent::Message(_) => None,
            })
            .expect("the loss is reported");

        assert!(
            problem
                .remedy
                .as_deref()
                .unwrap()
                .contains("documents are unaffected")
        );
        assert!(!session.state().is_connected());

        // And a send after the loss reports the same reason rather than panicking.
        let refused = session.apply(vec![1], ApplyWhen::OnArrival).unwrap_err();
        assert!(refused.remedy.is_some());
    }

    #[test]
    fn frames_advance_and_are_carried_on_every_request() {
        let (editor_reader, runtime_writer) = pipe();
        let (mut runtime_reader, editor_writer) = pipe();
        let session = Session::over(editor_reader, editor_writer);

        session.advance_frame();
        let expected = session.advance_frame();
        session.apply(vec![], ApplyWhen::AtTickBoundary).unwrap();

        let payload = read_frame(&mut runtime_reader).unwrap().unwrap();
        match Message::decode(&payload).unwrap() {
            Message::Apply { frame, .. } => assert_eq!(frame, expected),
            other => panic!("expected an Apply, got {other:?}"),
        }
        drop(runtime_writer);
    }
}
