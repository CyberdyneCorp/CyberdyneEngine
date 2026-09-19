# implement-m11d-desktop

M11.d of the roadmap: the RHI **interface** settled on Vulkan and the null backend before a second
graphics API exists, a native `Platform` and `DisplayServer` replacing SDL3 on one desktop with no
change in `src/core/`, `src/ecs/`, `src/servers/` or `src/scene/`, the desktop half of
`rendering-forward-clustered`, the core rows the port audits, the build and quality gates, and
`samples/11-ship` on desktop.

**Metal and D3D12 are no longer here.** This rung's spike established that neither compiles on the
Linux host this project works on, so sections 2 and 3 moved — with `rhi-and-render-graph`'s Complete
cell and the three-backend golden-image comparison — into
[`implement-m11d5-backends`](../implement-m11d5-backends/README.md), a rung inserted between M11.d
and M11.e. `design.md` §1.4 is the measurement that argued for it.
