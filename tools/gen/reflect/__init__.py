"""The reflection generator — task 1.1.2, governed by `core-type-system`.

Split into one module per concern so that no single function has to hold the whole pipeline:

    annotations.py  the annotation argument grammar
    attrspec.py     the attribute table, the schemas a module declares, and their validation
    model.py        what a parse produced, independent of libclang
    parse.py        libclang: the frontend, the pruned descent, and the include digests
    manifest.py     the committed identity manifest: assignment, tombstones, and the gate
    emit.py         the generated C++
    cache.py        the content-keyed incremental cache
    cli.py          the command line

Run it through `tools/gen/reflect_gen.py`, or through `just generate-headers`.
"""
