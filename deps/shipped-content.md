## Shipped content

**M11.c task 6.2.** `thirdparty-dependencies`' governance applies to content the project ships as
much as to code it links, and M11.c is the rung that first made the project ship any.

Everything under `content/` is **the project's own**, licensed as the rest of this repository is
([`LICENSE`](LICENSE)). Nothing in it is a third-party asset and nothing in it is derived from one,
which is why no entry appears above for any of it.

| what | where | provenance |
|---|---|---|
| Nine PNG textures | `content/beauty/textures/` | generated from seed `0x5EEDB107` by `tools/content/make_beauty_textures.py` — [the record](content/beauty/textures/PROVENANCE.md) |
| Three material graphs | `content/beauty/materials/` | authored on the editor's own node-graph canvas by `cy-author-material`; the `.cygraph` is written by the engine |
| Six primitive sources | `content/beauty/meshes/` | six-line `.cyprim` files; the geometry is `cy::import::build_primitive_mesh`'s |
| One scene | `content/beauty/shot.cyshot` | the camera, the sun, the grade and thirty-one placements |

**The rule this establishes, for whoever adds the next asset**: a file under `content/` carries a
`PROVENANCE.md` beside it naming its licence and either the generator and seed that produce it or the
source it came from and the terms it came under. A file that carries neither does not get committed.
