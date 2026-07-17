# AI Upscale プラグイン開発ガイド

## クイックスタート（ワンショットセットアップ、macOS / Apple Silicon）

MacBook Pro M4 Max等のApple Silicon Mac上であれば、以下の1コマンドで
依存インストール〜ビルド〜モデル取得〜Premiere Proへのインストール〜
動作確認までを一括実行できます。

```bash
bash plugin/setup_mac.sh
```

- 冪等です。何度実行しても、既に完了している工程は自動的にスキップされます。
- 唯一の手動ステップは **Adobe After Effects SDKの初回ダウンロード**です
  （Adobeのライセンス上、本リポジトリでは再配布できないため）。SDKが
  見つからない場合、スクリプトが https://developer.adobe.com/after-effects/
  を開いて案内し、`~/AdobeSDK` に展開後の再実行を促します。
- Homebrew自体が未インストールの場合も、公式インストールコマンドを表示して
  中断します（任意のインストールスクリプトを自動実行するのはセキュリティ上
  避けているため、手動実行が必要です）。
- `bash plugin/setup_mac.sh --uninstall` でPremiere/AEのMediaCoreフォルダから
  プラグインを削除できます。
- `bash plugin/setup_mac.sh --help` でオプション一覧を表示します。

詳しい各工程の説明・トラブルシューティングは以下の各節を参照してください。

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

# 並列タイル処理 (--jobs) の指定例。0 (既定) は自動
# (hardware_concurrency)、1 は直列実行。--tile を省略すると
# choose_tile_size() が実行プロバイダに応じて自動選択します。
./plugin/build/upscale_cli plugin/build/test_model_4x.onnx /tmp/in.png /tmp/out_par.png \
  --tile 128 --jobs 4
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

macOS版はバンドル (`AIUpscale.plugin`) として出力されます。PiPLリソース
(`AIUpscalePiPL.r`) は `plugin/CMakeLists.txt` が自動でコンパイルします
（SDK付属の `Examples/Skeleton` Xcodeプロジェクトの「Compile PiPL」ビルド
フェーズと同じ2段階を、`cc -E -P` によるプリプロセスと `Rez`
（`xcrun -f Rez` で解決、Xcode Command Line Toolsに含まれる）による
コンパイルとして、`AIUpscale`ターゲットのビルド後処理に組み込んでいます）。
生成物は `AIUpscale.plugin/Contents/Resources/AIUpscale.rsrc` に配置され、
これが無いとバンドル自体は正常にビルド・インストールされても
Premiere Pro/After Effectsのエフェクト一覧に「AI Upscale」が表示されません
（実機で確認された症状）。`Rez` が見つからない場合、cmake configure時に
FATAL_ERRORで中断し対処法を表示します。`src/plugin/Info.plist.in` は
バンドルの `Info.plist` テンプレートです。

### macOS (Apple Silicon / M4 Max) 向け推奨設定

このプラグインは MacBook Pro M4 Max (36GB ユニファイドメモリ) を主要ターゲットとして
最適化されています。実機でビルド・実行する際は以下を推奨します。

- **onnxruntime**: 公式の `onnxruntime-osx-arm64-<version>.tgz`（CoreML EP同梱版）を
  使用してください。x86_64版やuniversal2版のRosetta経由実行は避けてください
  （ネイティブarm64の方が明確に高速です）。
- **CMakeアーキテクチャ**: `CMakeLists.txt` はmacOS上で `CMAKE_OSX_ARCHITECTURES=arm64`
  をデフォルトで設定します（明示指定は不要）。x86_64スライスを含むユニバーサル
  バイナリが必要な場合のみ `-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"` を上書きしてください
  （ただしx86_64側はCoreML EPを使わずCPU実行のみになります）。
- **実行プロバイダ**: `UPSCALE_WITH_COREML` はmacOSでデフォルトON（CMakeLists.txt参照）。
  `OnnxUpscaler::load()` はCoreML EPを `ModelFormat=MLProgram` /
  `MLComputeUnits=ALL` で優先的に試行し（ANE/GPU/CPUをCoreML自身にスケジューリング
  させる設定）、失敗時は自動的にCPU実行にフォールダックしてログとホストメッセージで
  通知します（詳細は本ファイル「安定運用ガイド」参照）。
