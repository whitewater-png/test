#!/usr/bin/env python3
"""export_anime_onnx.py - Exports the Real-ESRGAN anime 6-block PyTorch
weights (RealESRGAN_x4plus_anime_6B.pth) to ONNX, without depending on the
`basicsr` package.

Why not just `pip install basicsr`? At the time this script was written,
basicsr's `degradations.py` does `from torchvision.transforms.functional_tensor
import rgb_to_grayscale`, a private torchvision module that was removed in
torchvision >= 0.17. On any reasonably current PyTorch/torchvision install,
`import basicsr` (transitively pulled in by importing
`basicsr.archs.rrdbnet_arch`) raises ModuleNotFoundError. Rather than pin an
old torchvision (which drags in an old, potentially unpatched torch), this
script defines the RRDBNet architecture itself, using nothing but `torch`.

Architecture note: RRDBNet / ResidualDenseBlock / RRDB below are a minimal
re-implementation of the network architecture described in the Real-ESRGAN
paper and reference implementation:
  https://github.com/xinntao/Real-ESRGAN (BSD-3-Clause license)
  https://github.com/xinntao/BasicSR (also BSD-3-Clause; basicsr's
  rrdbnet_arch.py is the direct source of this architecture)
No basicsr/xinntao source code is imported or copied verbatim; the module
structure (conv layer shapes, growth-channel wiring, residual scaling) is
reproduced from the published architecture description because the exact
layer names and shapes must match the state_dict keys inside the .pth
checkpoint for load_state_dict() to succeed.

This script targets the "anime 6B" variant specifically:
  num_in_ch=3, num_out_ch=3, num_feat=64, num_block=6, num_grow_ch=32, scale=4
(6 RRDB blocks, vs. 23 for the general-purpose "photo" x4plus model -- the
photo model ships as a pre-exported ONNX file, see download_models.py, so
this script does not need a num_block=23 mode.)

Security note on loading the checkpoint:
  .pth files are pickle archives. Unpickling a *stock* torch.load() call
  executes arbitrary Python objects embedded in the pickle stream by
  design -- a torch.save() checkpoint from an untrusted source file is
  therefore, in the general case, a remote-code-execution vector, not a
  reasonable one to defend against with a mtime/free-scan of "does it look
  weird" or a mere source-trust argument. This script always loads with
  torch.load(path, map_location="cpu", weights_only=True). weights_only=True
  restricts unpickling to a safe allow-list of tensor/container types (see
  https://pytorch.org/docs/stable/notes/serialization.html#torch-load-with-
  weights-only) and refuses to unpickle arbitrary callables/objects, so a
  malicious .pth cannot smuggle code execution through this loader. If the
  checkpoint genuinely contains something outside that allow-list, loading
  will raise rather than silently falling back to unsafe unpickling -- this
  script does not catch that error and retry with weights_only=False.

Usage:
    python3 export_anime_onnx.py <input.pth> <output.onnx>
"""
from __future__ import annotations

import sys
from pathlib import Path

import torch
import torch.nn as nn
import torch.nn.functional as F


class ResidualDenseBlock(nn.Module):
    """5-conv residual dense block, as used inside each RRDB block."""

    def __init__(self, num_feat: int = 64, num_grow_ch: int = 32) -> None:
        super().__init__()
        self.conv1 = nn.Conv2d(num_feat, num_grow_ch, 3, 1, 1)
        self.conv2 = nn.Conv2d(num_feat + num_grow_ch, num_grow_ch, 3, 1, 1)
        self.conv3 = nn.Conv2d(num_feat + 2 * num_grow_ch, num_grow_ch, 3, 1, 1)
        self.conv4 = nn.Conv2d(num_feat + 3 * num_grow_ch, num_grow_ch, 3, 1, 1)
        self.conv5 = nn.Conv2d(num_feat + 4 * num_grow_ch, num_feat, 3, 1, 1)
        self.lrelu = nn.LeakyReLU(negative_slope=0.2, inplace=True)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        x1 = self.lrelu(self.conv1(x))
        x2 = self.lrelu(self.conv2(torch.cat((x, x1), 1)))
        x3 = self.lrelu(self.conv3(torch.cat((x, x1, x2), 1)))
        x4 = self.lrelu(self.conv4(torch.cat((x, x1, x2, x3), 1)))
        x5 = self.conv5(torch.cat((x, x1, x2, x3, x4), 1))
        # Empirical residual scaling factor used by the reference
        # implementation to keep the network stable at initialization.
        return x5 * 0.2 + x


class RRDB(nn.Module):
    """Residual in Residual Dense Block: three ResidualDenseBlocks chained
    with an outer residual connection (also scaled by 0.2)."""

    def __init__(self, num_feat: int, num_grow_ch: int = 32) -> None:
        super().__init__()
        self.rdb1 = ResidualDenseBlock(num_feat, num_grow_ch)
        self.rdb2 = ResidualDenseBlock(num_feat, num_grow_ch)
        self.rdb3 = ResidualDenseBlock(num_feat, num_grow_ch)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        out = self.rdb1(x)
        out = self.rdb2(out)
        out = self.rdb3(out)
        return out * 0.2 + x


