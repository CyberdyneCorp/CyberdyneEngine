# `identity/` — the identity manifest

`manifest.toml` is the source of truth for every persistent type and field identifier the engine
issues. It is committed, it is reviewed like code, and the generator only ever appends to it.

## Why it is here and not under `src/core/reflect/`

The concept belongs to `core-type-system`, whose module is `src/core/reflect/`, and putting the file
there was the obvious first choice. It is the wrong one for one reason: **the manifest is
engine-wide**. Every module that annotates a type adds entries to it, so a rename in
`src/core/memory/` would produce a diff inside another module's directory, which reads as though
that module changed.

At the repository root it sits beside `deps/manifest.toml`, the other committed, engine-wide,
single-source-of-truth manifest, and the symmetry is the point: both are files a reviewer should
notice in a diff, and neither belongs to the module that happens to read it.

## What an identifier is

An **opaque number assigned on first sight**. Not a hash of the name, not a hash of the type, not an
index, and not a content digest — `design.md` §1 and the header comment of the manifest itself give
the reasoning. There is no function anywhere in the engine from a name to an identifier, and that
absence is deliberate: the invariant is easier to keep when the shortcut does not compile.

Renaming a type or a field, moving it into a namespace, or relocating it between modules changes its
`name` here and **nothing else**. Names are metadata; identifiers are the contract.

## Tombstones

`status = "removed"` retires a number permanently. It is never issued again, because a recycled
identifier produces data that loads successfully and is wrong — the one failure mode with no
diagnostic. `next_type_id`, and each type's `next_field_id`, stay above every number ever issued,
tombstones included.

## The workflow

```
just generate-headers      assign identifiers to new declarations and regenerate the metadata
just generate-check        fail if the generated metadata is stale or not reproducible
just quality-identity      the gate: nothing unassigned, nothing removed silently, nothing moved
```

When a declaration leaves the tree, generation stops and names it, its identifier, and the two ways
forward — because the generator cannot tell a rename from a removal and must not guess:

```
just generate-headers --rename 'cy::demo::Health::maximum=maximum_health'   # keeps the identifier
just generate-headers --tombstone 'cy::demo::Health::maximum'               # retires it
```

Both are a reviewed diff of this file, which is what `core-type-system` means by "deliberate
identity changes SHALL be possible through an explicit, reviewed manifest edit".

**Governed by**: `core-type-system` — "Identity manifest and tombstones".