- **タイルサイズ**: `TileOptions::tile_size` を `0`（自動）のままにしておくと、
  `choose_tile_size()` がCoreML有効時は既定512px、CPU実行時は既定256pxを目安に、
  36GBユニファイドメモリを前提とした見積もりメモリ予算内で自動選択します。
  4K以上の映像を頻繁に扱う場合、CoreML EPが安定して使えることを確認した上で
  タイルサイズを明示的に大きくする（例: `--tile 512` on CLI）ことも可能です。
- **並列度**: `--jobs`（CLI）/ `TileOptions::num_workers` は `0`（自動 =
  `std::thread::hardware_concurrency()`、M4 Max なら性能コア+効率コア合計の
  論理コア数）が既定です。CPU実行プロバイダ時はタイル単位でONNX Runtimeの
  `Run()` を並列実行し（`Ort::Session::Run()` は同一セッションに対して
  スレッドセーフと公式に規定されているため、セッションを共有して並列呼び出し
  可能）、CoreML実行プロバイダ時は推論呼び出し自体を直列化しつつ、前処理
  （NCHW変換）・後処理（アルファ合成・クランプ・書き戻し）のみ並列化します
  （`src/core/onnx_upscaler.cpp` の `infer_mutex_` 周辺コメント参照）。

### Linux

Linux上ではAdobe SDKが提供されていないため `AIUpscale` ターゲットは
自動的にスキップされます（`AE_SDK_PATH` を指定してもメッセージを出して
スキップします）。`upscale_core` / `upscale_cli` は通常通りビルド・テスト可能です。

## モデルの入手

```bash
python3 plugin/scripts/download_models.py --out-dir plugin/models
```

`RealESRGAN_x4plus.pth`（Photo用）と `RealESRGAN_x4plus_anime_6B.pth`
（Anime用）の公式PyTorch重みを `plugin/models/` にダウンロードします。

Photo用・Anime用のいずれも、事前エクスポートされたONNXファイルは配布せず、
両方ともこの公式PyTorch重みからローカルでONNXへ変換する方式に統一して
います（`plugin/scripts/export_realesrgan_onnx.py`、高さ・幅を動的軸として
エクスポートするため、タイルサイズに関わらず推論可能です）。

> 以前のバージョンではPhoto用に Hugging Face の
> `qualcomm/Real-ESRGAN-x4plus` （Qualcomm NPU向けにエクスポートされた
> ONNX）を直接ダウンロードしていましたが、このONNXは入力形状が
> `1x3x128x128` に固定されており、128x128以外のタイルを流すと
> `Got invalid dimensions for input: image index: 2 Got: 8 Expected: 128`
> のような推論エラーで失敗することが実機検証で判明しました。本プラグインの
> タイル分割推論パイプラインは任意サイズのタイルを流すため、この固定形状
> ONNXとは非互換です。そのため、Anime用と同じ「公式PyTorch重み→ローカルで
> 動的サイズ対応ONNXに変換」方式に統一しています。
> 既に `plugin/models/realesrgan-x4plus.onnx` が残っている環境で
> `plugin/setup_mac.sh` を実行すると、対応する `RealESRGAN_x4plus.pth` が
> 無い場合はこの旧ONNXを自動的に `.bak.<タイムスタンプ>` へ退避し、
> `.pth` を再取得・再変換します（ステップ5、`step5a_quarantine_stale_photo_onnx`）。

`plugin/scripts/export_realesrgan_onnx.py` は、チェックポイントの
state_dictキー（`body.<N>....`）からRRDBブロック数を自動推定するため、
Photo用（23ブロック）・Anime用（6ブロック）のどちらにも同じスクリプトを
使えます（`--num-block` での明示指定も可能）。

### モデル取得先が利用できない場合

- Photo用 (`RealESRGAN_x4plus.pth`): 本スクリプトは xinntao/Real-ESRGAN の
  公式GitHubリリース (`v0.1.0`) から取得します。このURLも将来利用できなく
  なった場合は https://github.com/xinntao/Real-ESRGAN/releases で最新の
  リリースを確認するか、下記の手動エクスポート手順を使ってください。
