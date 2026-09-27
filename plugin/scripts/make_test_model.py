#!/usr/bin/env python3
"""make_test_model.py - Generates a tiny ONNX model for testing the core
inference pipeline (plugin/src/core) without needing to download a real
Real-ESRGAN model.

The generated model is NOT a real super-resolution model. It simply
resizes its input by 4x using ONNX's Resize op (nearest-neighbor, so the
output is trivially predictable for correctness checks), matching the
[1,3,H,W] float32 NCHW in/out contract that OnnxUpscaler expects. This is
useful for:
  - Verifying the C++ tensor plumbing (NCHW packing/unpacking, tiling,
    alpha handling) is correct, independent of model quality.
  - CI/dev environments (like this one) without GPU or large downloads.

Usage:
    pip3 install onnx numpy
    python3 make_test_model.py [output_path]   # default: test_model_4x.onnx

Requires: onnx, numpy (pip install onnx numpy)
"""
from __future__ import annotations

import sys

import numpy as np
import onnx
from onnx import TensorProto, helper


def build_model(scale: int = 4) -> onnx.ModelProto:
    # Dynamic H/W via symbolic dims so the model works on any input size,
    # matching how OnnxUpscaler probes native scale with an 8x8 image and
    # then runs on arbitrary tile sizes.
    input_tensor = helper.make_tensor_value_info(
        "input", TensorProto.FLOAT, ["batch", 3, "height", "width"]
    )
    output_tensor = helper.make_tensor_value_info(
        "output", TensorProto.FLOAT, ["batch", 3, "out_height", "out_width"]
    )

    # Resize op with explicit scales input: [1, 1, scale, scale].
    scales_init = helper.make_tensor(
        name="scales",
        data_type=TensorProto.FLOAT,
        dims=[4],
        vals=np.array([1.0, 1.0, float(scale), float(scale)], dtype=np.float32),
    )
    # Resize (opset 11+) requires a `roi` input even when coordinate_transformation_mode
    # doesn't use it; pass an empty tensor.
    roi_init = helper.make_tensor(
        name="roi", data_type=TensorProto.FLOAT, dims=[0], vals=[]
    )

    resize_node = helper.make_node(
        "Resize",
        inputs=["input", "roi", "scales"],
        outputs=["output"],
        mode="linear",
        coordinate_transformation_mode="half_pixel",
        name="upsample_resize",
    )

    graph = helper.make_graph(
        nodes=[resize_node],
        name="TestUpscale4x",
        inputs=[input_tensor],
        outputs=[output_tensor],
        initializer=[scales_init, roi_init],
    )

    model = helper.make_model(
        graph,
        producer_name="make_test_model.py",
        opset_imports=[helper.make_opsetid("", 13)],
    )
    model.ir_version = 8
    onnx.checker.check_model(model)
    return model


def main(argv: list[str]) -> int:
    out_path = argv[1] if len(argv) > 1 else "test_model_4x.onnx"
    model = build_model(scale=4)
    onnx.save(model, out_path)
    print(f"Wrote test model to {out_path} ({model.ByteSize()} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
