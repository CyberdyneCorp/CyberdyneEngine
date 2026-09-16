# `content/beauty/` — the beauty shot's content

**The first content this repository ships.** M11.c tasks 6.1c and 6.2.

| | |
|---|---|
| `shot.cyshot` | the scene: the camera, the sun, the grade, and thirty-one placements |
| `materials/*.cymatcanvas` | what the editor's node-graph canvas produced — an interchange, not a format anything reads twice |
| `materials/*.cygraph` | the canonical authored graph, written by the ENGINE's `cy::graph::write_graph` |
| `meshes/*.cyprim` | six primitive sources: a plane, a cylinder, three boxes and a sphere |
| `textures/*.png` | nine 256x256 source images — [licence and provenance](textures/PROVENANCE.md) |

`just capture-beauty-shot` turns all of it into
[`docs/design/images/m11c-beauty-shot.png`](../../docs/design/beauty-shot.md), and regenerates the
`.cymatcanvas` and `.cygraph` files on the way: they are committed because the picture is a function
of them, and reproduced by the recipe because a committed file nothing regenerates is a file nobody
can check.

**What is NOT here**: no cooked asset. The BC7 and BC5 blocks the device samples, the generated
Slang, the compiled SPIR-V and the material bundle are all produced at capture time into the build
directory. A cooked artefact in a source tree is a cache with no invalidation.
