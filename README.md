# test

## AI動画アップスケールパイプライン

動画編集で低解像度の素材を拡大すると、補間の限界によりぼやけやジャギーが目立ち画質が荒くなってしまいます。このリポジトリでは、その問題を軽減するためのAI動画アップスケールツール `upscale.py` を提供しています。

手法の比較や選定の考え方は [`docs/アップスケール手法比較.md`](docs/アップスケール手法比較.md) を参照してください（市販製品の ScaleUp / Topaz Video AI との比較、OSSの Real-ESRGAN / video2x / waifu2x / ffmpegフィルタとの比較を含みます）。

### 必要環境

- Python 3.11以上（標準ライブラリのみ使用、追加パッケージのインストールは不要）
- `ffmpeg` / `ffprobe`（必須）
  - Ubuntu/Debian: `sudo apt-get install -y ffmpeg`
  - macOS (Homebrew): `brew install ffmpeg`
  - Windows: https://www.gyan.dev/ffmpeg/builds/ からダウンロードしPATHに追加
- `realesrgan-ncnn-vulkan`（任意）
  - PATH上にあれば自動的にAIアップスケール(Real-ESRGAN)を使用します
  - 無い場合はffmpegの高品質フィルタ（lanczos + unsharp）に自動的にフォールバックします

### 使い方

```bash
python3 upscale.py input.mp4 -o output.mp4 --scale 2 --mode photo
```

主なオプション:

| オプション | 説明 |
|---|---|
| `-o, --output` | 出力ファイルパス（省略時は `<入力名>_upscaled.mp4`） |
| `--scale {2,4}` | 拡大倍率（デフォルト: 2） |
| `--mode {photo,anime}` | `photo`=実写向け、`anime`=アニメ・イラスト向け（ScaleUpのSharp/Cartoonに相当） |
| `--engine {auto,realesrgan,ffmpeg}` | 使用するエンジンを明示的に指定（デフォルト: `auto`） |

処理の流れ:

1. `ffprobe` で入力動画のフレームレートや音声有無を取得
2. `realesrgan-ncnn-vulkan` が利用可能な場合: フレームをPNGとして抽出 → フレームごとにAIアップスケール → 元動画の音声・fpsを保持して再結合（H.264, yuv420p, crf 18）
3. 利用できない場合: ffmpegの `scale`（lanczos）+ `unsharp` フィルタで1パス処理

## Premiere Pro / After Effects 用ネイティブプラグイン

書き出し前処理としてではなく、編集ソフト内で完結させてアップスケールしたい場合は
[`plugin/`](plugin/) に FlashBack Japan の ScaleUp と同形態のAIアップスケール・エフェクト
プラグイン（"AI Upscale"）のソースを用意しています。Adobe SDK非依存のコア推論エンジン
（ONNX Runtime + タイル処理）と、AE/Premiere SDK層に分離した構成で、コア層はこのリポジトリの
Linux開発環境でもビルド・テストできます。詳細は [`plugin/README.md`](plugin/README.md) を
参照してください。
