# src/ml/ — CyberML: tensors, model assets, sessions, the backend abstraction, the per-frame budget,
# and the determinism boundary. M8.c section 4.
#
# ================================================================================================
# WHAT IS HERE AND WHAT IS DELIBERATELY NOT
# ================================================================================================
#
# `ml-inference` draws the line in its own Purpose and this file keeps it: "The engine does not
# implement a neural runtime — operator coverage and per-device optimisation represent enormous
# investment with no differentiating benefit — and instead provides the asset model, the tensor and
# session API, the backend abstraction over ONNX Runtime, Core ML, DirectML and TensorRT, the
# scheduling and budget, and the determinism boundary."
#
# Everything in that list is here. Nothing else is: this module does not parse a model, does not
# implement an operator, and does not know what a graph is. `src/ml/src/onnxruntime_backend.cpp` is
# the one translation unit that names a runtime's types, and it does so inside
# `#if defined(CY_ML_ONNXRUNTIME)`.
#
# TWO TARGETS, AND THE SECOND ONE EXISTS BECAUSE OF A LAYER RULE.
#
#   cy::ml        LAYER servers (2). Depends on cy::core and cy::ecs and nothing above. That is what
#                 lets a session be created from a server, from a scene system, or from a cook.
#   cy::ml-cook   LAYER scene (4). The bridge from a `ModelAsset` to `cy::gameplay::ModelPinning`,
#                 which the cook-time firewall takes. It is a separate target because
#                 `<cy/gameplay/cook_firewall.h>` is at layer 4 and a layer-2 module may not name
#                 it — and it is a bridge rather than a gate because the gate belongs to
#                 src/gameplay/, for the reasons that header states at length.
#
# WHY cy::ml DEPENDS ON cy::ecs. `<cy/ecs/firewall.h>` is the determinism firewall's enforcement
# point, and `cy::ml::ResultScope` opens `WriteOrigin::Inference` or `WriteOrigin::PinnedInference`
# around code that consumes a result. A session is where "pinned" stops being a claim in an asset
# and becomes a runtime fact, so the session is what owns the origin. cy::ecs is layer 1, so this is
# a downward dependency like any other.
#
# WHAT src/ai/ DOES NOT DO. `ml-inference`: "`ai-system` SHALL NOT depend on `ml-inference`". There
# is no edge from cy::ai to cy::ml in this build and the link graph is what keeps it that way; the
# AI graph's inference node is a consumer relationship the cook resolves, not a link.
#
# GATED BY CY_ML, which is ON by default (cmake/features.cmake says why, and says what the option
# does and does not fetch: nothing).

cy_add_module(
    NAME cy_ml
    LAYER servers
    ALIAS cy::ml
    TYPE STATIC
    SOURCES
        src/backend.cpp
        src/model.cpp
        src/onnxruntime_backend.cpp
        src/schedule.cpp
        src/session.cpp
        src/tensor.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES
        cy::core-base
        cy::core-jobs
        cy::core-memory
        cy::core-values
        cy::ecs)

# The inference runtime, when the option is on. The wrapper cmake/dependencies.cmake publishes is
# `cy::dep::onnxruntime`; it is PRIVATE, so its include directories are not inherited and no header
# above src/onnxruntime_backend.cpp could resolve an onnxruntime include even if one were written.
if(CY_ML_ONNXRUNTIME AND TARGET cy::dep::onnxruntime)
    target_link_libraries(cy_ml PRIVATE cy::dep::onnxruntime)
    # No target_compile_definitions here on purpose. The option reaches the source through the
    # generated <cy_features.h>, which is the one place a CY_* option is spelled; defining it a
    # second time on the command line would be a macro redefinition and a second source of truth.
endif()

# The cook-time bridge. Layer 4 because `cy::gameplay` is.
cy_add_module(
    NAME cy_ml_cook
    LAYER scene
    ALIAS cy::ml-cook
    TYPE STATIC
    SOURCES cook/src/cook.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/cook/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-values cy::gameplay cy::ml)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
