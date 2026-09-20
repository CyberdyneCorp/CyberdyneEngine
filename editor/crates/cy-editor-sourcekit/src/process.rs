//! Process and LSP framing boundary.

use std::io::{BufRead, BufReader, Write};
use std::process::{Child, ChildStdin, Command, Stdio};
use std::sync::mpsc::{self, Receiver, RecvTimeoutError};
use std::thread;
use std::time::Duration;

/// A failure reported by the process transport.
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum ProcessFailure {
    /// The process or one of its pipes stopped.
    Stopped,
    /// No message arrived before the deadline.
    Timeout,
    /// Framing or I/O failed with this explanation.
    Invalid(String),
}

/// Message-oriented process transport used by the client.
///
/// Implementations own JSON-RPC framing. This small boundary lets tests use a recording process
/// without spawning a shell or depending on platform-specific process-control crates.
pub trait LanguageServerProcess: Send {
    /// Send one unframed JSON message.
    fn send(&mut self, message: &str) -> Result<(), ProcessFailure>;
    /// Receive one unframed JSON message.
    fn receive(&mut self, timeout: Duration) -> Result<String, ProcessFailure>;
    /// Stop and reap the process.
    fn stop(&mut self);
}

/// Creates a process, allowing unavailable-toolchain failures to remain structured.
pub trait ProcessFactory {
    /// Start the configured language server.
    fn start(&self) -> Result<Box<dyn LanguageServerProcess>, String>;
}

/// Factory for an installed `sourcekit-lsp` executable.
#[derive(Clone, Debug)]
pub struct CommandProcessFactory {
    executable: String,
    arguments: Vec<String>,
}

impl CommandProcessFactory {
    /// Use the executable resolved from `PATH`.
    #[must_use]
    pub fn sourcekit_lsp() -> Self {
        Self {
            executable: "sourcekit-lsp".to_string(),
            arguments: Vec::new(),
        }
    }

    /// Use a particular executable and argument list.
    #[must_use]
    pub fn new(
        executable: impl Into<String>,
        arguments: impl IntoIterator<Item = impl Into<String>>,
    ) -> Self {
        Self {
            executable: executable.into(),
            arguments: arguments.into_iter().map(Into::into).collect(),
        }
    }
}

impl ProcessFactory for CommandProcessFactory {
    fn start(&self) -> Result<Box<dyn LanguageServerProcess>, String> {
        StdProcess::spawn(&self.executable, &self.arguments)
            .map(|process| Box::new(process) as Box<dyn LanguageServerProcess>)
    }
}

struct StdProcess {
    child: Child,
    input: ChildStdin,
    messages: Receiver<Result<String, ProcessFailure>>,
}

impl StdProcess {
    fn spawn(executable: &str, arguments: &[String]) -> Result<Self, String> {
        let mut child = Command::new(executable)
            .args(arguments)
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::null())
            .spawn()
            .map_err(|error| error.to_string())?;
        let input = child
            .stdin
            .take()
            .ok_or_else(|| "the process exposed no standard input".to_string())?;
        let output = child
            .stdout
            .take()
            .ok_or_else(|| "the process exposed no standard output".to_string())?;
        let (sender, messages) = mpsc::channel();
        thread::spawn(move || {
            let mut reader = BufReader::new(output);
            loop {
                match read_frame(&mut reader) {
                    Ok(message) => {
                        if sender.send(Ok(message)).is_err() {
                            break;
                        }
                    }
                    Err(failure) => {
                        let _ = sender.send(Err(failure));
                        break;
                    }
                }
            }
        });
        Ok(Self {
            child,
            input,
            messages,
        })
    }
}

impl LanguageServerProcess for StdProcess {
    fn send(&mut self, message: &str) -> Result<(), ProcessFailure> {
        write!(
            self.input,
            "Content-Length: {}\r\n\r\n{message}",
            message.len()
        )
        .and_then(|()| self.input.flush())
        .map_err(|_| ProcessFailure::Stopped)
    }

    fn receive(&mut self, timeout: Duration) -> Result<String, ProcessFailure> {
        match self.messages.recv_timeout(timeout) {
            Ok(result) => result,
            Err(RecvTimeoutError::Timeout) => Err(ProcessFailure::Timeout),
            Err(RecvTimeoutError::Disconnected) => Err(ProcessFailure::Stopped),
        }
    }

    fn stop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

impl Drop for StdProcess {
    fn drop(&mut self) {
        self.stop();
    }
}

fn read_frame(reader: &mut impl BufRead) -> Result<String, ProcessFailure> {
    let mut content_length = None;
    loop {
        let mut header = String::new();
        let read = reader
            .read_line(&mut header)
            .map_err(|error| ProcessFailure::Invalid(error.to_string()))?;
        if read == 0 {
            return Err(ProcessFailure::Stopped);
        }
        if header == "\r\n" || header == "\n" {
            break;
        }
        if let Some(value) = header
            .trim_end()
            .strip_prefix("Content-Length:")
            .map(str::trim)
        {
            content_length = Some(
                value
                    .parse::<usize>()
                    .map_err(|_| ProcessFailure::Invalid("invalid Content-Length".to_string()))?,
            );
        }
    }
    let length = content_length
        .ok_or_else(|| ProcessFailure::Invalid("missing Content-Length".to_string()))?;
    let mut body = vec![0; length];
    reader
        .read_exact(&mut body)
        .map_err(|_| ProcessFailure::Stopped)?;
    String::from_utf8(body).map_err(|error| ProcessFailure::Invalid(error.to_string()))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn reads_lsp_content_length_frame() {
        let body = r#"{"jsonrpc":"2.0"}"#;
        let bytes = format!("Content-Length: {}\r\n\r\n{body}", body.len());
        assert_eq!(read_frame(&mut bytes.as_bytes()), Ok(body.to_string()));
    }
}
