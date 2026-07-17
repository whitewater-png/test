#!/usr/bin/env python3
"""download_models.py - Fetches Real-ESRGAN PyTorch weights into
plugin/models/, for local conversion to ONNX (see export_realesrgan_onnx.py
and setup_mac.sh step 5).

Downloads:
  - RealESRGAN_x4plus.pth           (Photo mode, general-purpose 4x, 23-block RRDBNet)
  - RealESRGAN_x4plus_anime_6B.pth  (Anime mode, compact 6-block RRDBNet)

Neither model is downloaded as a pre-exported ONNX file. Both are converted
locally from the official PyTorch weights, via
plugin/scripts/export_realesrgan_onnx.py, which traces the model with
dynamic height/width axes so the resulting ONNX accepts arbitrary tile
sizes.

  Why not a pre-exported ONNX file for Photo mode? An earlier version of
  this script downloaded a pre-exported ONNX mirror
  (huggingface.co/qualcomm/Real-ESRGAN-x4plus), built for a fixed-shape NPU
  pipeline: it hardcodes a 1x3x128x128 input shape. Real hardware testing
  showed this fails for any tile that isn't exactly 128x128
  ("Got invalid dimensions for input ... Got: 8 Expected: 128"), which is
  incompatible with this plugin's tiled upscaling pipeline (tiles are not
  all 128x128). Downloading the official .pth and exporting locally with
  dynamic axes (the same approach already used for the Anime model) fixes
  this and unifies both models onto one conversion path.

Known-good source checked at the time this script was written (see
plugin/README.md for the "if the URL is dead" fallback procedure):

  Photo:  https://github.com/xinntao/Real-ESRGAN/releases/download/v0.1.0/
          RealESRGAN_x4plus.pth (official xinntao/Real-ESRGAN GitHub
          release, ~67MB).

  Anime:  https://huggingface.co/amd/realesrgan-x4plus-anime-6b/resolve/main/
          RealESRGAN_x4plus_anime_6B.pth (mirror of xinntao/Real-ESRGAN's
          anime 6-block weights; no maintained direct ONNX release of this
          variant was found at time of writing).

setup_mac.sh converts both .pth files to ONNX automatically (via
export_realesrgan_onnx.py, a basicsr-free exporter that also auto-detects
each checkpoint's RRDB block count -- see that script for details); this
script just downloads the source weights and prints a pointer to the
manual conversion command as a fallback.

Security notes:
  - HTTPS-only: every URL in this script is validated to use the https://
    scheme before any request is made (see require_https()). This is a
    hard requirement, not a preference -- a downgrade to plain HTTP would
    make these downloads trivially tamperable in transit.
  - SHA-256 verification: downloaded files are checked against the known
    hashes in KNOWN_SHA256 below. Because upstream model files can be
    re-exported/updated by their maintainers (unlike a versioned release
    tarball), this script ships with the hashes observed at the time it
    was written; if a URL's content changes upstream, downloads will
    correctly start failing verification until this dict is updated by a
    maintainer who has manually confirmed the new file is legitimate.
    If a URL has no entry in KNOWN_SHA256 (hash not yet known/pinned),
    the script prints an explicit "verification skipped" WARNING rather
    than silently treating the file as trusted.
  - Atomic downloads: files are written to a ".part" temp path and only
    renamed to their final name after a full, verified download. A
    failed/interrupted download never leaves a partial file at the final
    destination path.

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
import hashlib
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

PHOTO_PTH_URL = "https://github.com/xinntao/Real-ESRGAN/releases/download/v0.1.0/RealESRGAN_x4plus.pth"
PHOTO_PTH_FILENAME = "RealESRGAN_x4plus.pth"
PHOTO_ONNX_FILENAME = "realesrgan-x4plus.onnx"

ANIME_PTH_URL = "https://huggingface.co/amd/realesrgan-x4plus-anime-6b/resolve/main/RealESRGAN_x4plus_anime_6B.pth"
ANIME_PTH_FILENAME = "RealESRGAN_x4plus_anime_6B.pth"
ANIME_ONNX_FILENAME = "realesrgan-x4plus-anime.onnx"

# Known-good SHA-256 checksums, keyed by source URL. Populate these once a
# maintainer has manually downloaded and verified a file's authenticity
# (e.g. cross-checked against the model author's own published hash, or a
# reproducible from-source ONNX export). Left empty for URLs whose hash
# has not yet been pinned by a maintainer -- download_with_retry() prints
# an explicit warning (not a silent pass) whenever a URL has no entry
# here, so "verification was skipped" is always visible in the script's
# output rather than assumed safe.
#
# Both hashes below are pinned from values observed on real hardware /
# widely-published official values, not guessed:
#   - Photo (RealESRGAN_x4plus.pth): the well-known published SHA-256 for
#     xinntao/Real-ESRGAN's official v0.1.0 GitHub release asset.
#   - Anime (RealESRGAN_x4plus_anime_6B.pth): the SHA-256 actually measured
#     from a real download of amd/realesrgan-x4plus-anime-6b's mirrored
#     file during on-device testing of this plugin.
# If a real download's hash ever fails to match either pinned value, that
# is surfaced to the user as a checksum-mismatch error (see
# download_with_retry() below) rather than silently accepted -- so an
# eventual upstream file change would be caught, not masked, by these
# pins.
KNOWN_SHA256: dict[str, str] = {
    PHOTO_PTH_URL: "4fa0d38905f75ac06eb49a7951b426670021be3018265fd191d2125df9d682f1",
    ANIME_PTH_URL: "f872d837d3c90ed2e05227bed711af5671a6fd1c9f7d7e91c911a61f155e99da",
}

MAX_RETRIES = 4
INITIAL_BACKOFF_SEC = 2


def require_https(url: str) -> None:
    """Raises ValueError if `url` does not use the https:// scheme.

    Called before every network request in this script -- no download
    path in this file is permitted to fall back to plain HTTP."""
    scheme = urllib.parse.urlsplit(url).scheme.lower()
    if scheme != "https":
        raise ValueError(f"refusing to download non-HTTPS URL (scheme={scheme!r}): {url}")


def sha256_of(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def download_with_retry(url: str, dest: Path) -> bool:
    """Downloads url to dest with exponential backoff (2s/4s/8s/16s),
    HTTPS-only, atomically (via a .part temp file), with SHA-256
    verification against KNOWN_SHA256 when a hash is pinned for this URL.

    Returns True on success (including "verification skipped, no known
    hash"), False if all retries were exhausted or verification failed."""
    require_https(url)

    part_path = dest.with_name(dest.name + ".part")
    backoff = INITIAL_BACKOFF_SEC
    for attempt in range(1, MAX_RETRIES + 1):
        # Always start this attempt from a clean slate -- never resume a
        # partial download into a checksum-verified file.
        part_path.unlink(missing_ok=True)
        try:
            print(f"  [{attempt}/{MAX_RETRIES}] GET {url}")
            req = urllib.request.Request(url, headers={"User-Agent": "ai-upscale-plugin/1.0"})
            with urllib.request.urlopen(req, timeout=60) as resp, open(part_path, "wb") as f:
                total = 0
                while True:
                    chunk = resp.read(1 << 20)
                    if not chunk:
                        break
                    f.write(chunk)
                    total += len(chunk)

            expected_hash = KNOWN_SHA256.get(url)
            actual_hash = sha256_of(part_path)
            if expected_hash:
                if actual_hash.lower() != expected_hash.lower():
                    print(f"  CHECKSUM MISMATCH for {dest.name}: expected {expected_hash}, got {actual_hash}")
                    part_path.unlink(missing_ok=True)
                    return False
                print(f"  Checksum verified (sha256={actual_hash}).")
            else:
                print(f"  WARNING: no known SHA-256 pinned for this URL; verification SKIPPED. "
                      f"See KNOWN_SHA256 in this script -- treat {dest.name} as unverified until a "
                      f"maintainer pins its hash.")
                print(f"  Downloaded file sha256={actual_hash}")
                print(f"  If you have manually confirmed this file is legitimate, you can pin this "
                      f"value in KNOWN_SHA256[\"{url}\"] so future downloads are verified automatically.")

            # Atomic rename: dest either doesn't exist, or is a complete,
            # (when possible) verified file -- never a partial download.
            part_path.replace(dest)
            print(f"  OK: wrote {dest} ({total / (1 << 20):.1f} MiB)")
            return True
        except (urllib.error.URLError, urllib.error.HTTPError, TimeoutError, OSError) as exc:
            print(f"  Failed: {exc}")
            part_path.unlink(missing_ok=True)
            if attempt < MAX_RETRIES:
                print(f"  Retrying in {backoff}s...")
                time.sleep(backoff)
                backoff *= 2
    part_path.unlink(missing_ok=True)
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
    exporter = Path(__file__).resolve().parent / "export_realesrgan_onnx.py"

    def _download_pth_weights(label: str, url: str, pth_filename: str, onnx_filename: str) -> bool:
        pth_dest = args.out_dir / pth_filename
        if not download_with_retry(url, pth_dest):
            print(f"ERROR: could not download {url}")
            print("See plugin/README.md 'モデル取得先が利用できない場合' for alternatives.")
            return False
        print(
            f"\nDownloaded {label} PyTorch weights to {pth_dest}.\n"
            f"This needs a local PyTorch -> ONNX export step (dynamic input shape, so the "
            f"result works for any tile size -- see export_realesrgan_onnx.py's module "
            f"docstring).\n\n"
            f"If you're running plugin/setup_mac.sh, it detects this .pth file and runs the "
            f"conversion automatically (in a dedicated venv, torch is not a hard dependency "
            f"of this download script) -- no action needed.\n\n"
            f"To convert manually instead:\n"
            f"  pip3 install torch onnx\n"
            f"  python3 {exporter} {pth_dest} {args.out_dir / onnx_filename}\n\n"
            f"(The RRDB block count is auto-detected from the checkpoint; use --num-block to "
            f"override if needed. Full walkthrough also in plugin/README.md.)"
        )
        return True

    print("Downloading Photo mode model source weights...")
    if not _download_pth_weights("Photo", PHOTO_PTH_URL, PHOTO_PTH_FILENAME, PHOTO_ONNX_FILENAME):
        ok = False

    print("\nDownloading Anime mode model source weights...")
    if not _download_pth_weights("Anime", ANIME_PTH_URL, ANIME_PTH_FILENAME, ANIME_ONNX_FILENAME):
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
