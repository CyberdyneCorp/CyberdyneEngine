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