- Anime用 (`RealESRGAN_x4plus_anime_6B.pth`): 執筆時点で維持されているONNX
  直配布が見当たらなかったため、本スクリプトは `amd/realesrgan-x4plus-anime-6b`
  の `.pth`（PyTorch重み、xinntao/Real-ESRGAN由来）をダウンロードします。

いずれも `plugin/setup_mac.sh` を使う場合、`.pth` からのONNX変換は自動で
行われます（ステップ5。専用venv `plugin/build/torch-venv` を作成し、
`plugin/scripts/export_realesrgan_onnx.py` で変換します）。手動で行いたい
場合は以下のコマンドで実行できます:

```bash
pip3 install torch onnx

# Photo用 (num_block=23はチェックポイントから自動推定されます)
python3 plugin/scripts/export_realesrgan_onnx.py \
    plugin/models/RealESRGAN_x4plus.pth \
    plugin/models/realesrgan-x4plus.onnx

# Anime用 (num_block=6も同様に自動推定されます)
python3 plugin/scripts/export_realesrgan_onnx.py \
    plugin/models/RealESRGAN_x4plus_anime_6B.pth \
    plugin/models/realesrgan-x4plus-anime.onnx
```

  `export_realesrgan_onnx.py` は `basicsr` に依存しません
  （`basicsr` は新しいtorchvisionで壊れている既知の問題があるため —
  詳細はスクリプト先頭のコメント参照）。RRDBNetアーキテクチャをtorchのみで
  インライン定義し、`.pth` の読み込みには
  `torch.load(path, map_location="cpu", weights_only=True)` を使用します。
  `.pth` はpickle形式であり、`weights_only=True` を指定しない `torch.load()`
  は任意コード実行につながり得るため、このスクリプトは常に
  `weights_only=True` でロードし、テンソル以外のオブジェクトが混入して
  いた場合は（安全側に倒して）ロードごと失敗させます。

### インストール先

