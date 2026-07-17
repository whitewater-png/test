# AI Upscale プラグイン開発ガイド

FlashBack Japan の [ScaleUp](https://flashbackj.com/product/scaleup) と同形態の、
Premiere Pro / After Effects 用ネイティブAIアップスケール・エフェクトプラグインです。
編集ソフト内でエフェクトとして適用でき、Real-ESRGAN系のONNXモデルによる超解像処理で
クリップを2倍・4倍にアップスケールします（Photo=実写向け / Anime=アニメ・イラスト向け、
ScaleUpのSharp/Cartoonに相当）。

ルートの `upscale.py`（ffmpeg + realesrgan-ncnn-vulkanによる書き出し前処理ツール）とは別に、
こちらは編集ソフト内で完結させたい場合の選択肢です。両者の違いは
[`docs/アップスケール手法比較.md`](../docs/アップスケール手法比較.md) を参照してください。

## アーキテクチャ

```
+-------------------------------------------------------------+
|  Adobe After Effects / Premiere Pro                          |
|                                                                |
|   +--------------------------------------------------------+ |
|   |  src/plugin/ (AE SDK層。Adobe SDKが無いとビルド不可)    | |
|   |   AIUpscale.cpp/.h : PF_Cmd ディスパッチ、               | |
|   |                       PF_EffectWorld <-> ImageRGBA8 変換  | |
|   |   AIUpscalePiPL.r  : PiPLリソース (エフェクト名/カテゴリ) | |
|   +--------------------------|-------------------------------+ |
|                              | ImageRGBA8 (core非依存の共通形式) |
|   +--------------------------v-------------------------------+ |
|   |  src/core/ (Adobe非依存。この環境で単体ビルド・テスト可) | |
|   |   onnx_upscaler.cpp/.h : ONNX Runtime推論、前処理/後処理  | |
|   |   tile.cpp/.h          : タイル分割・オーバーラップ合成    | |
|   +------------------------------------------------------------+ |
+-------------------------------------------------------------+

   src/cli/upscale_cli.cpp : src/core/ を単体で叩くテスト用CLI
                              (PNG入出力、Adobe/GPU不要)
```

コア層 (`src/core`) はAdobe SDKに一切依存しないため、Adobe SDKもGPUも無い
Linux環境でも `upscale_cli` を通してビルド・テストできます。AE/Premiere用の
`src/plugin` 層は薄いアダプタで、`PF_EffectWorld`（BGRA_8u前提）を
`ImageRGBA8` に変換してコア層に渡すだけです。Adobe SDKが手に入り次第、
コア層はそのまま流用してプラグイン層だけをビルドできる構成にしています。

## 前提条件

| 依存 | 用途 | 入手先 |
|---|---|---|
| CMake 3.16+ | ビルドシステム | https://cmake.org/download/ |
| C++17 コンパイラ | Visual Studio 2022 (Windows) / Xcode (macOS) / g++ (Linux, core層のみ) | |
| ONNX Runtime (C++ API, プリビルト) | 推論エンジン | https://github.com/microsoft/onnxruntime/releases |
| Adobe After Effects SDK | `src/plugin` 層のビルドに必須。無償だがAdobeアカウントが必要 | https://developer.adobe.com/after-effects/ |

Adobe After Effects SDKは developer.adobe.com の "After Effects SDK" ページから
無償でダウンロードできます（要Adobeアカウントでのログイン）。ダウンロードした
SDKを展開したパスを `AE_SDK_PATH` としてCMakeに渡してください。SDK内の
`Examples/Headers`, `Examples/Util`, `Examples/Resources` 以下のヘッダ配置は
SDKのバージョンによって多少変わることがあるため、実際の配置に応じて
`plugin/CMakeLists.txt` の `target_include_directories(AIUpscale ...)` を
調整してください。

**このリポジトリにはAdobe SDKのヘッダ (`AE_Effect.h` など) は含まれていません。**
（Adobeのライセンス上、再配布不可のため）`AE_SDK_PATH` で参照する形にしています。

## ビルド手順

### コア層 + CLI（Adobe SDK不要、Linux/macOS/Windowsで共通）

```bash
# 1. ONNX Runtimeのプリビルトを展開 (例: Linux x64 CPU版)
#    https://github.com/microsoft/onnxruntime/releases から
#    onnxruntime-linux-x64-<version>.tgz 等を取得し展開

cmake -S plugin -B plugin/build \
  -DONNXRUNTIME_ROOT=/path/to/onnxruntime-linux-x64-<version> \
  -DCMAKE_BUILD_TYPE=Release
cmake --build plugin/build -j

# テスト用モデル生成（実モデル無しでパイプライン検証したい場合）
pip3 install onnx numpy
python3 plugin/scripts/make_test_model.py plugin/build/test_model_4x.onnx

# テスト画像生成 → アップスケール実行
python3 plugin/tests/make_test_image.py /tmp/in.png 64 64
./plugin/build/upscale_cli plugin/build/test_model_4x.onnx /tmp/in.png /tmp/out.png
python3 plugin/tests/check_png_size.py /tmp/out.png --expect 256 256
```

### Windows (.aex, AE SDKあり)

```powershell
cmake -S plugin -B plugin/build -G "Visual Studio 17 2022" -A x64 `
  -DONNXRUNTIME_ROOT=C:\path\to\onnxruntime-win-x64-<version> `
  -DAE_SDK_PATH=C:\path\to\AfterEffectsSDK
cmake --build plugin/build --config Release
```

Windows版は `AIUpscale.aex` として出力されます（`.dll` を `.aex` にリネームした
形態。CMakeLists.txt側で `SUFFIX ".aex"` を設定済み）。PiPLリソース
(`AIUpscalePiPL.r`) はSDK付属のリソースコンパイラ（PiPLtool / .rc変換フロー）で
`.rc` に変換し、プロジェクトのリンク対象に追加する必要があります。この変換は
SDKバージョンごとに手順が異なるため、SDK付属の `Examples/Skeleton` プロジェクトの
`.vcxproj` を参照して同様に設定してください（本リポジトリでは自動化していません）。

### macOS (.plugin バンドル、AE SDKあり)

```bash
cmake -S plugin -B plugin/build -G Xcode \
  -DONNXRUNTIME_ROOT=/path/to/onnxruntime-osx-<version> \
  -DAE_SDK_PATH=/path/to/AfterEffectsSDK
cmake --build plugin/build --config Release
```

macOS版はバンドル (`AIUpscale.plugin`) として出力されます。PiPLリソースは
Rezでコンパイルしてバンドルに埋め込む必要があります（SDK付属の
`Examples/Skeleton` Xcodeプロジェクトのビルドフェーズを参照）。
`src/plugin/Info.plist.in` はバンドルの `Info.plist` テンプレートです。

### Linux

Linux上ではAdobe SDKが提供されていないため `AIUpscale` ターゲットは
自動的にスキップされます（`AE_SDK_PATH` を指定してもメッセージを出して
スキップします）。`upscale_core` / `upscale_cli` は通常通りビルド・テスト可能です。

## モデルの入手

```bash
python3 plugin/scripts/download_models.py --out-dir plugin/models
```

`realesrgan-x4plus.onnx`（Photo用）と `realesrgan-x4plus-anime.onnx`（Anime用）を
`plugin/models/` にダウンロードします。

### モデル取得先が利用できない場合

- Photo用 (`realesrgan-x4plus`): 本スクリプトは Hugging Face の
  `qualcomm/Real-ESRGAN-x4plus` リポジトリの `Real-ESRGAN-x4plus.onnx` を
  取得します。URLが変わっている場合は https://huggingface.co/models?search=real-esrgan
  で ONNX 形式のミラーを探すか、下記のPyTorch→ONNXエクスポート手順を使ってください。
- Anime用 (`realesrgan-x4plus-anime`): 執筆時点で維持されているONNX直配布が
  見当たらなかったため、本スクリプトは `amd/realesrgan-x4plus-anime-6b` の
  `.pth`（PyTorch重み、xinntao/Real-ESRGAN由来）をダウンロードし、以下の
  エクスポート手順を案内します。

```bash
pip3 install torch basicsr realesrgan onnx
python3 -c "
import torch
from basicsr.archs.rrdbnet_arch import RRDBNet
model = RRDBNet(num_in_ch=3, num_out_ch=3, num_feat=64, num_block=6, num_grow_ch=32, scale=4)
state = torch.load('RealESRGAN_x4plus_anime_6B.pth', map_location='cpu')
model.load_state_dict(state['params_ema'] if 'params_ema' in state else state)
model.eval()
dummy = torch.randn(1, 3, 64, 64)
torch.onnx.export(model, dummy, 'realesrgan-x4plus-anime.onnx',
    input_names=['input'], output_names=['output'],
    dynamic_axes={'input': {2: 'height', 3: 'width'}, 'output': {2: 'height', 3: 'width'}},
    opset_version=13)
"
```

Photo用 (`realesrgan-x4plus`, 23-block RRDBNet) も同様のコードで
`num_block=23` としてPyTorch重み (`RealESRGAN_x4plus.pth`,
https://github.com/xinntao/Real-ESRGAN/releases) からエクスポートできます。

### インストール先

| 編集ソフト | プラグインの配置場所 |
|---|---|
| After Effects | `Adobe After Effects <version>/Support Files/Plug-ins/` |
| Premiere Pro | 共通プラグインフォルダ（MediaCore）: Windows `C:\Program Files\Common Files\Adobe\Plug-ins\Common\`、macOS `/Library/Application Support/Adobe/Common/Plug-ins/<version>/MediaCore/` |

`models/` ディレクトリはプラグイン本体（`.aex` / `.plugin`）と同じ場所に
配置してください（`AIUpscale.cpp` の `resolve_plugin_directory()` が
プラグインディレクトリ配下の `models/` を参照します）。

## 既知の制約

- **8bpc（8bit）カラーのみ対応。** 16bpc / 32bpc(float) プロジェクトでは動作しません
  (`PF_Cmd_RENDER` 内で `PF_WORLD_IS_DEEP` チェックによりエラーを返します)。
- **Premiereでのバッファ拡張は実機検証が必要です。** `PF_Cmd_FRAME_SETUP` で
  `out_data->width/height` を入力の scale 倍に拡大する実装（blur系エフェクトと
  同じ「バッファ拡張」方式）を採用していますが、この挙動をPremiere Proが
  期待通りに解釈するかは実際のPremiereビルドでの検証が必要です
  （AEでは確立されたパターンですが、PremiereのエフェクトホストはAEほど
  柔軟に出力サイズ変更を扱わない場合があります）。
- **リアルタイム再生は不可**、レンダリング/書き出し用途を想定しています
  （CPU推論はもちろん、GPU推論であっても4Kフレームの超解像はフレームあたり
  数百ms〜数秒かかるため）。
- GPU推奨。この実装のCPUパスは動作確認用であり、実運用には
  CUDA/DirectML/CoreMLいずれかの実行プロバイダが有効なonnxruntimeビルドを
  推奨します。
- SmartFX（`PF_Cmd_SMART_PRE_RENDER` / `PF_Cmd_SMART_RENDER`）には未対応です。
  現状は旧来のバッファ拡張レンダリングパスのみ実装しています。

## ロードマップ

- SmartFX対応（`PF_Cmd_SMART_PRE_RENDER` / `PF_Cmd_SMART_RENDER`）によるRAM
  プレビュー・キャッシュとの統合改善
- 16bpc / 32bpc(float) 対応
- DirectML (Windows) / CoreML (macOS) 実行プロバイダの動作検証・既定有効化
- フレーム間の時間的一貫性（temporal consistency）を考慮した処理
  （現状はフレーム単位で独立に推論するため、動画では微小なちらつきが
  出る可能性があります）

## この環境（Adobe SDK無し・GPU無し）での検証について

このリポジトリの開発/CI環境ではAdobe SDKもGPUも利用できないため、
検証は以下の方針で行っています。

1. `src/core` + `src/cli`（Adobe非依存）はこの環境で実際にビルド・実行し、
   タイル処理を含めて動作確認しています。
2. `src/plugin`（Adobe SDK層）はこの環境ではコンパイルできないため、
   AE SDKの実際のAPIシグネチャに忠実に書く一方、コンパイルエラーの
   機械的な検証はできていません。Adobe SDK入手後、最初のビルドで
   ヘッダパスやAPI詳細の細かな調整が必要になる可能性があります。
