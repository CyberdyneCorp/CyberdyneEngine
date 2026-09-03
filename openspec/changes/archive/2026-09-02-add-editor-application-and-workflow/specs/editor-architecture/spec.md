## MODIFIED Requirements

### Requirement: Editor is an engine application
The editor SHALL be a **separate Rust application** as defined in `editor-rust-application`, and
SHALL NOT be compiled into the engine runtime. It SHALL reach the engine only through the stable C
ABI and the live bridge protocol.

The world separation this capability defines remains, but its parts are owned by different processes:

| World | Owner |
|---|---|
| Interface | The editor process, in Rust; not an ECS world |
| Authoring | The runtime, hosting the edited project |
| Preview | The runtime, isolated per asset editor |
| Runtime | The runtime, instantiated from authoring data by play mode |

Authoring-only data SHALL NOT reach a runtime world. This SHALL be checkable, and it is now enforced
by two mechanisms rather than one: the compilation separation of authoring code, and the fact that
the editor is a different binary that cannot be linked into a game.

A shipping game SHALL contain no editor code — which follows from the editor being a separate
application rather than from a build flag.

#### Scenario: Export build excludes the editor
- **WHEN** a game is built for shipping
- **THEN** no editor code SHALL be linked, since the editor is a separate binary

#### Scenario: Two worlds
- **WHEN** the editor runs
- **THEN** the editor's interface state SHALL live in the editor process and the edited content in a
  runtime world, each with its own state and scheduling

#### Scenario: Previews do not touch the project
- **WHEN** an asset editor previews content
- **THEN** it SHALL use an isolated preview world in the runtime, and the edited project SHALL be
  unaffected

### Requirement: Plugin architecture
The editor SHALL be extensible through the **extension points** defined in `project-and-plugins`,
which also owns plugin identity, manifests, lifecycle, dependency resolution, the binary boundary,
and hot reload eligibility. This capability defines the editor-specific extension points and their
behaviour.

Editor extension points SHALL include: panels, view models, and commands; menus, toolbars, and
keyboard shortcuts; inspector property editors and component gizmos; importers and asset
post-processors; viewport tools receiving input; build steps and platform targets; document kinds and
asset editors; source control providers; search providers; settings pages; and editor event
subscriptions.

Extension points SHALL be expressed in **editor SDK abstractions**, not in the Rust interface
toolkit's types, so that a plugin survives a change of toolkit.

Editor extensions SHALL be authored in one of three forms, each with a defined boundary:

| Form | Boundary |
|---|---|
| Rust plugin distributed as source | Compiled with the editor; uses the editor SDK crates directly |
| Binary plugin | Crosses the engine's stable C ABI, as required by `project-and-plugins` |
| Swift or out-of-process plugin | Crosses the C ABI or the live bridge protocol |

**Rust's native binary interface SHALL NOT be used as a plugin contract.**

Plugins SHALL read and write edited content only through the transaction API, so their changes are
undoable and journalled like any other.

Editor extension interfaces SHALL be versioned independently of the engine's release version, so a
plugin targets an interface rather than a patch release.

#### Scenario: Swift editor plugin
- **WHEN** a project includes a Swift editor plugin
- **THEN** it SHALL be reachable across the C ABI or the live bridge, SHALL register its panels and
  tools through editor SDK abstractions, and SHALL reload on rebuild where it declares support for
  reload

#### Scenario: Plugin API version
- **WHEN** a plugin requires a newer editor extension interface than is present
- **THEN** it SHALL be reported as incompatible rather than loaded and failing at random

#### Scenario: Plugin edits go through transactions
- **WHEN** a plugin tool modifies the edited document
- **THEN** it SHALL do so through the transaction API, and no other write path SHALL exist
