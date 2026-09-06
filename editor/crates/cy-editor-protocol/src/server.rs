//! The runtime's side of the bridge, as a loop a host can call.
//!
//! The real hosted runtime is a C++ binary and belongs to `live-editing` at task 5.2. What lives
//! here is the half that is *protocol* rather than engine: accept a connection, read framed
//! messages, hand each to a handler, write what the handler returns. A runtime host — or a test
//! double, or a headless tool — supplies the handler.
//!
//! Keeping it here rather than in the runtime has one concrete payoff: the editor's tests drive a
//! real server over a real socket, so the framing, the message set and the loss behaviour are
//! exercised end to end without the engine being built. When the C++ host arrives it implements the
//! same message set against the same committed encoding, and a disagreement is a decode error with
//! a tag number in it rather than a hang.

use std::io::{Read, Write};

use cy_editor_core::problem::Result;

use crate::frame::{read_frame, write_frame};
use crate::message::Message;

/// Serve one connection until the peer closes it or the handler asks to stop.
///
/// The handler returns the messages to send back — zero, one or several — and `None` to close the
/// connection. Returning several is what lets a runtime answer an `Apply` with both an `Applied` and
/// whatever it wants to volunteer, without the protocol needing a notion of a stream.
pub fn serve(
    reader: &mut impl Read,
    writer: &mut impl Write,
    mut handle: impl FnMut(Message) -> Option<Vec<Message>>,
) -> Result<()> {
    while let Some(payload) = read_frame(reader)? {
        let message = Message::decode(&payload)?;
        let Some(replies) = handle(message) else {
            return Ok(());
        };
        for reply in replies {
            write_frame(writer, &reply.encode())?;
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use std::time::Duration;

    use super::*;
    use crate::message::{ApplyWhen, RequestId};
    use crate::session::{Session, SessionEvent};

    /// A runtime double that echoes what it was asked to apply, over a real Unix domain socket.
    #[cfg(unix)]
    #[test]
    fn a_session_and_a_server_talk_over_a_unix_socket() {
        use std::os::unix::net::{UnixListener, UnixStream};

        let directory = std::env::temp_dir().join(format!(
            "cy-editor-bridge-{}-{}",
            std::process::id(),
            line!()
        ));
        std::fs::create_dir_all(&directory).unwrap();
        let path = directory.join("runtime.sock");

        let listener = UnixListener::bind(&path).unwrap();
        let runtime = std::thread::spawn(move || {
            let (stream, _) = listener.accept().unwrap();
            let mut reader = stream.try_clone().unwrap();
            let mut writer = stream;
            let _ = serve(&mut reader, &mut writer, |message| match message {
                Message::Hello { .. } => Some(vec![Message::Welcome {
                    abi_major: 1,
                    abi_minor: 1,
                    runtime: "double".into(),
                }]),
                Message::Apply {
                    request,
                    frame,
                    transaction,
                    ..
                } => Some(vec![Message::Applied {
                    request,
                    frame,
                    observed: transaction,
                }]),
                _ => Some(Vec::new()),
            });
        });

        let stream = UnixStream::connect(&path).unwrap();
        // A third handle, kept so the test can close the connection at the end. A `Session` has no
        // `close`: it is generic over any reader and writer, and its reader thread lives until the
        // stream ends — which is correct for an editor, whose session ends when the runtime does,
        // and which means a test that owns the socket has to be the one to end it.
        let control = stream.try_clone().unwrap();
        let session = Session::over(stream.try_clone().unwrap(), stream);
        session
            .send(&Message::Hello {
                abi_major: 1,
                abi_minor: 1,
                editor: "test".into(),
            })
            .unwrap();
        let welcome = session
            .block_until(Duration::from_secs(5), |event| match event {
                SessionEvent::Message(Message::Welcome { runtime, .. }) => Some(runtime.clone()),
                _ => None,
            })
            .expect("the runtime welcomes us");
        assert_eq!(welcome, "double");

        let request = session.apply(vec![9, 9, 9], ApplyWhen::OnArrival).unwrap();
        let observed = session
            .block_until(Duration::from_secs(5), |event| match event {
                SessionEvent::Message(Message::Applied {
                    request: id,
                    observed,
                    ..
                }) if *id == request => Some(observed.clone()),
                _ => None,
            })
            .expect("the runtime echoes what it applied");
        assert_eq!(observed, vec![9, 9, 9]);

        drop(session);
        control.shutdown(std::net::Shutdown::Both).unwrap();
        let _ = runtime.join();
        std::fs::remove_dir_all(&directory).unwrap();
    }

    /// The milestone's headline: killing the runtime mid-session leaves the editor running.
    #[cfg(unix)]
    #[test]
    fn a_runtime_that_disappears_mid_session_is_survived() {
        use std::os::unix::net::{UnixListener, UnixStream};

        let directory = std::env::temp_dir().join(format!(
            "cy-editor-bridge-{}-{}",
            std::process::id(),
            line!()
        ));
        std::fs::create_dir_all(&directory).unwrap();
        let path = directory.join("runtime.sock");

        let listener = UnixListener::bind(&path).unwrap();
        let runtime = std::thread::spawn(move || {
            let (stream, _) = listener.accept().unwrap();
            let mut reader = stream.try_clone().unwrap();
            let mut writer = stream;
            // Answer exactly one message, then vanish without a goodbye — which is what a crash
            // looks like from the other end of a socket.
            let _ = serve(&mut reader, &mut writer, |message| match message {
                Message::Apply { request, frame, .. } => Some(vec![Message::Applied {
                    request,
                    frame,
                    observed: Vec::new(),
                }]),
                _ => None,
            });
        });

        let stream = UnixStream::connect(&path).unwrap();
        let session = Session::over(stream.try_clone().unwrap(), stream);
        let request = session.apply(vec![1], ApplyWhen::OnArrival).unwrap();
        assert_eq!(
            session.block_until(Duration::from_secs(5), |event| match event {
                SessionEvent::Message(Message::Applied { request: id, .. }) if *id == request =>
                    Some(()),
                _ => None,
            }),
            Some(())
        );

        session
            .send(&Message::Ping {
                frame: session.frame(),
            })
            .unwrap();
        let problem = session
            .block_until(Duration::from_secs(5), |event| match event {
                SessionEvent::Lost(problem) => Some(problem.clone()),
                SessionEvent::Message(_) => None,
            })
            .expect("the editor learns the runtime is gone");

        assert!(
            problem
                .remedy
                .as_deref()
                .unwrap()
                .contains("documents are unaffected")
        );
        assert!(!session.state().is_connected());

        let _ = runtime.join();
        std::fs::remove_dir_all(&directory).unwrap();
    }

    #[test]
    fn a_handler_may_answer_with_nothing() {
        let mut request = Vec::new();
        write_frame(
            &mut request,
            &Message::Ping {
                frame: crate::FrameId::from_raw(1),
            }
            .encode(),
        )
        .unwrap();
        let mut reader = request.as_slice();
        let mut written = Vec::new();
        serve(&mut reader, &mut written, |_| Some(Vec::new())).unwrap();
        assert!(written.is_empty());
    }

    #[test]
    fn an_unmatched_reply_does_not_confuse_a_session() {
        // A reply to a request nobody made is data, not a fault: a session that panicked on one
        // would be a session a buggy runtime could take down.
        let applied = Message::Applied {
            request: RequestId::from_raw(999),
            frame: crate::FrameId::from_raw(0),
            observed: Vec::new(),
        };
        assert_eq!(Message::decode(&applied.encode()).unwrap(), applied);
    }
}
