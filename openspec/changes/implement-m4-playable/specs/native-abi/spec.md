## MODIFIED Requirements

### Requirement: Hot reload
Modules declared hot-reloadable SHALL support reload without restarting the engine.

Reload SHALL: quiesce at a frame boundary, serialize live instances of module-owned types through
the vtable of the generation that created them, destroy those instances through that same vtable,
call `shutdown` on the outgoing image, **retire that image**, load the new library, call
`initialize`, and restore instances through schema migration by field name.

**The outgoing image SHALL NOT be unloaded in a development build.** It is retired: its vtables are
dropped from the host's lookup so that no stale entry is reachable by name, and the image itself
stays mapped for the process lifetime. Only a Shipping build — which supports no reload at all —
ever unloads a module.

This replaces the earlier "unload the library" step, and it is a **measured** correction rather than
a preference. M4's spike (`openspec/changes/implement-m4-playable/tasks.md` §0) established three
facts on Linux with Swift 6.3.3, each of which independently forbids the unload:

| Measurement | Consequence |
|---|---|
| `dlclose` really unmaps, and the **next** image is mapped over the same addresses ("stale address mapped = YES (reused!)") | A stale call is a jump into unrelated live code, not a reliable fault |
| Foreign (imported C) type metadata is interned process-wide **by name**, holding the first image's `rodata` string; after that image is unloaded the next module naming the same C type dies in `__strlen_avx2` ← `swift_getForeignTypeMetadata` | Unloading corrupts a cache the runtime, not the module, owns |
| The protocol-conformance section list keeps the unloaded image's section, so a **failing** conformance lookup walks freed memory and dies in `swift_conformsToProtocolMaybeInstantiateSuperclasses` | The crash appears in code that never mentioned the unloaded module |

A 20-cycle `dlclose` test **passed** when the two images happened to be the same size, because the
new image landed at the same address and the dangling name string happened to match — so a green
result from an unloading implementation is not evidence that unloading works.

Every live instance handle SHALL carry the generation that created it, and the host SHALL resolve
create, destroy, update and serialize through that generation's vtable, never through the current
one. Measured consequence of not doing so: version-2 code running against a version-1 object
reported `health = 17` (the old object's `ammo`), `shield` as an entity id, and `mana` as the raw bit
pattern of a string field — with no trap and no diagnostic.

Each generation SHALL be loaded from a **distinct file path**, and a Swift module SHALL additionally
be compiled with a **unique `-module-name` per generation**. A unique filename alone is insufficient:
name-based type lookup (`swift_getTypeByMangledNameInEnvironment`, `_typeByName`, `Codable`,
reflection) is process-global and first-registration-wins, and with two resident images both named
`CyGame` the **new** image asking for its own type received the **old** image's metadata.

The cost of retaining images SHALL be documented rather than hidden: 58-85 kB of virtual address
space per reload, never reclaimed, with reload itself measured at 0.1-0.6 ms and flat across 40
generations. A session performing a thousand reloads spends under 90 MB of address space; the
mitigation, if it is ever needed, is a process restart and not a `dlclose`.

These findings are ELF, glibc and Swift-on-Linux specific. Windows and macOS are **unverified** and
SHALL be re-measured before this requirement is relied on there.

#### Scenario: Behaviour reload preserves state
- **WHEN** a Swift behaviour is edited and its module reloaded
- **THEN** entity state SHALL be preserved and the new code SHALL run against it from the next
  frame

#### Scenario: Incompatible reload
- **WHEN** the new module removes a component type that live entities still use, or its schema
  predates the serialized state
- **THEN** the reload SHALL be rejected with a diagnostic, the previous generation SHALL remain
  live, and every live instance SHALL remain valid

#### Scenario: The outgoing image is retired, not unloaded
- **WHEN** a hot reload completes in a development build
- **THEN** the outgoing image SHALL remain mapped, its registrations SHALL no longer be reachable by
  name, and an instance it created SHALL still be destroyable through its own vtable

#### Scenario: A generation resolves its own vtable
- **WHEN** an instance created by generation *N* is updated or destroyed while generation *N + k* is
  live
- **THEN** the call SHALL be made through generation *N*'s vtable, and the value read SHALL be the
  value that was written

#### Scenario: Reload is refused before anything is destroyed
- **WHEN** the new library cannot be opened, or its entry point returns false, or a live type is
  absent from it
- **THEN** no live instance SHALL have been destroyed, the generation counter SHALL be unchanged,
  and the failure SHALL be reported with the offending type named
