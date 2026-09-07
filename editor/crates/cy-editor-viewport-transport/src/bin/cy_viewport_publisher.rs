//! The reference publisher: the runtime's half of the viewport transport, in a real second process.
//!
//! A thin wrapper, because the code it runs lives in the library — `cy_editor_viewport_transport::publisher`.
//! That is not tidiness: the transport is dma-buf, `memfd` and `OPAQUE_FD`, none of which exists on
//! macOS or Windows, and the editor's workspace is built on all three. Keeping the platform code in
//! a module the library can decline to compile is what lets this crate be a workspace member
//! everywhere and a working program where the mechanism exists.

fn main() {
    #[cfg(target_os = "linux")]
    cy_editor_viewport_transport::publisher::main();
    #[cfg(not(target_os = "linux"))]
    eprintln!("{}", cy_editor_viewport_transport::UNSUPPORTED);
}
