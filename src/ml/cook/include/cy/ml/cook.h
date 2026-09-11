#pragma once
// The cook-time half of the determinism boundary, from CyberML's side. M8.c tasks 4.2 and 1.3.
//
// `ml-inference`: "Where a model is used inside an AI graph, the graph node SHALL declare whether
// its output is authoritative. A non-pinned model feeding an authoritative node SHALL be **rejected
// at cook time**."
//
// ================================================================================================
// WHAT THIS FILE IS, AND WHAT IT DELIBERATELY IS NOT
// ================================================================================================
//
// The GATE is `cy::gameplay::check_inference_bindings` in `<cy/gameplay/cook_firewall.h>`, and it
// lives there rather than here for the reason that header states at length: "authoritative" is a
// gameplay word, a gate inside the module it gates would be a producer checking itself, and the
// gate must be callable from a cook built with `CY_ML` **off**.
//
// This file is the BRIDGE and nothing else: it turns what a `ModelAsset` declares into the plain
// `ModelPinning` value the gate takes. Three field copies. It exists so that the copy is written
// once, in the module that owns the asset, rather than re-derived at every cook driver — because
// re-deriving it is how "pinned" acquires a second definition that disagrees with the first.
//
// A COOK DRIVER STILL HAS TO CALL THE GATE, and on the tree this was written against none does.
// `tools/cook/`'s `run()` is the call site, it already fails a run on a returned error, and wiring
// it is two lines: collect a `ModelPinning` per model asset with `pinning_of` below, collect an
// `InferenceBinding` per AI graph node that binds one from `src/ai/`, and call
// `check_inference_bindings`. Neither `tools/cook/` nor `src/ai/` was this agent's to edit; what is
// here is the half that is, and `unit.ml_cook` drives the gate end to end over real assets so that
// the wiring is the only thing missing rather than the checking.

#include <cy/core/base/types.h>
#include <cy/gameplay/cook_firewall.h>
#include <cy/ml/model.h>

namespace cy::ml {

/// What `asset` declares about its own reproducibility, in the form the cook-time firewall takes.
///
/// The names are the asset's own enumerator spellings — "onnxruntime", "f32" — so that a refusal
/// names the configuration a reader can look up in the asset rather than an integer.
[[nodiscard]] gameplay::ModelPinning pinning_of(const ModelAsset& asset) noexcept;

/// The digest a cook targeting this configuration produces, for
/// `InferenceBinding::cook_configuration`.
///
/// The same function `ModelDeterminism::verified_configuration` was filled from, which is the
/// point: the gate compares two numbers, and two numbers computed by different functions is the
/// defect the gate exists to catch wearing the gate's own clothes.
[[nodiscard]] u64 cook_configuration(BackendKind backend, Precision precision,
                                     std::string_view device) noexcept;

}  // namespace cy::ml
