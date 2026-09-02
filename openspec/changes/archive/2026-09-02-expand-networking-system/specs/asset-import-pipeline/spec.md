## ADDED Requirements

### Requirement: Cook profiles
Cooking SHALL be parameterised by a **cook profile** declaring what a build needs, so that content
selection is a declared policy rather than an accumulation of per-asset flags.

The engine SHALL provide at minimum:

| Profile | Retains | Excludes |
|---|---|---|
| `Client` | Everything a playable client needs | Editor-only data, server-only debug data |
| `DedicatedServer` | Collision, navigation, gameplay data, animation data gameplay depends on | Textures, shaders, high-resolution meshes, audio, VFX assets, UI assets |
| `Editor` | Everything, including source references and authoring metadata | Nothing |

Profiles SHALL be extensible per project, and SHALL compose with platform variants so a
`DedicatedServer` cook for Linux differs from a `Client` cook for Windows in both content and
format.

Where a server needs a **subset** of an otherwise client-only asset — collision geometry derived
from a render mesh — the profile SHALL retain that subset rather than either the whole asset or
nothing.

Each cook SHALL report what was excluded and the resulting size by category, so accidental
inclusions are visible.

#### Scenario: Server cook excludes rendering content
- **WHEN** a `DedicatedServer` cook runs
- **THEN** textures, shaders, and VFX assets SHALL be excluded, and the report SHALL show what was
  removed and the size saved

#### Scenario: Collision survives mesh exclusion
- **WHEN** a mesh contributes collision geometry and is excluded from a server cook
- **THEN** its collision representation SHALL be retained

#### Scenario: Accidental inclusion is visible
- **WHEN** a server cook unexpectedly includes a large texture
- **THEN** the size report SHALL surface it rather than it passing unnoticed
