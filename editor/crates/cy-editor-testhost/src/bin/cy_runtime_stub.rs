//! `cy-runtime-stub` — a hosted runtime, as a separate process.
//!
//! It exists so that "kill the hosted runtime mid-session and the editor survives with the document
//! intact" can be tested by killing a **process**, which is the thing the claim is about. A test
//! that dropped a socket would prove the protocol handles an end of stream; only a test that kills
//! a process proves the editor's fate is not tied to the runtime's.
//!
//! What it is not: an engine. It holds no world, and it echoes what it is asked to apply. The real
//! hosted runtime is a C++ binary over `cy::abi::Host`, and it arrives with `live-editing` at task
//! 5.2 — speaking this same message set, over this same committed encoding, so the editor's half of
//! the test does not change when it does.
//!
//! ```text
//! cy-runtime-stub <socket-path>
//! ```

#![forbid(unsafe_code)]

use std::process::ExitCode;

use cy_editor_protocol::{Message, serve};

fn main() -> ExitCode {
    let Some(path) = std::env::args().nth(1) else {
        eprintln!("cy-runtime-stub: usage: cy-runtime-stub <socket-path>");
        return ExitCode::FAILURE;
    };

    #[cfg(unix)]
    {
        use std::io::Write as _;
        use std::os::unix::net::UnixListener;

        let _ = std::fs::remove_file(&path);
        let listener = match UnixListener::bind(&path) {
            Ok(listener) => listener,
            Err(error) => {
                eprintln!("cy-runtime-stub: cannot listen on {path}: {error}");
                return ExitCode::FAILURE;
            }
        };
        // Printed and flushed so that a parent process can wait for readiness rather than sleeping.
        println!("listening");
        let _ = std::io::stdout().flush();

        for stream in listener.incoming() {
            let Ok(stream) = stream else { continue };
            let Ok(mut reader) = stream.try_clone() else {
                continue;
            };
            let mut writer = stream;
            let _ = serve(&mut reader, &mut writer, |message| match message {
                Message::Hello { .. } => Some(vec![Message::Welcome {
                    abi_major: cy_editor_sdk::abi::MAJOR,
                    abi_minor: cy_editor_sdk::abi::MINOR,
                    runtime: format!("cy-runtime-stub {}", env!("CARGO_PKG_VERSION")),
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
                Message::Ping { frame } => Some(vec![Message::Pong { frame }]),
                _ => Some(Vec::new()),
            });
        }
        ExitCode::SUCCESS
    }

    #[cfg(not(unix))]
    {
        eprintln!("cy-runtime-stub: this stub listens on a Unix domain socket only");
        ExitCode::FAILURE
    }
}
