#!/usr/bin/env python3
"""Generate src/ml/assets/threat_classifier.onnx — the committed model M8.c task 4.1d runs.

WHY THE MODEL IS COMMITTED AND THIS SCRIPT IS NOT RUN BY THE BUILD
------------------------------------------------------------------
`ml-inference` at Seed means the backend RUNS: "A committed model asset small enough to live in the
repository, loaded and run with a checked result." A model produced at build time by a script that
needs the `onnx` PyPI package would make the suite depend on a Python package the manifest does not
carry, on a machine that may be offline, and — worse — would make the checked result depend on
whatever weights that run happened to produce. So the model is a committed artefact of about six
hundred bytes, and this script is its provenance: run it to regenerate, diff the result to verify.

    python3 -m venv .venv && .venv/bin/pip install onnx
    .venv/bin/python src/ml/tools/make_threat_classifier.py

WHAT THE MODEL IS
-----------------
A two-layer perceptron over four normalised perception features, producing three class scores — the
smallest thing that is honestly shaped like the classifier `ml-inference`'s own scenario describes
("WHEN a perception classifier node produces a target classification with a confidence"):

    input   features  float32[batch, 4]   proximity, exposure, threat_history, ammunition
    Gemm -> Relu ->   float32[batch, 8]
    Gemm -> Softmax   float32[batch, 3]   ignore, engage, retreat
    output  scores    float32[batch, 3]

The batch dimension is DYNAMIC, which is what makes the batching requirement testable at all:
"WHEN many agents evaluate the same inference node in one tick THEN their inputs SHALL be batched
into one session call."

THE WEIGHTS ARE CHOSEN, NOT TRAINED, AND THEY ARE INTERPRETABLE ON PURPOSE
--------------------------------------------------------------------------
A pseudo-random weight matrix would produce a model whose only checkable property is that it
reproduces a number somebody read off a run — a test that cannot distinguish a working backend from
a backend that returns the same wrong answer twice. These weights are written by hand so that the
model has behaviour a reader can predict from the file:

    hidden 0..3   Relu(f)      the four features, which are in [0, 1], so Relu is the identity
    hidden 4..7   Relu(-f)     zero for a valid input; present so the layer is a real 4x8 Gemm

    ignore   =  1.0 - proximity - exposure - threat - 0.5 * ammunition
    engage   = -1.0 + 2.0 * proximity - exposure + threat + 2.0 * ammunition
    retreat  = -1.0 + 2.0 * exposure + 1.5 * threat - 2.0 * ammunition

so each of the three classes wins for an input a person can describe, and the test asserts the
ARGMAX of three of them rather than only the arithmetic of one. The all-zero input is the fourth
case and it is the one that can be computed on paper: the hidden layer is zero, the logits are the
output bias [1, -1, -1], and the scores are softmax of that.
"""

from __future__ import annotations

from pathlib import Path

import numpy as np
import onnx
from onnx import TensorProto, helper, numpy_helper

OUTPUT = Path(__file__).resolve().parents[1] / "assets" / "threat_classifier.onnx"

FEATURES = 4
HIDDEN = 8
CLASSES = 3

# hidden = Relu([f, -f]).
HIDDEN_WEIGHT = np.concatenate(
    (np.eye(FEATURES, dtype=np.float32), -np.eye(FEATURES, dtype=np.float32)), axis=1
)
HIDDEN_BIAS = np.zeros(HIDDEN, dtype=np.float32)

#                     ignore  engage  retreat
OUTPUT_WEIGHT = np.array(
    [
        [-1.0, 2.0, 0.0],  # proximity
        [-1.0, -1.0, 2.0],  # exposure
        [-1.0, 1.0, 1.5],  # threat history
        [-0.5, 2.0, -2.0],  # ammunition
        [0.0, 0.0, 0.0],  # the negated half, which a valid input never activates
        [0.0, 0.0, 0.0],
        [0.0, 0.0, 0.0],
        [0.0, 0.0, 0.0],
    ],
    dtype=np.float32,
)
OUTPUT_BIAS = np.array([1.0, -1.0, -1.0], dtype=np.float32)

SAMPLES = (
    ("all zero", [0.0, 0.0, 0.0, 0.0]),
    ("close, hidden, hostile, armed", [0.9, 0.2, 0.9, 0.9]),
    ("close, exposed, hostile, dry", [0.9, 0.9, 0.9, 0.05]),
    ("far, calm, unknown, dry", [0.05, 0.05, 0.05, 0.05]),
)


def evaluate(sample: np.ndarray) -> np.ndarray:
    hidden = np.maximum(sample @ HIDDEN_WEIGHT + HIDDEN_BIAS, 0.0)
    logits = hidden @ OUTPUT_WEIGHT + OUTPUT_BIAS
    exponentials = np.exp(logits - logits.max(axis=1, keepdims=True))
    return exponentials / exponentials.sum(axis=1, keepdims=True)


def main() -> None:
    nodes = [
        helper.make_node("Gemm", ["features", "w0", "b0"], ["hidden"], name="hidden_gemm"),
        helper.make_node("Relu", ["hidden"], ["activated"], name="hidden_relu"),
        helper.make_node("Gemm", ["activated", "w1", "b1"], ["logits"], name="output_gemm"),
        helper.make_node("Softmax", ["logits"], ["scores"], axis=1, name="output_softmax"),
    ]

    graph = helper.make_graph(
        nodes,
        "cyberdyne_threat_classifier",
        inputs=[helper.make_tensor_value_info("features", TensorProto.FLOAT, ["batch", FEATURES])],
        outputs=[helper.make_tensor_value_info("scores", TensorProto.FLOAT, ["batch", CLASSES])],
        initializer=[
            numpy_helper.from_array(HIDDEN_WEIGHT, "w0"),
            numpy_helper.from_array(HIDDEN_BIAS, "b0"),
            numpy_helper.from_array(OUTPUT_WEIGHT, "w1"),
            numpy_helper.from_array(OUTPUT_BIAS, "b1"),
        ],
    )

    model = helper.make_model(
        graph,
        producer_name="cyberdyne-engine",
        # Opset 13 rather than the newest: Softmax changed its axis semantics at 13, every backend
        # `ml-inference` names supports it, and "portable" has to mean something for an asset that
        # is committed once and read by whatever runtime a target platform has.
        opset_imports=[helper.make_opsetid("", 13)],
        ir_version=8,
    )
    model.producer_version = ""
    onnx.checker.check_model(model)

    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_bytes(model.SerializeToString())
    print(f"wrote {OUTPUT} ({OUTPUT.stat().st_size} bytes)")

    # The provenance of every constant in src/ml/tests/test_onnxruntime.cpp.
    for label, values in SAMPLES:
        scores = evaluate(np.array([values], dtype=np.float32))
        rendered = ", ".join(f"{value:.6f}" for value in scores[0])
        print(f"  {label:32s} argmax={int(scores.argmax())}  scores=[{rendered}]")


if __name__ == "__main__":
    main()
