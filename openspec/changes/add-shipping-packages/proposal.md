# Distributable game builds: signed, chunked, self-contained

## Why

The RTS, the first-person and the third-person games are playable in a window (#144, #145, #150),
and none of them can be handed to anybody. Each reads its module, its map and its textures from
absolute paths into the build tree, so a copy of the executable on another machine finds nothing.
`build-and-packaging`'s "Manifest integrity and content protection" has been an exemption since
M11.d: content is hashed and verified, but no manifest is signed and nothing refuses a modified one.
`build-system-and-platforms`' "Distribution artefacts" asks for a game packaged by combining a
runtime with its cooked content and its Swift module, and no case did. Issue #87 and M11.e's "Ship"
rung both name the gap.

## What changes

- **`just package <game> <target>`** (`tools/ship/package.py`) packages `rts`, `fps`, `tps` and
  `ship` (samples/11-ship), or `all`, for `linux` (a tarball, stripped, symbols in a separate
  archive), `windows` (a zip) or `macos` (an ad-hoc signed `.app` in a zip). A target is packaged on
  its own operating system from the tree that host built.
- **Cooked content is cut into chunks** (4 MiB) under `content/`, and **one text manifest**,
  `ship.manifest`, lists every file of the artefact by size and BLAKE3 digest, every chunk, and
  every content entry's offset, size and digest in the chunk stream.
- **The manifest is signed** with Ed25519 (RFC 8032) from Monocypher, a new dependency
  (`deps/manifest.toml`). A build trusts one public key, `-D CY_SHIP_PUBLIC_KEY=<hex>`; with none it
  trusts a published development key, and says so on every launch.
- **A packaged game verifies itself at startup** (`cy::ship::start`, `tools/ship/`): signature
  first, then every file — a modified chunk, a missing file, a file the manifest does not list, or a
  signature by an untrusted key stops the game with exit 3 and the file named. Then the content is
  materialised into the user's cache directory, the crash handler is installed into the user's data
  directory, and a settings file is read from (or written to) the user's configuration directory.
- **The Swift runtime ships inside the module** in the Shipping configuration on Linux
  (`cy_swift_module.py --static-stdlib`, issue #81's shipping static link); a development module's
  runtime libraries are copied beside it with `RUNPATH $ORIGIN`.
- **CI packages the three desktop targets** from the build each leg already made, and uploads the
  artefacts (`package` job).
- **The version and changelog come from `tools/release/release.py`**, and `just release-games`
  packages every game from the release profile and refuses any other.

## What this is not

- Not anti-tamper or anti-cheat. The check runs inside the executable it would have to distrust;
  the executable's own integrity is the platform code signature's job.
- Not notarized: notarization needs an Apple Developer ID and credentials this project does not
  hold. The guide documents the steps.
- Not an AppImage: `appimagetool` is not part of the toolchain; the tarball is the Linux artefact.
- Not a Windows game artefact yet: the Windows CI leg has no Swift toolchain, so only
  `samples/11-ship` is packaged there.
