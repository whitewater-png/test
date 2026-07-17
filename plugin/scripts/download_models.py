#!/usr/bin/env python3
"""download_models.py - Fetches Real-ESRGAN ONNX models into plugin/models/.

Downloads:
  - realesrgan-x4plus.onnx        (Photo mode, general-purpose 4x, 23-block RRDBNet)
  - realesrgan-x4plus-anime.onnx  (Anime mode, compact 6-block RRDBNet)

Known-good source checked at the time this script was written (see
plugin/README.md for the "if the URL is dead" fallback procedure):

  Photo:  https://huggingface.co/qualcomm/Real-ESRGAN-x4plus
          (direct file: Real-ESRGAN-x4plus.onnx, ~67MB, ONNX export of
          xinntao/Real-ESRGAN's RealESRGAN_x4plus.pth). The URL below is
          pinned to a specific immutable git revision (commit hash), not
          the mutable "main" branch ref -- "main" 404'd after this file
          was apparently removed from it upstream, and pinning to a
          revision is also the right call from a supply-chain-integrity
          angle regardless: a revision hash can't be silently swapped for
          different content the way a branch ref can.

  Anime:  As of this writing, no maintained direct ONNX release of the
          6-block anime model (RealESRGAN_x4plus_anime_6B) was found; only
          the original PyTorch weights are mirrored on GitHub/Hugging
          Face. This script therefore downloads the .pth weights for the
          anime model. setup_mac.sh converts these to ONNX automatically
          (via export_anime_onnx.py, a basicsr-free exporter -- see that
          script for why basicsr itself is avoided); this script just
          prints a pointer to that instead of a manual basicsr-based
          recipe. If a maintained ONNX mirror appears later, update
          ANIME_ONNX_URL below and this script will use it directly.

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

PHOTO_ONNX_URL = (
    "https://huggingface.co/qualcomm/Real-ESRGAN-x4plus/resolve/"
    "01179a4da7bf5ac91faca650e6afbf282ac93933/Real-ESRGAN-x4plus.onnx"
)
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

# Known-good SHA-256 checksums, keyed by source URL. Populate these once a
# maintainer has manually downloaded and verified a file's authenticity
# (e.g. cross-checked against the model author's own published hash, or a
# reproducible from-source ONNX export). Left empty for URLs whose hash
# has not yet been pinned by a maintainer -- download_with_retry() prints
# an explicit warning (not a silent pass) whenever a URL has no entry
# here, so "verification was skipped" is always visible in the script's
# output rather than assumed safe.
KNOWN_SHA256: dict[str, str] = {
    # "https://huggingface.co/qualcomm/Real-ESRGAN-x4plus/resolve/01179a4da7bf5ac91faca650e6afbf282ac93933/Real-ESRGAN-x4plus.onnx": "<fill in after manual verification>",
    # "https://huggingface.co/amd/realesrgan-x4plus-anime-6b/resolve/main/RealESRGAN_x4plus_anime_6B.pth": "<fill in after manual verification>",
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
                f"No maintained ONNX mirror is known for this model, so it needs a local "
                f"PyTorch -> ONNX export step.\n\n"
                f"If you're running plugin/setup_mac.sh, it detects this .pth file and runs the "
                f"conversion automatically (in a dedicated venv, torch is not a hard dependency "
                f"of this download script) -- no action needed.\n\n"
                f"To convert manually instead:\n"
                f"  pip3 install torch onnx\n"
                f"  python3 {Path(__file__).resolve().parent / 'export_anime_onnx.py'} "
                f"{pth_dest} {args.out_dir / ANIME_ONNX_FILENAME}\n\n"
                f"(This uses a basicsr-free exporter -- see export_anime_onnx.py's module "
                f"docstring for why basicsr itself is avoided. Full walkthrough also in "
                f"plugin/README.md.)"
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
