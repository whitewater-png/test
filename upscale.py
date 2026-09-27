#!/usr/bin/env python3
"""upscale.py - 動画ファイルをAI/高品質フィルタでアップスケールするCLIツール

動画編集で素材を拡大すると画質が荒くなる問題を軽減するために作られたツールです。
可能であれば realesrgan-ncnn-vulkan (Real-ESRGAN) を使ってフレーム単位でAIアップスケールし、
それが無ければ ffmpeg の高品質フィルタ (lanczos + unsharp) にフォールバックします。

使い方:
    python3 upscale.py input.mp4 -o output.mp4 --scale 2 --mode photo

必要環境:
    - ffmpeg / ffprobe (必須)
    - realesrgan-ncnn-vulkan (任意。PATHにあれば自動的に使用されます)

外部Pythonパッケージには依存していません（標準ライブラリのみ）。
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

# realesrgan-ncnn-vulkan の実行ファイル名候補
REALESRGAN_BIN_CANDIDATES = ["realesrgan-ncnn-vulkan"]

# mode -> realesrgan モデル名
REALESRGAN_MODELS = {
    "photo": "realesrgan-x4plus",
    "anime": "realesrgan-x4plus-anime",
}


class UpscaleError(RuntimeError):
    """ユーザーに分かりやすいエラーメッセージを表示するための例外。"""


def check_ffmpeg_available() -> None:
    """ffmpeg / ffprobe がインストールされているか確認する。"""
    missing = [tool for tool in ("ffmpeg", "ffprobe") if shutil.which(tool) is None]
    if missing:
        raise UpscaleError(
            "ffmpeg が見つかりません（不足しているコマンド: {}）。\n"
            "以下のいずれかの方法でインストールしてください。\n"
            "  Ubuntu/Debian: sudo apt-get update && sudo apt-get install -y ffmpeg\n"
            "  macOS (Homebrew): brew install ffmpeg\n"
            "  Windows: https://www.gyan.dev/ffmpeg/builds/ から実行ファイルを取得し、PATHに追加してください\n"
            .format(", ".join(missing))
        )


def find_realesrgan_binary() -> str | None:
    """PATH上に realesrgan-ncnn-vulkan があればそのパスを返す。無ければ None。"""
    for name in REALESRGAN_BIN_CANDIDATES:
        path = shutil.which(name)
        if path:
            return path
    return None


def run(cmd: list[str], **kwargs) -> subprocess.CompletedProcess:
    """subprocess.run のラッパー。失敗時にコマンドと標準エラーを表示する。"""
    result = subprocess.run(
        cmd,
        stdout=kwargs.pop("stdout", subprocess.PIPE),
        stderr=kwargs.pop("stderr", subprocess.PIPE),
        text=True,
        **kwargs,
    )
    if result.returncode != 0:
        raise UpscaleError(
            "コマンドの実行に失敗しました: {}\n{}".format(
                " ".join(cmd), result.stderr.strip()
            )
        )
    return result


def probe_video(input_path: Path) -> dict:
    """ffprobe で fps・解像度・音声有無を取得する。"""
    # フレームレート取得
    fps_result = run(
        [
            "ffprobe",
            "-v",
            "error",
            "-select_streams",
            "v:0",
            "-show_entries",
            "stream=r_frame_rate,width,height",
            "-of",
            "csv=p=0",
            str(input_path),
        ]
    )
    line = fps_result.stdout.strip().splitlines()[0]
    width_str, height_str, fps_str = line.split(",")
    if "/" in fps_str:
        num, den = fps_str.split("/")
        fps = float(num) / float(den) if float(den) != 0 else float(num)
    else:
        fps = float(fps_str)

    # 音声ストリームの有無
    audio_result = run(
        [
            "ffprobe",
            "-v",
            "error",
            "-select_streams",
            "a",
            "-show_entries",
            "stream=index",
            "-of",
            "csv=p=0",
            str(input_path),
        ]
    )
    has_audio = bool(audio_result.stdout.strip())

    return {
        "width": int(width_str),
        "height": int(height_str),
        "fps": fps,
        "has_audio": has_audio,
    }


def extract_frames(input_path: Path, frames_dir: Path, fps: float) -> None:
    run(
        [
            "ffmpeg",
            "-y",
            "-i",
            str(input_path),
            "-vsync",
            "0",
            "-qscale:v",
            "1",
            str(frames_dir / "frame_%08d.png"),
        ]
    )


def upscale_frames_with_realesrgan(
    realesrgan_bin: str,
    frames_dir: Path,
    upscaled_dir: Path,
    scale: int,
    mode: str,
) -> None:
    model = REALESRGAN_MODELS[mode]
    run(
        [
            realesrgan_bin,
            "-i",
            str(frames_dir),
            "-o",
            str(upscaled_dir),
            "-n",
            model,
            "-s",
            str(scale),
            "-f",
            "png",
        ]
    )


def encode_frames_to_video(
    frames_dir: Path,
    input_path: Path,
    output_path: Path,
    fps: float,
    has_audio: bool,
) -> None:
    cmd = [
        "ffmpeg",
        "-y",
        "-framerate",
        str(fps),
        "-i",
        str(frames_dir / "frame_%08d.png"),
    ]
    if has_audio:
        cmd += ["-i", str(input_path)]
    cmd += [
        "-map",
        "0:v:0",
    ]
    if has_audio:
        cmd += ["-map", "1:a:0"]
    cmd += [
        "-c:v",
        "libx264",
        "-pix_fmt",
        "yuv420p",
        "-crf",
        "18",
        "-r",
        str(fps),
    ]
    if has_audio:
        cmd += ["-c:a", "aac", "-shortest"]
    cmd += [str(output_path)]
    run(cmd)


def upscale_with_realesrgan(
    realesrgan_bin: str,
    input_path: Path,
    output_path: Path,
    scale: int,
    mode: str,
    info: dict,
) -> None:
    with tempfile.TemporaryDirectory(prefix="upscale_frames_") as tmp:
        tmp_path = Path(tmp)
        frames_dir = tmp_path / "frames"
        upscaled_dir = tmp_path / "upscaled"
        frames_dir.mkdir()
        upscaled_dir.mkdir()

        print("[1/3] フレームを抽出しています...")
        extract_frames(input_path, frames_dir, info["fps"])

        print("[2/3] realesrgan-ncnn-vulkan でフレームをアップスケールしています "
              f"(mode={mode}, scale={scale})...")
        upscale_frames_with_realesrgan(realesrgan_bin, frames_dir, upscaled_dir, scale, mode)

        print("[3/3] 動画に再結合しています...")
        encode_frames_to_video(upscaled_dir, input_path, output_path, info["fps"], info["has_audio"])


def upscale_with_ffmpeg_fallback(
    input_path: Path,
    output_path: Path,
    scale: int,
    mode: str,
    info: dict,
) -> None:
    """realesrgan が無い場合のフォールバック。
    フレーム抽出なしで1パス処理する。lanczosスケーリング + unsharpで
    輪郭を強調し、単純拡大より画質劣化を抑える。
    """
    print(f"[1/1] ffmpegフィルタでアップスケールしています (mode={mode}, scale={scale})...")

    scale_filter = f"scale=iw*{scale}:ih*{scale}:flags=lanczos"

    if mode == "anime":
        # アニメ/イラスト向け: エッジをより強く保持するシャープ設定
        unsharp_filter = "unsharp=5:5:1.2:5:5:0.0"
    else:
        # 実写向け: 過度なリンギングを避けるやや控えめなシャープ設定
        unsharp_filter = "unsharp=5:5:0.8:5:5:0.0"

    vf = f"{scale_filter},{unsharp_filter}"

    cmd = [
        "ffmpeg",
        "-y",
        "-i",
        str(input_path),
        "-vf",
        vf,
        "-c:v",
        "libx264",
        "-pix_fmt",
        "yuv420p",
        "-crf",
        "18",
        "-r",
        str(info["fps"]),
    ]
    if info["has_audio"]:
        cmd += ["-c:a", "aac"]
    else:
        cmd += ["-an"]
    cmd += [str(output_path)]
    run(cmd)


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="動画をAI/高品質フィルタでアップスケールします。",
    )
    parser.add_argument("input", type=Path, help="入力動画ファイル")
    parser.add_argument(
        "-o", "--output", type=Path, default=None,
        help="出力動画ファイル (省略時は '<入力ファイル名>_upscaled.mp4')",
    )
    parser.add_argument(
        "--scale", type=int, choices=(2, 4), default=2,
        help="拡大倍率 (2 または 4、デフォルト: 2)",
    )
    parser.add_argument(
        "--mode", choices=("photo", "anime"), default="photo",
        help="photo=実写向け(Sharp相当) / anime=アニメ・イラスト向け(Cartoon相当) (デフォルト: photo)",
    )
    parser.add_argument(
        "--engine", choices=("auto", "realesrgan", "ffmpeg"), default="auto",
        help="使用するアップスケールエンジン (デフォルト: auto = realesrganがあれば使用、無ければffmpeg)",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_arg_parser()
    args = parser.parse_args(argv)

    try:
        check_ffmpeg_available()

        input_path: Path = args.input
        if not input_path.exists():
            raise UpscaleError(f"入力ファイルが見つかりません: {input_path}")

        output_path: Path = args.output or input_path.with_name(
            f"{input_path.stem}_upscaled.mp4"
        )

        info = probe_video(input_path)

        realesrgan_bin = None
        if args.engine in ("auto", "realesrgan"):
            realesrgan_bin = find_realesrgan_binary()
            if args.engine == "realesrgan" and realesrgan_bin is None:
                raise UpscaleError(
                    "--engine realesrgan が指定されましたが、realesrgan-ncnn-vulkan が"
                    "PATH上に見つかりません。インストールするか --engine ffmpeg を"
                    "指定してください。"
                )

        if realesrgan_bin is not None:
            print(f"エンジン: realesrgan-ncnn-vulkan ({realesrgan_bin})")
            upscale_with_realesrgan(
                realesrgan_bin, input_path, output_path, args.scale, args.mode, info
            )
        else:
            print("エンジン: ffmpeg (lanczos + unsharp フォールバック)")
            upscale_with_ffmpeg_fallback(
                input_path, output_path, args.scale, args.mode, info
            )

        print(f"完了しました: {output_path}")
        return 0

    except UpscaleError as exc:
        print(f"エラー: {exc}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("\n中断されました。", file=sys.stderr)
        return 130


if __name__ == "__main__":
    sys.exit(main())