| 編集ソフト | プラグインの配置場所 |
|---|---|
| After Effects | `Adobe After Effects <version>/Support Files/Plug-ins/` |
| Premiere Pro | 共通プラグインフォルダ（MediaCore）: Windows `C:\Program Files\Common Files\Adobe\Plug-ins\Common\`、macOS `/Library/Application Support/Adobe/Common/Plug-ins/<version>/MediaCore/` |

`models/` ディレクトリはプラグイン本体（`.aex` / `.plugin`）と同じ場所に
配置してください（`AIUpscale.cpp` の `resolve_plugin_directory()` が
プラグインディレクトリ配下の `models/` を参照します）。

## 安定運用ガイド

このプラグイン/CLIで問題が起きた場合の切り分け手順です。

### ログの場所

構造化ログ（`src/core/logger.h`）は以下に出力されます。1行ごとに
`<タイムスタンプ> [INFO|WARN|ERROR] [tid=<スレッドID>] <メッセージ>` の形式です。
5MBを超えると `AIUpscale.log.old` にローテーションされます（既存の`.old`は上書き）。

| プラットフォーム | ログパス |
|---|---|
| macOS | `~/Library/Logs/AIUpscale/AIUpscale.log` |
| Windows | `%TEMP%\AIUpscale\AIUpscale.log` |
| Linux / その他 (CLI開発用) | `$TMPDIR/AIUpscale/AIUpscale.log`（`$TMPDIR`未設定時は`/tmp/AIUpscale/AIUpscale.log`） |

### エラー発生時の切り分け手順

1. **ホスト側のエラーメッセージを確認**: After Effects/Premiereはエフェクト
   ダイアログや警告バナーに `out_data->return_msg`（例:
   「AI Upscale: failed to load model (...)」）を表示します。CLIは同内容を
   stderrに出力し、非0で終了します。
2. **ログファイルで詳細を確認**: 上記のログパスを開き、直近の `[ERROR]` 行を
   確認してください。例外内容・入力サイズ・実行プロバイダ・タイル設定が
   記録されています。
3. **CPUフォールバックの確認**: ログに
   `"falling back to CPUExecutionProvider"` がある場合、CoreML初期化に
   失敗しCPU実行になっています（動作はしますが大幅に遅くなります）。
   onnxruntimeがCoreML EP同梱のmacOS(arm64)ビルドか、`UPSCALE_WITH_COREML`
   が有効か([CMake出力](#macos-apple-silicon--m4-max-向け推奨設定)参照)を確認してください。
4. **メモリ不足（OOM）の確認**: ログに
   `"out of memory at tile_size=..."` がある場合、自動的にタイルサイズを
   半分にして1回だけ再試行します。それでも失敗する場合はさらに小さい
   `--tile` を明示指定するか、他のメモリ使用量の多いアプリを閉じてください。
5. **異常サイズ入力の確認**: ログに `"exceeds maximum supported dimension"`
   や `"exceeds safety limit"` がある場合、入力フレームが安全上限
   （既定: 一辺8192px、出力2億ピクセル、単一バッファ4GiB）を超えています。
   意図的なUHD/8K以上のワークロードであれば `src/core/size_limits.h` の
   定数を見直してください。

### 推奨設定（まとめ）

- macOS/M4 Max: CoreML EP（既定でON）+ タイルサイズ自動選択（既定512px）+
  `--jobs 0`（自動並列）。
- 汎用/低メモリ機: `--tile 128`〜`256` を明示指定し、`--jobs` は物理コア数
  以下に抑える。
- 常時: `plugin/models/` 配下のモデルファイルの出所を確認する
  （次節「セキュリティ」参照）。

## セキュリティ

- **モデルパスの検証**: AE/Premiereプラグイン層 (`AIUpscale.cpp` の
  `model_path_for()`) は、モデルを必ず `<プラグインディレクトリ>/models/`
  配下からのみロードします。`std::filesystem::canonical()` でシンボリック
  リンクを解決した上で、解決後のパスが `models/` ディレクトリの子孫である
  ことを文字列プレフィックス比較で検証し、範囲外であれば空文字列を返して
  ロードを拒否します（パストラバーサル・シンボリックリンク経由の
  ディレクトリエスケープ対策）。
- **`download_models.py`**: HTTPS以外のURLはリクエスト前に拒否します
  (`require_https()`)。ダウンロード後はSHA-256チェックサムを検証します
  （既知ハッシュは `KNOWN_SHA256` 定数に保持）。ハッシュが未登録のURLは
  「検証をスキップした」旨を明示的な警告として出力します（サイレントに
  信頼済み扱いはしません）。ダウンロードは `.part` 一時ファイルへ書き込み、
  完了後にのみ最終ファイル名へアトミックにリネームするため、失敗/中断時に
  部分ダウンロードが最終パスに残ることはありません。
- **画像デコードの検証**: `upscale_cli` は `STBI_MAX_DIMENSIONS` で
  stb_image自体のデコード上限を制限した上、デコード後も
  `validate_input_dims()`（`src/core/size_limits.h`）で寸法・チャンネル数を
  再検証します。
- **整数オーバーフロー対策**: 幅・高さ・チャンネル数からのバッファサイズ
  計算はすべて `size_limits.h` の `checked_mul_i64()` / `safe_buffer_bytes()`
  経由で行われ、乗算前にオーバーフローと安全上限（既定: 出力2億ピクセル、
  単一バッファ4GiB）を必ずチェックします。
- **プラグインAPI境界での例外捕捉**: `EffectMain`（`AIUpscale.cpp`）は
  すべてのコマンドディスパッチを try/catch で包み、いかなる例外もホストへ
  伝播させません（未捕捉の例外がC言語リンケージ境界を越えるとホスト
  クラッシュにつながるため）。各 `Handle*()` 関数内でもより詳細な
  `PF_Err` / `return_msg` を設定した上でエラーを捕捉しています。
- **ログへの機微情報の非記載**: ログにはモデル/入力ファイルのパス、画像
  寸法、実行プロバイダ、タイル/スレッド設定、例外メッセージのみを記録し、
  環境変数・認証情報・ユーザー名単体・画像/モデルの中身は記録しません
  （`src/core/logger.h` 冒頭のコメント参照）。

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
  （CPU推論はもちろん、GPU/ANE推論であっても4Kフレームの超解像はフレームあたり
  数百ms〜数秒かかるため）。
- **CoreML EP (macOS) はこの環境では実機未検証です。** `UPSCALE_WITH_COREML`
  はコンパイル時に有効化され、`AppendExecutionProvider("CoreML", ...)` の
  呼び出しコード自体はこのLinux環境でもコンパイル対象になりますが
  （`#ifdef __APPLE__` の外側のロジックはビルドされます）、実際にCoreML EPが
  ロードされ推論が成功するかはM4 Max実機での検証が必要です。失敗時は
  自動的にCPUへフォールバックする設計です（安定運用ガイド参照）。
