# tools/ — layer 7: code generators and command-line utilities.
#
# Built when CY_BUILD_TOOLS is on, and never linked into a shipped runtime. Several tools are
# scripts rather than compiled targets — tools/layercheck/, tools/deps/, tools/gen/ — and are invoked
# by recipes directly; only the ones that are C++ targets are added here.
#
# cook/ is the first of those. It arrives at M2 task 3.2.13 with `core-assets-and-io`'s "the runtime
# SHALL load only cooked assets": the package format's write path was M1's and the thing that
# produces cooked bytes is M2's, and this is what joins them.
add_subdirectory(cook)

# import/ is M5 task 5.1: the importer framework, the cook cache's client, glTF and texture import,
# and the mesh processing beneath them. Like cook/ it is a library plus a thin front end, and like
# cook/ nothing links it into a runtime — a source-format parser is tool-time only, which is what
# layer 7 means here.
add_subdirectory(import)

# build/ is M6 section 7: `build-and-packaging` to Working. The derivation graph, derivation keys,
# explicit inputs, immutable artefacts, the derived data cache, the build service, precise
# invalidation, packages and chunk-level patching. Like cook/ and import/ it is a library plus a thin
# front end, and like them nothing links it into a runtime.
add_subdirectory(build)
