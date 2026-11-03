# `tools/ship` — distributable game builds

Issue #87, the packaging half of M11.e's "Ship". The guide is
[`docs/guides/shipping.md`](../../docs/guides/shipping.md); this is the reference.

```
just package rts linux                       the RTS, release profile, a tarball in dist/packages/
just package all host --profile dev          every game this tree built, for this host
just release-games                           every game, release profile only, dist/releases/<v>/games
just package-test                            smoke.ship_games: each artefact launched from a clean dir
```

| Part | What it is |
|---|---|
| `include/cy/ship/signature.h` | Ed25519 (RFC 8032) from Monocypher; the key this build trusts; the published development key |
| `include/cy/ship/manifest.h` | `ship.manifest`: every file by size and BLAKE3, the content chunks, the content entries |
| `include/cy/ship/pack.h` | content cut into 4 MiB chunks, every file of the staged root listed, the manifest signed |
| `include/cy/ship/installation.h` | the startup check, and the content materialised into the user's cache |
| `include/cy/ship/settings.h` | the settings file in the user's configuration directory |
| `include/cy/ship/launch.h` | what a game's `main()` calls first: `cy::ship::start` |
| `src/main.cpp` | `cy_ship pack \| verify \| read \| keygen \| public-key \| trusted` |
| `package.py` | stages a game for a target, runs `cy_ship pack`, archives it |
| `test_ship_games.py` | `smoke.ship_games` |
| `tests/test_ship.cpp` | `integration.ship_manifest` |

## An artefact

```
CyberdyneRTS-0.5.0-linux-x86_64/
  bin/cyberdyne-rts              the host, stripped (symbols in ...-symbols.tar.gz)
  lib/libCyGame_g0.so            the Swift game module; RUNPATH $ORIGIN
  lib/libswiftCore.so ...        a development module's Swift runtime (none in a release build)
  content/chunk-0000.cychunk     the cooked content stream, cut every 4 MiB
  ship.manifest                  every file above, signed by ship.manifest.sig
  README.txt CHANGELOG.md LICENSE THIRD_PARTY.md
```

Windows is the same tree with `bin/` holding the executable and its DLLs, zipped. macOS is
`CyberdyneRTS.app/Contents/{MacOS,Frameworks,Resources}` with the manifest and the chunks in
`Resources/` (codesign seals what is there and refuses loose files elsewhere), the main
executable listed as `sealed` (codesign rewrites it after the manifest is signed), and the bundle
ad-hoc signed and zipped with `ditto`.

## The startup check

`cy::ship::start(argc, argv, spec, launch)` looks for `ship.manifest` beside the executable's
directory (`../ship.manifest`, or `../Resources/ship.manifest` in a bundle). None: the program is
running out of a build tree and nothing changes. Found:

1. the signature is verified against the key compiled into the build — and nothing else in the
   manifest is read until it holds;
2. every listed file is checked for presence, size and digest, and every file present but not
   listed is reported; any problem stops the game with exit 3, each file named;
3. the content entries are written out of the chunks into
   `<user cache>/content/<manifest digest>/`, re-verified on every later launch;
4. the crash handler writes to `<user data>/crashes/`;
5. `<user config>/settings.cfg` is read, or written with its defaults, and becomes flags ahead of
   the player's own;
6. a game started with no arguments gets its defaults (`--play`).

The user directories are the SDL3 platform's (`~/.local/share/CyberdyneEngine/<Game>/` on Linux,
`%APPDATA%\CyberdyneEngine\<Game>\` on Windows, `~/Library/Application Support/CyberdyneEngine/<Game>/`
on macOS), or `$CY_SHIP_USER_DIR/<Game>/` when that is set.

## Keys

| | |
|---|---|
| Default | the published development key (`trusted=development` on every launch); anyone can sign with it |
| A release key | `cy_ship keygen --out release.key` prints the public half; configure with `-D CY_SHIP_PUBLIC_KEY=<hex>` |
| Signing | `just package ... --key release.key`, or `CY_SHIP_SIGNING_KEY=<secret hex>` in the environment (CI secrets) |

A build configured with a release key refuses content the development key signed.
`-D CY_SHIP_REQUIRE_INSTALLATION=ON` makes a game refuse to start outside a verified installation.

## What it is not

Not anti-tamper or anti-cheat: the check runs inside the executable, and the executable's own
integrity is the platform code signature's (macOS codesign, Windows Authenticode). Not encryption.
Not notarized (an Apple Developer ID is needed; the guide has the steps). Not an AppImage.

## Why it is in tools/

The games it packages are tools-layer samples, and the check they start with lives beside the packer
that writes what it checks. It should move to `src/runtime/` when a game outside `samples/` ships,
with the user directories taken from the game's own `Platform` rather than a short-lived SDL3 one.
