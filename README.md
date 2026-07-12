# DesktopMate 🍡

デスクトップに住む小さなマスコット「モチ」と、AIでおしゃべりできるMac用アプリです。
[Desktop Mate](https://www.infiniteloop.co.jp/desktopmate) のように、キャラクターが常に画面の隅にいて、話しかけるとAI(Anthropic Claude / OpenAI GPT / Google Gemini)が返事をしてくれます。

```
        ┌──────────────────┐
        │  モチとおしゃべり      │
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
- 🖼️ **キャラクター画像の差し替え** — 好きな画像を選ぶとマスコットがその絵に変わる(白背景の自動透過つき)
- 😊 **表情アニメーション** — 待機中はぷかぷか＆まばたき、考え中は「…」、話し中は口が開く(図形マスコット時)
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

# または .app バンドルを作成
./scripts/build-app.sh
open dist/DesktopMate.app
```

初回起動時に設定ウィンドウが開きます。使いたいプロバイダーを選び、APIキーを入力して「保存」を押してください。
キーはmacOSのキーチェーンにプロバイダーごとに保存されます。環境変数(`ANTHROPIC_API_KEY` / `OPENAI_API_KEY` / `GEMINI_API_KEY`)でも可。

設定画面下部の「キャラクター画像を選択…」から好きな画像を選ぶと、マスコットがその絵に変わります。白背景の一枚絵は「白い背景を透過する」で自動的に切り抜かれます。

## 使い方

| 操作 | 動作 |
|---|---|
| キャラクターをクリック | チャット吹き出しを開く / 閉じる |
| キャラクターをドラッグ | 好きな位置に移動 |
| メニューバーの 😊 アイコン | 表示切替 / 設定 / 終了 |
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
- キャラクターは画像を設定していればその絵を、なければSwiftUIの図形を描画します
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

## 料金について

会話のたびにClaude APIを呼び出すため、Anthropicのアカウントに従量課金が発生します。
返答は短め(最大4096トークン)に設定してあり、日常的なおしゃべり程度なら少額で済みますが、
[料金ページ](https://www.anthropic.com/pricing)で単価を確認しておくことをおすすめします。

## 今後のアイデア

- 音声入力(マイク)・音声読み上げでの会話
- キャラクターの着せ替え / 複数キャラクター
- 時間帯に応じたひとりごと(自発的な発話)
- Live2D / VRMモデルの表示対応
