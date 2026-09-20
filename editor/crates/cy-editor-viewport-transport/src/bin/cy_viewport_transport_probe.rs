//! The headless editor: the same import, wait, claim and release code the window will run, with no
//! window.
//!
//! A thin wrapper selecting the Linux Vulkan/dma-buf or macOS Metal/IOSurface implementation. The
//! workspace still builds on platforms where neither native mechanism exists.

fn main() {
    #[cfg(target_os = "linux")]
    cy_editor_viewport_transport::probe::main();
    #[cfg(target_os = "macos")]
    cy_editor_viewport_transport::darwin::probe_main();
    #[cfg(not(any(target_os = "linux", target_os = "macos")))]
    eprintln!("{}", cy_editor_viewport_transport::UNSUPPORTED);
}