- SmartFX（`PF_Cmd_SMART_PRE_RENDER` / `PF_Cmd_SMART_RENDER`）には未対応です。
  現状は旧来のバッファ拡張レンダリングパスのみ実装しています。

## ロードマップ

- SmartFX対応（`PF_Cmd_SMART_PRE_RENDER` / `PF_Cmd_SMART_RENDER`）によるRAM
  プレビュー・キャッシュとの統合改善
- 16bpc / 32bpc(float) 対応
- DirectML (Windows) 実行プロバイダの動作検証・既定有効化（CoreMLはmacOSで
  既定有効化済み。実機検証は引き続き必要 -- 上記「既知の制約」参照）
- フレーム間の時間的一貫性（temporal consistency）を考慮した処理
  （現状はフレーム単位で独立に推論するため、動画では微小なちらつきが
  出る可能性があります）

## この環境（Adobe SDK無し・GPU無し）での検証について

このリポジトリの開発/CI環境ではAdobe SDKもGPUも利用できないため、
検証は以下の方針で行っています。

1. `src/core` + `src/cli`（Adobe非依存）はこの環境で実際にビルド・実行し、
   タイル処理を含めて動作確認しています。
2. `setup_mac.sh`（macOSワンショットセットアップスクリプト）は、
   構文チェック (`bash -n`) 、shellcheck、`--check-only` / `--help` の
   実行、およびSDK探索ロジック（`find_ae_sdk()`）をモックの `$HOME` で
   関数単位に切り出して検証しています。ただしHomebrewでの実インストール、
   `open` コマンドでのブラウザ起動、`sudo` を使った実際のファイルコピー、
   実機Xcode/cmakeビルドはこの環境では検証できないため、**M4 Max実機での
   確認が必要**です（安全側に倒すため、これらの操作は自動テストでは
   モック関数に置き換えて呼び出しの発生のみを確認しています）。
3. `src/plugin`（Adobe SDK層）はこの環境ではコンパイルできないため、
   AE SDKの実際のAPIシグネチャに忠実に書く一方、コンパイルエラーの
   機械的な検証はできていません。Adobe SDK入手後、最初のビルドで
   ヘッダパスやAPI詳細の細かな調整が必要になる可能性があります。CoreML EP
   自体（`AppendExecutionProvider("CoreML", ...)` の実際の初期化成否）も
   同様にM4 Max実機での検証が必要です。

### タイル並列化の検証結果（このLinux/CPU環境、4コア）

`upscale_tiled()`（`src/core/tile.cpp`）の並列化について、テスト用モデル
（`make_test_model.py` が生成する軽量Resizeモデル）を使い、直列
(`--jobs 1`) と並列 (`--jobs N`) の出力一致・所要時間を確認しました。

| 入力サイズ | tile | jobs | Elapsed (upscale部分) | 出力バイト完全一致 |
|---|---|---|---|---|
| 1024x1024 | 128 | 1 | 2.360s | 基準 |
| 1024x1024 | 128 | 4 | 2.466s | `cmp` で一致 |
| 1024x1024 | 128 | 8 | 2.094s | `cmp` で一致 |
| 2048x2048 | 128 | 1 | 11.286s | 基準 |
| 2048x2048 | 128 | 4 | 3.311s (**約3.4倍高速**) | `cmp` で一致 |

出力は `cmp`（バイト完全一致）で検証しており、これは意図的な設計です
（`upscale_tiled()` はタイルの推論・前処理・後処理を並列実行しつつ、
出力バッファへの合成（ブレンド）は常に固定順序で単一スレッド実行するため、
`num_workers` の値によらず出力がビット完全に一致します -- `tile.h` / `tile.cpp`
のコメント参照）。1024x1024/tile=128のような画像1枚あたりのタイル数が
少ないケースではスレッド起動オーバーヘッドが並列化の利得を相殺気味ですが、
2048x2048（タイル数361個）のように並列化できる作業量が十分にある場合は
明確な高速化が確認できました。M4 Max実機（CoreML EP、より多いコア数）では
さらに大きな高速化が期待されます。
