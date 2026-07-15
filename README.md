# DesktopMate 🍡

デスクトップに住む小さなマスコット「セナ」と、AIでおしゃべりできるMac用アプリです。
[Desktop Mate](https://www.infiniteloop.co.jp/desktopmate) のように、キャラクターが常に画面の隅にいて、話しかけるとAI(Anthropic Claude / OpenAI GPT / Google Gemini)が返事をしてくれます。

```
        ┌──────────────────┐
        │  セナとおしゃべり      │
        │ ┌──────────────┐ │
        │ │ こんにちは！     │ │
        │ │    やっほー！🍡 │ │
        │ └──────────────┘ │
        │ [メッセージを入力…] ➤ │
        └──────────────────┘
              ／⌒⌒⌒＼
             |  ●    ●  |     ← ぷかぷか浮いて、まばたきする
              ＼  ▽  ／
               ￣￣￣￣
```

## 特徴

- 🫧 **透明・最前面ウィンドウ** — キャラクターだけがデスクトップに浮かんで見える
- 🖱️ **クリックでおしゃべり** — キャラクターをクリックすると吹き出しチャットが開く
- ✋ **ドラッグで移動** — キャラクターを掴んで好きな場所に置ける
- 🤖 **マルチベンダー対応** — Anthropic (Claude) / OpenAI (GPT) / Google (Gemini) を設定で切替
- 💬 **ストリーミング応答** — どのプロバイダーでも返事がリアルタイムに表示される
- 🧑‍🎨 **既定は図形キャラ** — 何も設定しなくても、図形で描いたまんまるキャラが表示される
- 🖼️ **キャラクター画像の差し替え** — 好きな画像を選べばマスコットを変更できる(白背景の自動透過つき)
- 🧍 **VRM / GLB(3Dモデル)対応** — `.vrm` / `.glb` / `.gltf` を選ぶと3Dアバターとして表示(three.js + @pixiv/three-vrm)。VRM拡張があればまばたき等も動作
- 😊 **アニメーション** — ぷかぷか浮遊・まばたき・表情変化。VRMは待機中も自律的に動く(キョロキョロ・振り向き・膝を曲げたその場歩き)、発話に合わせた口パク/表情、そして**マウスカーソルを頭と体で追いかける**
- 🙋 **しぐさ(VRM)** — メニューバーの「しぐさ」から、手を振る / 座る / 立つ を指示できる
- 🔐 **APIキーはKeychain保存** — プロバイダーごとにメニューバーアイコンから設定
- 🖥️ **Dockに出ない常駐アプリ** — メニューバーから表示切替・終了

## 動作環境

- macOS 14 (Sonoma) 以降
- Swift 5.9 以降 (Xcode 15+ または Command Line Tools)
- いずれかのAIプロバイダーのAPIキー:
  - Anthropic Claude — [Anthropic Console](https://console.anthropic.com/settings/keys)
  - OpenAI GPT — [OpenAI Platform](https://platform.openai.com/api-keys)
  - Google Gemini — [Google AI Studio](https://aistudio.google.com/apikey)

## セットアップ & 起動

```bash
git clone <このリポジトリ>
cd test

# そのまま実行(開発用)
swift run

# または .app バンドルを作成(Apple Silicon / Intel 両対応・アイコン付き)
./scripts/build-app.sh
open dist/DesktopMate.app

# 配布用の .zip / .dmg までまとめて作る
./scripts/package.sh 1.0.0
```

初回起動時に設定ウィンドウが開きます。使いたいプロバイダーを選び、APIキーを入力して「保存」を押してください。
キーはmacOSのキーチェーンにプロバイダーごとに保存されます。環境変数(`ANTHROPIC_API_KEY` / `OPENAI_API_KEY` / `GEMINI_API_KEY`)でも可。

設定画面下部の「キャラクター画像を選択…」から好きな画像を選ぶと、マスコットがその絵に変わります。白背景の一枚絵は「白い背景を透過する」で自動的に切り抜かれます。

## 使い方

| 操作 | 動作 |
|---|---|
| キャラクターをクリック | チャット吹き出しを開く / 閉じる |
| キャラクターをドラッグ | 好きな位置に移動 |
| メニューバーの 😊 アイコン | 表示切替 / しぐさ / 設定 / 終了 |
| メニュー → しぐさ | VRMに 手を振る / 座る / 立つ をさせる |
| 設定 → プロバイダー | Anthropic / OpenAI / Google を切替 |
| 設定 → 画像を選択 | マスコットの絵を差し替え |
| チャットの 🗑 ボタン | 会話履歴をリセット |

## しくみ

```
┌─────────────────────────────────────────────┐
│ MascotWindow (NSWindow)                     │
│   borderless / 透明 / .floating / 全Space表示 │
│  ┌───────────────────────────────────────┐  │
│  │ ContentView (SwiftUI)                 │  │
│  │  ├─ ChatBubbleView … 吹き出しチャットUI │  │
│  │  └─ MascotView    … キャラクター描画    │  │
│  └───────────────────────────────────────┘  │
└─────────────────────────────────────────────┘
          │ ChatViewModel
          ▼
   ChatBackend (protocol)
     ├─ AnthropicBackend … api.anthropic.com/v1/messages
     ├─ OpenAIBackend    … api.openai.com/v1/chat/completions
     └─ GoogleBackend    … generativelanguage.googleapis.com (Gemini)
                            いずれも stream / SSE で受信
```

- プロバイダーは `ChatBackend` プロトコルで抽象化し、選択に応じて実装を切り替えます
- キャラクターは「VRM(3Dモデル)→ ユーザー選択画像 → 図形描画」の順に表示を切り替えます(既定は図形描画)
- VRMは透明な `WKWebView` 上で three.js + @pixiv/three-vrm により描画します(`Resources/viewer.html`)。3D描画ライブラリは実行時にCDN(esm.sh)から読み込むため、VRM表示時はネット接続が必要です
- 会話履歴はアプリ内に保持し、毎リクエストで全履歴を送信します(APIはステートレス)
- 応答はServer-Sent Eventsでストリーミング受信し、少しずつ吹き出しに表示します

## カスタマイズ

| 変えたいもの | 場所 |
|---|---|
| キャラクターの性格・口調 | `Sources/DesktopMate/ChatBackends.swift` の `AssistantPersona.systemPrompt` |
| プロバイダー/モデルの追加 | `Sources/DesktopMate/LLMProvider.swift` と `ChatBackends.swift` |
| 既定モデル | `LLMProvider.defaultModel` (アプリの設定画面でも変更可) |
| キャラクターの見た目(図形) | `Sources/DesktopMate/MascotView.swift` |
| 初期位置・ウィンドウサイズ | `Sources/DesktopMate/MascotWindow.swift` |

## 配布する

他の人に配りたいときは [`DISTRIBUTION.md`](DISTRIBUTION.md) を参照してください。要点だけ:

```bash
./scripts/package.sh 1.0.0     # dist/ に .dmg と .zip ができる
```

- Apple Silicon / Intel 両対応のユニバーサルバイナリで書き出します。
- **未署名配布**を前提にしています。受け取った人は初回だけ **右クリック →「開く」** で起動します（詳細は DISTRIBUTION.md）。警告を完全になくすには Apple Developer Program（年 $99）での署名＋公証が必要です。
- 配布物には `LICENSE` と `THIRD_PARTY_LICENSES.md` を同梱してください。
- ⚠️ 他者が作った画像や VRM を同梱して再配布する場合は、その作者の許諾条件を必ず確認してください（本体には既定画像を同梱していません）。

## ライセンス

本体は MIT ライセンス（[`LICENSE`](LICENSE)）です。VRM/GLB 描画に使う three.js / @pixiv/three-vrm（いずれも MIT）のクレジットは [`THIRD_PARTY_LICENSES.md`](THIRD_PARTY_LICENSES.md) にまとめています。

## 料金について

会話のたびにClaude APIを呼び出すため、Anthropicのアカウントに従量課金が発生します。
返答は短め(最大4096トークン)に設定してあり、日常的なおしゃべり程度なら少額で済みますが、
[料金ページ](https://www.anthropic.com/pricing)で単価を確認しておくことをおすすめします。

## 今後のアイデア

- 音声入力(マイク)・音声読み上げでの会話
- キャラクターの着せ替え / 複数キャラクター
- 時間帯に応じたひとりごと(自発的な発話)
- Live2Dモデルの表示対応(VRMは対応済み)
- デスクトップ上を実際に歩き回る(ウィンドウ自体の移動。現在はその場歩き)
