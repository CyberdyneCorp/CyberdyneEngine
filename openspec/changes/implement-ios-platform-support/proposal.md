# Native iOS platform support

## Why

Cyberdyne can render through Metal on macOS, but the build rejects
`CMAKE_SYSTEM_NAME=iOS`, has no iOS platform implementation, and produces no installable mobile
application. A physical iPhone and a signed external spike now prove that the Xcode toolchain,
SDL3's UIKit backend, Metal, signing, installation, and launch are available on this host.

## What changes

- Add documented CMake toolchains for physical iOS devices and the iOS simulator.
- Recognise iOS in module discovery and exclude desktop-only targets from mobile builds.
- Add an iOS platform module with application lifecycle, one fullscreen display, touch input,
  sandbox directories, clocks, memory reporting, and a `CAMetalLayer` surface.
- Make the Metal backend and its portable tests build for iOS.
- Add a small signed sample application that starts the engine, renders through Metal, and exposes
  a device-verifiable success marker.
- Add host-side contract tests and real-device build/install/launch evidence.

## Scope

This change establishes a supported native iOS target and one visible Metal frame. Advanced mobile
rendering policy, App Store distribution, TestFlight upload, and the complete M11.e capability
sweep remain separate work.
