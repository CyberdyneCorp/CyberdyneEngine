# `fixtures/reload/` — two generations of one Swift game module

The other side of `bindings/swift/tests/test_swift_reload.cpp`. One behaviour, `SwiftCounter`,
written twice:

| | `v1/` | `v2/` |
|---|---|---|
| schema | 1 | 2 |
| exported | `health`, `ammo`, `label` | `health`, `mana`, `shield`, `label` |
| migration | — | `ammo` becomes `mana`, halved, in `onMigrate` |

**The layout differs between them on purpose.** That is the case in-place preservation gets wrong
silently: the hot-reload spike ran v2 code over a v1 object and read `health = 17` (v1's `ammo`),
`shield = 1` (an entity id) and `mana = 3.5e18` (the raw bits of a `String`), with no trap and no
diagnostic. A fixture whose two versions had the same layout would pass against a loader that
preserved instances in place, which is exactly the loader this milestone exists to not have.

`bindings/swift/tools/cy_swift_module.py` builds **three** libraries from these two directories —
`libCyGame_g0.so` (v1), `libCyGame_g1.so` (v2) and `libCyGame_g2.so` (v1 again) — because reloading
1 → 2 is then a **downgrade**: a schema-1 module reading a schema-2 blob. That is the Swift side of
`native-abi`'s "Incompatible reload", and it needs a third generation rather than a third source.

**A different file and a different Swift `-module-name` per generation**, which the spike measured
to be necessary and which the loader cannot enforce for itself.

Both use `@Behaviour` and `@GameModule`, so the macro plugin is on the path the reload test
exercises rather than only on the path the package's own tests do.
