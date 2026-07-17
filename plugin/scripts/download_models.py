#!/usr/bin/env python3
"""download_models.py - Fetches Real-ESRGAN ONNX models into plugin/models/.

Downloads:
  - realesrgan-x4plus.onnx        (Photo mode, general-purpose 4x, 23-block RRDBNet)
  - realesrgan-x4plus-anime.onnx  (Anime mode, compact 6-block RRDBNet)

Known-good source checked at the time this script was written (see
plugin/README.md for the "if the URL is dead" fallback procedure):

  Photo:  https://huggingface.co/qualcomm/Real-ESRGAN-x4plus
          (direct file: Real-ESRGAN-x4plus.onnx, ~67MB, ONNX export of
          xinntao/Real-ESRGAN's RealESRGAN_x4plus.pth)

  Anime:  As of this writing, no maintained direct ONNX release of the
          6-block anime model (RealESRGAN_x4plus_anime_6B) was found; only
          the original PyTorch weights are mirrored on GitHub/Hugging
          Face. This script therefore downloads the .pth weights for the
          anime model and prints the ONNX export command from
          plugin/README.md rather than silently producing a fake file.
          If a maintained ONNX mirror appears later, update
          ANIME_ONNX_URL below and this script will use it directly.

Note on this repository's dev/CI environment: outbound access to
huggingface.co and raw GitHub release downloads may be blocked by
network policy in sandboxed environments (this was the case in the
environment this plugin was originally developed in). If downloads fail
here, run this script on a machine with normal internet access, or follow
the manual export steps in plugin/README.md.

Usage:
    python3 download_models.py [--out-dir plugin/models]
"""
from __future__ import annotations

import argparse
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

PHOTO_ONNX_URL = "https://huggingface.co/qualcomm/Real-ESRGAN-x4plus/resolve/main/Real-ESRGAN-x4plus.onnx"
PHOTO_ONNX_FILENAME = "realesrgan-x4plus.onnx"

# No confirmed maintained ONNX mirror for the anime 6B model at time of
# writing; left as None so the script clearly reports "not available"
# rather than guessing a URL that 404s. See README.md for the PyTorch ->
# ONNX export fallback (which is the actually-verified path for this
# model).
ANIME_ONNX_URL = None
ANIME_PTH_URL = "https://huggingface.co/amd/realesrgan-x4plus-anime-6b/resolve/main/RealESRGAN_x4plus_anime_6B.pth"
ANIME_PTH_FILENAME = "RealESRGAN_x4plus_anime_6B.pth"
ANIME_ONNX_FILENAME = "realesrgan-x4plus-anime.onnx"

MAX_RETRIES = 4
INITIAL_BACKOFF_SEC = 2


def download_with_retry(url: str, dest: Path) -> bool:
    """Downloads url to dest with exponential backoff (2s/4s/8s/16s).
    Returns True on success, False if all retries were exhausted."""
    backoff = INITIAL_BACKOFF_SEC
    for attempt in range(1, MAX_RETRIES + 1):
        try:
            print(f"  [{attempt}/{MAX_RETRIES}] GET {url}")
            req = urllib.request.Request(url, headers={"User-Agent": "ai-upscale-plugin/1.0"})
            with urllib.request.urlopen(req, timeout=60) as resp, open(dest, "wb") as f:
                total = 0
                while True:
                    chunk = resp.read(1 << 20)
                    if not chunk:
                        break
                    f.write(chunk)
                    total += len(chunk)
            print(f"  OK: wrote {dest} ({total / (1 << 20):.1f} MiB)")
            return True
        except (urllib.error.URLError, urllib.error.HTTPError, TimeoutError, OSError) as exc:
            print(f"  Failed: {exc}")
            if attempt < MAX_RETRIES:
                print(f"  Retrying in {backoff}s...")
                time.sleep(backoff)
                backoff *= 2
    return False


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--out-dir", type=Path, default=Path(__file__).resolve().parent.parent / "models",
        help="Destination directory for downloaded models (default: plugin/models)",
    )
    args = parser.parse_args(argv)
    args.out_dir.mkdir(parents=True, exist_ok=True)

    ok = True

    print("Downloading Photo mode model (realesrgan-x4plus)...")
    photo_dest = args.out_dir / PHOTO_ONNX_FILENAME
    if not download_with_retry(PHOTO_ONNX_URL, photo_dest):
        print(f"ERROR: could not download {PHOTO_ONNX_URL}")
        print("See plugin/README.md 'モデル取得先が利用できない場合' for the manual export fallback.")
        ok = False

    print("\nDownloading Anime mode model source weights...")
    if ANIME_ONNX_URL:
        anime_dest = args.out_dir / ANIME_ONNX_FILENAME
        if not download_with_retry(ANIME_ONNX_URL, anime_dest):
            print(f"ERROR: could not download {ANIME_ONNX_URL}")
            ok = False
    else:
        pth_dest = args.out_dir / ANIME_PTH_FILENAME
        if download_with_retry(ANIME_PTH_URL, pth_dest):
            print(
                f"\nDownloaded PyTorch weights to {pth_dest}.\n"
                f"No maintained ONNX mirror is known for this model; export it yourself:\n\n"
                f"  pip3 install torch basicsr realesrgan onnx\n"
                f"  python3 -c \"\n"
                f"import torch\n"
                f"from basicsr.archs.rrdbnet_arch import RRDBNet\n"
                f"model = RRDBNet(num_in_ch=3, num_out_ch=3, num_feat=64, num_block=6, num_grow_ch=32, scale=4)\n"
                f"state = torch.load('{pth_dest.name}', map_location='cpu')\n"
                f"model.load_state_dict(state['params_ema'] if 'params_ema' in state else state)\n"
                f"model.eval()\n"
                f"dummy = torch.randn(1, 3, 64, 64)\n"
                f"torch.onnx.export(model, dummy, '{ANIME_ONNX_FILENAME}',\n"
                f"    input_names=['input'], output_names=['output'],\n"
                f"    dynamic_axes={{'input': {{2: 'height', 3: 'width'}}, 'output': {{2: 'height', 3: 'width'}}}},\n"
                f"    opset_version=13)\n"
                f"\"\n\n"
                f"Then move the resulting {ANIME_ONNX_FILENAME} into {args.out_dir}/.\n"
                f"(Full walkthrough also in plugin/README.md.)"
            )
        else:
            print(f"ERROR: could not download {ANIME_PTH_URL} either.")
            print("See plugin/README.md 'モデル取得先が利用できない場合' for alternatives.")
            ok = False

    if not ok:
        print(
            "\nOne or more downloads failed. This is non-fatal for local development: "
            "you can still test the inference pipeline with scripts/make_test_model.py, "
            "which generates a tiny synthetic ONNX model with no network access required."
        )
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
