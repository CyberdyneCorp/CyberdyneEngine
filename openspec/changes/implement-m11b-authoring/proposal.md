# M11.b — Authoring: the editor finished, and a game made with it

## Why

**`editor-architecture` and `live-editing` have been recorded at Seed since M5, and five milestones
have built features on top of them.** M6 streamed a world, M7 put the engine's own rendered frame in
the viewport, M8.a made a scene a person builds by hand, M8.b and M8.c made it play and look
finished, M9 made it defensible and M10 made it large — all of it above two rows the record has never
moved off the tier they were seeded at.

**That is a thin foundation and not a mis-record, and the record already establishes which.** M10's
audit read all nineteen contested cells against the tree M10 closes on rather than against the gate
that parked them, and it kept both rows at Seed with a grep as the evidence for each:

- **The three play modes are not exposed.** `grep -rniI 'SeparateProcess\|RemoteDevice\|InEditor'
  src/ editor/ tools/` returns nothing.
- **No specialised editor exists.** The three panel kinds the interface defines are `hierarchy`,
  `viewport` and `content-browser`, and `chrome.rs` reserves a `CentreLower` region for *"the active
  specialised editor: script graph, animation, materials, sequencing"* that nothing fills.
- **Project creation from templates, project settings, the build-and-deployment client, the debugger
  and the frame profiler have no implementation.** `profiler` exists as a docking-layout panel
  identifier and nothing draws one.
- **There is no live edit policy.** `grep -rniI
  'LiveEditPolicy\|ReinitializeComponent\|RecreateEntity\|RestartWorld' src/ editor/ tools/` returns
  nothing, against a requirement for per-field policy.

**The rest of the authoring surface is the same shape — working mechanisms with the authoring half
missing**, each verified rather than assumed:

- **`editor-documents-and-transactions`: source control integration is unstarted, not partial.**
  `grep -rniE 'source.control' editor/crates/*/src/` returns seven hits and every one is a comment or
  a remedy string; the requirement asks for a provider interface with Git, Perforce and a null
  provider behind it.
- **`editor-viewport-and-gizmos`: the editor's `ViewMode` enum carries nineteen entries and the
  requirement names eight more** — virtual-geometry clusters, lightmap and GI probes, virtual-texture
  feedback and residency, virtual-shadow pages, physics colliders, navigation data, audio emitters,
  streaming region state — and `viewmode.rs` lists them in its own header as absent.
- **`project-and-plugins`: four of eleven requirements have no implementation.** `find src tools
  -iname '*plugin*'` returns one layercheck fixture, and that sentence has been true since M5.
- **`asset-import-pipeline`: glTF refuses skins and animations by name** (`gltf.cpp:940`),
  `MeshData` carries no joint or weight array, `decode_image` reads Targa and names the decoders PNG
  and JPEG would need (`texture.cpp:438`), and there is **no BC7 or ASTC encoder, no USD and no
  virtual-geometry cooking**.
- **`input-and-actions`: nothing authors or cooks an input asset.** There is no input asset kind, no
  importer and no cook step; the module's README says the tables are built in code.
- **`swift-scripting` has no shipping configuration.** The static, whole-module half of the two the
  specification requires is untried, the Swift toolchain version is pinned nowhere though the
  requirement asks for a pin *"per engine release and verified in CI"*, `@Node(path)` resolves to
  `nil`, the tree callbacks are declared and not driven, and there is no chunk source.
- **A node has no name.** `cy_editor_documents` carries none — the third field of a `.cyworld`'s
  `node` line is its layer — so M8.a's own screenshot shows two authored objects as two outliner rows
  **both reading `Transform`**. No criterion can check it, because naming a node is a concept the
  document model does not have.

**And there is no game.** `samples/` holds fifteen entries and every one of them proves one slice:
`03-first-light` a frame, `04-character` a character, `05-editor-session` and `05b-editor-window` the
editor, `06-open-world` streaming and saving, `07-fidelity` the fidelity features, `08a-authoring`
create-drop-play-undo, `08-vertical-slice` a playable slice, `09-multiplayer` four peers,
`09b-animated-character` animation, `10-world` an environment. **`samples/11-ship` is a packaging
proof and is not a game either** — it is one project built, cooked, packaged and launched, and it
would prove a recipe over a directory with nothing in it just as well.

## What Changes

- **`editor-architecture` and `live-editing` off Seed and to Complete** — the three play modes, the
  specialised editors the `CentreLower` region was reserved for, project creation and settings, the
  build-and-deployment client, the debugger and the frame profiler; and on the live side, a per-field
  edit policy with the reinitialise, recreate and restart outcomes the requirement names.
