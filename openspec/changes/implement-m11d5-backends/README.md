# implement-m11d5-backends

M11.d.5 of the roadmap, **inserted between M11.d and M11.e by M11.d's own spike**: Metal native
rather than a translation layer, D3D12 from nothing, and the one claim no single leg of the matrix
can make — the M3 golden images matching across Vulkan, Metal and D3D12 within tolerance, with the
device that answered each named in the result.

It exists because the machine does not match the plan: this project works on a Linux host with one
GPU vendor and no Apple toolchain, so neither backend can be compiled where M11.d is worked, let
alone judged. The sections moved rather than being deleted or descoped, and
`golden-images-across-three-backends` moved with them so this rung cannot close on "it compiles
somewhere".
