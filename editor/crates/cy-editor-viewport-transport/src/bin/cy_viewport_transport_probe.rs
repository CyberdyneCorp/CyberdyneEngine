//! The headless editor: the same import, wait, claim and release code the window will run, with no
//! window.
//!
//! A thin wrapper over `cy_editor_viewport_transport::probe`, for the reason its sibling gives: the
//! mechanism is Linux's, and the workspace is built on three platforms.

fn main() {
    #[cfg(target_os = "linux")]
    cy_editor_viewport_transport::probe::main();
    #[cfg(not(target_os = "linux"))]
    eprintln!("{}", cy_editor_viewport_transport::UNSUPPORTED);
}
