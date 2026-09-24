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

# material/ is M7 task 6.3: material cooking as a node in the derivation graph. The compiler itself
# is engine code in src/rendering/material/, because `material-compiler` requires it and because a
# runtime previews nodes and creates instances with no tool present; what is here is the cook and
# its front end. Like cook/, import/ and build/ it is a library plus a thin front end, and like them
# nothing links it into a runtime.
add_subdirectory(material)

# shaders/ is M11.c tasks 1.5-1.7: `shader-system`'s pipeline step 4, which until this rung was a
# sentence in a comment. It compiles the shader set for every target the build emits — SPIR-V, MSL
# and DXIL — and measures the one thing that makes a second target worth having: that the same graph
# compiled twice declares the same interface both times. It is built only where a Slang front end
# is, and its own CMakeLists.txt returns early otherwise.
add_subdirectory(shaders)

# quiet-host/ is M11.c's seventh close: the quiet-host premise as a library (`cy::host-load`, which
# samples/10-world links for `--quiet-host`) and as the wrapper `cy_quiet_host` that every ledger
# criterion running a timing-sensitive suite goes through. Its own CMakeLists.txt says why a
# premise checked from outside the process replaced the harness's allowances.
add_subdirectory(quiet-host)
