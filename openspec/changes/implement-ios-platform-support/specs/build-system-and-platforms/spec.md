## ADDED Requirements

### Requirement: iOS cross-compilation

The build system SHALL provide documented CMake toolchains for physical iOS devices and the iOS
simulator, and an iOS configure SHALL select only modules supported by the target platform.

#### Scenario: Physical-device build

- **WHEN** a developer configures with the iOS device toolchain and an arm64 iPhoneOS SDK
- **THEN** the engine SHALL compile without requiring the host and target operating systems to match
- **AND** desktop-only modules SHALL be excluded from the target graph

#### Scenario: Signing remains application-owned

- **WHEN** the engine libraries are cross-compiled for iOS
- **THEN** they SHALL NOT require a signing identity
- **AND** an installable application SHALL accept its team and bundle identity as build inputs
