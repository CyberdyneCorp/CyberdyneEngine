# Release assembly

`release.py` implements the four `just release-*` recipes. It builds through CMake and Cargo,
then assembles five deterministic archives:

| Component | Contents |
|---|---|
| `editor` | The CyberEditor application |
| `runtime` | Embeddable engine libraries, public headers and the ABI description |
| `sdk` | The self-contained `CyberdyneKit` Swift package and C ABI |
| `tools` | Build service, cooker, importer, material packager and shader compilers |
| `templates` | The platform/configuration runtime host and packaged-project template |

Every archive contains `provenance.json`, including version, revision, platform, architecture,
configuration and a SHA-256 record for every payload. The release manifest hashes the archives.
Archive timestamps and ownership are normalized, so assembling unchanged inputs twice produces the
same bytes.

```sh
just release-version
just release-version --set 0.5.0
just release-changelog --version 0.5.0
just release-artefacts --version 0.5.0
just release-publish --manifest dist/releases/0.5.0/*-manifest.json
just release-check
```

`release-publish` verifies every artifact and defaults to printing the exact draft GitHub release
command. Publication requires both `--execute` and `--confirm v<version>`; this keeps inspection and
the irreversible upload as separate steps.

`release-check` drives all four operations with the declared source version in a temporary
directory. It performs a real Shipping build and is the end-to-end release gate.

For development builds, `--skip-build`, `--build-dir`, `--editor-dir`, and `--configuration` let
the assembler validate existing outputs without presenting them as Shipping artifacts.