- **The rest of the editor rows to Complete** — source control behind a provider interface with a
  null provider, the eight missing view modes, plugins and their lifecycle, resolution, lockfile and
  trust tiers, and a document model in which a node has a name.
- **The authoring formats** — skins and animations through glTF, PNG and JPEG decoding, **BC7 and
  ASTC encoding**, input assets authored and cooked, and virtual-geometry cooking reachable from
  inside the editor rather than from a command line.
- **The gameplay rows a game exercises, to Complete** — abilities and effects, AI, animation and
  skinning, camera, navigation, physics, the UI, text, 2D, visual scripting and sequences.
- **A real sample game, named as a deliverable and built through the editor.** Not a slice and not a
  packaging proof: a small complete game — a start, a loop, a way to lose or win, and content
  authored in the editor rather than assembled in C++ — which is the only artefact that can judge
  twenty-four authoring and gameplay rows at once.

## Capabilities

**Twenty-four rows to Complete — three from Seed and twenty-one from Working** — carrying **420
requirements**, the largest requirement load of the five rungs and 39% of the 1,069 requirements M11
owes in total.

From **Seed**: `editor-architecture`, `live-editing` (M5) and `ml-inference` (M8.c).
From **Working**: `editor-rust-application`, `editor-documents-and-transactions`,
`editor-viewport-and-gizmos`, `asset-import-pipeline` (M5), `editor-ui-ux`, `editor-visual-language`,
`editor-agent-interface` (M5.5), `project-and-plugins` (M1), `input-and-actions`, `swift-scripting`
(M4), `physics` (M8.a), `visual-scripting`, `ui-system`, `text-and-fonts`, `rendering-2d`,
`gameplay-abilities-and-effects`, `ai-system`, `animation-and-skinning`, `camera-system`,
`navigation` (M8.b) and `sequencing-and-cinematics` (M8.c).

## What is contingent, and what this rung predicts about itself

- **This rung is deliberately placed before M11.c, and the ordering is the argument.** An
  art-directed shot assembled by hand in C++ proves the renderer and nothing else; one authored
  *through* the editor this rung finishes proves both. The dependency is concrete rather than
  rhetorical: **M11.c needs textured materials and there is no BC7 or ASTC encoder in the tree**, so
  the texture path is this rung's work before it is that rung's subject.
- **`ml-inference` is the row this rung predicts it will not complete.** It is at Seed from M8.c with
  nine requirements, and the only thing that would exercise it is a game whose AI wants inference. If
  the game does not want it, the honest outcome is **an explicitly recorded deferral with its
  re-entry point at M11.e**, not a Complete cell over an unexercised runtime.
- **`swift-scripting` is the row most likely to be demoted into M11.d.** Three of its four open items
  are scripting; the fourth — a shipping configuration and a toolchain pin *verified in CI* — is a
  build-system requirement wearing a scripting row's name, and it cannot be judged before M11.d pins
  the matrix. **If that is how it falls out, the row moves with its reason recorded**, which is what
  M10's gate did with the cells it refused.
- **`sequencing-and-cinematics` (31 requirements), `animation-and-skinning` (30) and `camera-system`
  (28) carry no named blocker**, which is its own risk: nothing has refused them because nothing has
  read them end to end at Complete grade. Three rows at that size with no first-hand audit is the
  shape M10's gate found behind twelve parked Complete cells.
- **Twenty-four rows is more than any closed milestone on this ladder advanced**, and this rung says
  so at proposal time rather than at its gate. If the load does not fit, the outcome that keeps the
  record honest is a recorded demotion of named rows to M11.e with re-entry points — not a gate that
  reads a specification again and feels better about it.

## Impact

- **New code**: the play modes and the live edit policy; the specialised editor panels behind
  `CentreLower`; project templates, settings, the build client, the debugger and the frame profiler;
  a source-control provider interface and a null provider; eight view modes; the plugin surface; a
  texture encoder and the glTF skin and animation path; input assets and their cook step; and the
  game.
- **Existing code**: `cy_editor_documents` gains node naming, which is a document-model change rather
  than a field; `tools/import` becomes reachable from the editor for every format it supports.
- **Closing artefact**: **a real sample game**, authored through the editor, playable from a start
  state to an end state, with its content in the project rather than in C++. The artefact must be
  honest about which parts a person authored and which parts the sample's code assembles.
- **Risk**, and the rung's named spike: **the play-mode seam.** `InEditor`, `SeparateProcess` and
  `RemoteDevice` are one requirement in two specifications, and whether the hosted runtime can carry
  all three without a second world model is the question M5 seeded and nobody has asked since. Spike
  it at the head of the rung, because discovering it needs a second world model after the editor rows
  are built on the first one is a migration rather than an edit.
