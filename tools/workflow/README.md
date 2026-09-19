# tools/workflow/ — target-platform selection, as a table rather than an `if`

M11.d task 7.7. `developer-workflow-and-just`:

> The workflow SHALL support **selecting a target platform** for build, test, deploy, and package
> recipes, and SHALL report clearly when a target cannot be built on the current host and why.
>
> **Scenario: An impossible target is explained** — WHEN a target cannot be built on the current
> host, THEN the workflow SHALL say so and state what is required.

## What was here before

One `if` in `just/build.just`'s `_resolve-target`, and one sentence:

> This milestone builds for the host only. Cross-compilation needs a CMake toolchain file and a
> platform SDK, and the first of those arrive with the mobile targets at M9.

Three problems, and the stale milestone number is the least of them. It said **the same words**
about `macos` (the engine supports it; this host cannot produce it), about `android` (no port exists
in the tree at all) and about `windwos` (a misspelling) — so a developer could not tell "wrong
machine" from "not written yet" from "you typed it wrong". And only `build-engine` took the flag:
`test-*`, `content-package` and any deploy recipe ignored it or passed it to a tool that would have
failed naming an option.

## What is here now

| Piece | What it decides |
|---|---|
| `just/targets.toml` | the targets, their hosts, what each needs, and **which rung owns** each unwritten one |
| `targets.py` | reads the table, resolves a request, prints the listing, and proves its own refusals |
| `just env-targets` | the table with this host's verdict on every row |
| `just env-targets --selftest` | the refusals' negative cases |

`build-engine`, `_ctest` (so every `test-*` recipe), `content-package` and `deploy-install` all
resolve through the same function, so `just build-engine --platform ios` and `just test-unit
--platform ios` cannot give different answers.

### The three refusals are deliberately different, in words and in exit status

| Request | Says | Exit |
|---|---|---|
| `windwos` | there is no such target, and lists the ones that exist | **3** |
| `macos` from Linux | the engine supports it, this host cannot produce it, what a cross-build would need, and which hosts can | **2** |
| `android` | no port exists in this tree, what one would need, and **the rung that writes it** | **2** |

Different exit statuses because a caller should be able to tell a typo from a machine limit without
parsing prose.

### The guard that stops this becoming `just/release.just`

`just/release.just`'s four recipes refuse naming *"M12 — build-and-packaging"*, **a milestone that
does not exist on a ladder whose `record.MILESTONES` ends at `m11e`** — so a developer who reads
that refusal is told to wait for nothing. This table's `owner` field is checked against
`record.MILESTONES` **at load**, and a target owned by a rung that is not on the ladder fails the
selftest and every recipe that resolves a target. M11.d task 7.8 is the same rule pointed at the
release recipes; this is it enforced mechanically for targets.

## What `deploy` does and does not do

`just deploy-install` is real: `cy_build install` into an installation root followed by
`cy_build verify`, which re-digests every chunk the installed build names. The verification is not a
second recipe on purpose — an install that succeeded while writing a chunk whose digest does not
match is exactly what an installation root exists to catch.

`just deploy-device` **refuses**, naming what is required (a transport; `RemoteFileProvider` is the
seam) and the rung that owns it (M11.e). A recipe that pretended to deploy to a device by copying a
directory would be the ninth check in this repository that cannot fail.
