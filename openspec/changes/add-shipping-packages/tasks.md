# Tasks — distributable game builds

## 1. The signed manifest

- [x] 1.1 Monocypher 4.0.2 in `deps/manifest.toml`, provided by `cy__provide_monocypher`, attributed in THIRD_PARTY.md
- [x] 1.2 `cy::ship` signatures, checked against RFC 8032's test vectors
- [x] 1.3 The manifest's text form, its parser refusing paths outside the artefact and chunks that do not tile the stream
- [x] 1.4 `pack`: content cut into chunks, every file listed, the manifest signed
- [x] 1.5 `verify_installation`: signature first, then every listed file, then every unlisted one — each named

## 2. The launch

- [x] 2.1 `cy::ship::start`: locate, verify, materialise, crash handler, settings, default arguments
- [x] 2.2 The RTS, the first-person and the third-person hosts and samples/11-ship start through it
- [x] 2.3 `CY_SHIP_REQUIRE_INSTALLATION` refuses a run outside an installation
- [x] 2.4 The Swift runtime linked into a Shipping module on Linux

## 3. The artefacts

- [x] 3.1 `just package <game> <target>`, `just release-games`, `just package-test`
- [x] 3.2 Linux tarball (stripped, symbols archived), Windows zip, macOS ad-hoc signed bundle
- [x] 3.3 CI `package` job on linux-x86_64, macos-arm64 and windows-x86_64, artefacts uploaded

## 4. Evidence

- [x] 4.1 integration.ship_manifest — signatures, manifest, tampering, missing and unlisted files, launch, settings
- [x] 4.2 smoke.ship_games — each game unpacked into a clean directory, its digests held to the build tree's, refusals and a crash report
- [x] 4.3 docs/guides/shipping.md, tools/ship/README.md, requirements-coverage.toml