class RRDBNet(nn.Module):
    """Real-ESRGAN generator network (x4, no pixel-unshuffle stem, matching
    RealESRGAN_x4plus / RealESRGAN_x4plus_anime_6B checkpoint layouts).

    state_dict key names below (conv_first, body.<i>, conv_body,
    conv_up1/conv_up2, conv_hr, conv_last) are chosen to match the
    checkpoints' own key names, since load_state_dict() requires an exact
    key match.
    """

    def __init__(
        self,
        num_in_ch: int = 3,
        num_out_ch: int = 3,
        num_feat: int = 64,
        num_block: int = 6,
        num_grow_ch: int = 32,
        scale: int = 4,
    ) -> None:
        super().__init__()
        self.scale = scale
        self.conv_first = nn.Conv2d(num_in_ch, num_feat, 3, 1, 1)
        self.body = nn.Sequential(*[RRDB(num_feat, num_grow_ch) for _ in range(num_block)])
        self.conv_body = nn.Conv2d(num_feat, num_feat, 3, 1, 1)
        # Upsample by 2x twice (4x total) via nearest-neighbor + conv, as in
        # the reference implementation (avoids checkerboard artifacts from
        # transposed convolution).
        self.conv_up1 = nn.Conv2d(num_feat, num_feat, 3, 1, 1)
        self.conv_up2 = nn.Conv2d(num_feat, num_feat, 3, 1, 1)
        self.conv_hr = nn.Conv2d(num_feat, num_feat, 3, 1, 1)
        self.conv_last = nn.Conv2d(num_feat, num_out_ch, 3, 1, 1)
        self.lrelu = nn.LeakyReLU(negative_slope=0.2, inplace=True)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        feat = self.conv_first(x)
        body_feat = self.conv_body(self.body(feat))
        feat = feat + body_feat
        feat = self.lrelu(self.conv_up1(F.interpolate(feat, scale_factor=2, mode="nearest")))
        feat = self.lrelu(self.conv_up2(F.interpolate(feat, scale_factor=2, mode="nearest")))
        out = self.conv_last(self.lrelu(self.conv_hr(feat)))
        return out


def load_state_dict_safely(pth_path: Path) -> dict:
    """Loads a Real-ESRGAN .pth checkpoint with weights_only=True (see the
    module docstring for why this is required, not optional), and returns
    the state_dict to feed into RRDBNet.load_state_dict()."""
    checkpoint = torch.load(str(pth_path), map_location="cpu", weights_only=True)

    if isinstance(checkpoint, dict) and "params_ema" in checkpoint:
        return checkpoint["params_ema"]
    if isinstance(checkpoint, dict) and "params" in checkpoint:
        return checkpoint["params"]
    if isinstance(checkpoint, dict):
        # Some mirrors store the bare state_dict at the top level (no
        # 'params'/'params_ema' wrapper key).
        return checkpoint
    raise ValueError(
        f"Unrecognized checkpoint structure in {pth_path} "
        f"(expected a dict with 'params_ema', 'params', or a bare state_dict)."
    )


def export(pth_path: Path, onnx_path: Path) -> None:
    model = RRDBNet(num_in_ch=3, num_out_ch=3, num_feat=64, num_block=6, num_grow_ch=32, scale=4)
    state_dict = load_state_dict_safely(pth_path)
    model.load_state_dict(state_dict, strict=True)
    model.eval()

    dummy = torch.randn(1, 3, 64, 64)
    onnx_path.parent.mkdir(parents=True, exist_ok=True)
    tmp_path = onnx_path.with_name(onnx_path.name + ".part")

    # dynamo=False: pin to the classic TorchScript-based exporter. Newer
    # torch releases default to dynamo=True, which additionally requires
    # the `onnxscript` package -- keeping dynamo=False means this script's
    # only dependencies stay `torch` and `onnx`, matching what's documented
    # in plugin/README.md and download_models.py's on-success message.
    torch.onnx.export(
        model,
        dummy,
        str(tmp_path),
        input_names=["input"],
        output_names=["output"],
        dynamic_axes={
            "input": {2: "height", 3: "width"},
            "output": {2: "height", 3: "width"},
        },
        opset_version=17,
        dynamo=False,
    )
    tmp_path.replace(onnx_path)
    print(f"Exported {onnx_path} ({onnx_path.stat().st_size / (1 << 20):.1f} MiB)")

    _verify_with_onnxruntime(onnx_path)


def _verify_with_onnxruntime(onnx_path: Path) -> None:
    """Best-effort sanity check: run a small random input through the
    exported model with onnxruntime, if it's installed. Skipped (not
    failed) when onnxruntime is unavailable, since it's not a hard
    dependency of this export script."""
    try:
        import numpy as np
        import onnxruntime as ort
    except ImportError:
        print("onnxruntime (or numpy) not installed; skipping load/inference verification.")
        return

    session = ort.InferenceSession(str(onnx_path), providers=["CPUExecutionProvider"])
    dummy = np.random.randn(1, 3, 32, 32).astype(np.float32)
    input_name = session.get_inputs()[0].name
    outputs = session.run(None, {input_name: dummy})
    out_shape = outputs[0].shape
    expected = (1, 3, 128, 128)  # 32x32 input, scale=4
    if tuple(out_shape) != expected:
        raise RuntimeError(
            f"Verification failed: expected output shape {expected}, got {tuple(out_shape)}"
        )
    print(f"Verified with onnxruntime: input (1,3,32,32) -> output {tuple(out_shape)} OK.")


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(__doc__)
        print("Usage: python3 export_anime_onnx.py <input.pth> <output.onnx>", file=sys.stderr)
        return 1

    pth_path = Path(argv[0])
    onnx_path = Path(argv[1])

    if not pth_path.is_file():
        print(f"ERROR: input file not found: {pth_path}", file=sys.stderr)
        return 1

    export(pth_path, onnx_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
