# src/rendering/graph/ — layer 4: the render graph. Section 2.2 of the M3 tasks.
#
# THIS IS THE MILESTONE'S POINT. A pass declares what it reads and what it writes and has no API to
# emit a barrier; the graph derives barriers, image layout transitions, queue-family ownership
# transfers, cross-queue semaphores, transient memory aliasing and the submit boundaries from those
# declarations alone. `rhi-and-render-graph` requires it; design.md §2 explains why establishing it
# with the first pass rather than the thirtieth is the whole reason M3 exists.
#
# WHAT BELONGS HERE. The declaration vocabulary (resources, passes, uses), the derivation, the
# executor that records a compiled plan into RHI command buffers, and the dump that makes a plan
# readable as text or Graphviz.
#
# WHAT DOES NOT BELONG HERE. A pass. The graph knows nothing about depth prepasses, clustered
# lighting or shadow atlases — those are src/rendering/'s other subdirectories and they reach this
# module the same way every other producer will: by declaring uses.
#
# THE DERIVATION MAKES NO DEVICE CALLS. compile.cpp asks the device exactly one question, through a
# callback, and it is "how much memory does this transient need". Everything else is arithmetic over
# the declarations, which is what lets the null backend run the identical code path and what makes
# two independent processes produce byte-identical plans.
cy_add_module(
    NAME cy_rendering_graph
    LAYER rendering
    ALIAS cy::rendering-graph
    TYPE STATIC
    SOURCES
        src/compile.cpp
        src/executor.cpp
        src/graph.cpp
        src/visualise.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    # cy::core-jobs is what parallel command recording (task 2.2.5) runs on. It is a PUBLIC
    # dependency because ExecuteOptions names a JobSystem; the graph never requires one to be
    # running, and sequential recording is the reference the parallel path is compared against.
    PUBLIC_DEPENDENCIES cy::core-base cy::core-memory cy::core-jobs cy::rhi
    PRIVATE_DEPENDENCIES cy::core-diagnostics)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()

# --- The structural gate (task 2.2.4) ---------------------------------------------------------------
#
# design.md §2: "a grep-level gate fails the build if a barrier call appears outside it". This is
# that gate, declared beside the module that owns the only legitimate barrier call in the engine and
# built with it, so it runs on every build rather than only when somebody remembers to run
# `just quality-layers`.
#
# Two checks, both cheap (the whole tree in about a tenth of a second):
#
#   barriers  no barrier-emitting symbol outside src/backends/rhi/ and src/rendering/graph/
#   gpuapi    no Vulkan, Slang or SPIR-V header above src/backends/
#
# The same script CI runs, so there is no second copy of the rule to keep in step, and
# tools/layercheck/fixtures/barrier-outside-graph/ is the deliberate violation that proves it still
# fires — which is what task 2.2.4's "prove it by introducing one" asks for, repeated on every pull
# request rather than performed once by hand.
# IT IS A TARGET OF ITS OWN AND NOT A POST_BUILD STEP, and the difference is the whole gate.
# Attached as `add_custom_command(TARGET cy_rendering_graph POST_BUILD ...)` it ran only when the
# graph itself was relinked — so a barrier symbol introduced in src/rendering/forward/, which does
# not relink the graph, BUILT CLEAN. That was measured, not supposed: a `using ProbeRecorder =
# rhi::BarrierRecorder;` in src/rendering/forward/src/frame.cpp compiled and linked with exit 0
# while `just quality-layers` rejected the same tree. `ALL` on a target with no output makes the
# check run on every build of the default target, which is what design.md §2 claims it does.
#
# The passkey (rhi::GraphBarrierKey's private constructor) is the half that cannot be avoided at
# all: a barrier CALL does not compile outside the graph whatever this gate does. This one covers
# the rest of the surface — the symbol named, the Vulkan header included — and costs about 0.15 s.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_custom_target(cy_rendering_graph_barrier_gate ALL
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/layercheck/layercheck.py"
                --root "${CMAKE_SOURCE_DIR}" --check barriers --check gpuapi --quiet
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "render graph: no barrier call outside the graph, no graphics API above the backends"
        VERBATIM)
    # Ordering only: the gate reads sources rather than build output, but running it after the
    # module it guards keeps its message next to that module's own diagnostics in the log.
    add_dependencies(cy_rendering_graph_barrier_gate cy_rendering_graph)
else()
    # Reported rather than silently skipped: a gate that is absent on the machine that needed it is
    # worse than one that says so. `just quality-layers` still covers it in CI.
    message(WARNING
        "cy_rendering_graph: no Python 3 interpreter, so the barrier and graphics-API gates "
        "(M3 task 2.2.4) will not run at build time. `just quality-layers` still runs them.")
endif()
