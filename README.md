# DesktopMate 🍡

デスクトップに住む小さなマスコット「モチ」と、AIでおしゃべりできるMac用アプリです。
[Desktop Mate](https://www.infiniteloop.co.jp/desktopmate) のように、キャラクターが常に画面の隅にいて、話しかけるとClaude API経由で返事をしてくれます。

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
- 💬 **AI応答 (Claude API)** — ストリーミングで返事がリアルタイムに表示される
- 😊 **表情アニメーション** — 待機中はぷかぷか＆まばたき、考え中は「…」、話し中は口が開く
- 🔐 **APIキーはKeychain保存** — メニューバーアイコンから設定
- 🖥️ **Dockに出ない常駐アプリ** — メニューバーから表示切替・終了

## 動作環境

- macOS 14 (Sonoma) 以降
- Swift 5.9 以降 (Xcode 15+ または Command Line Tools)
- Anthropic APIキー ([Anthropic Console](https://console.anthropic.com/settings/keys) で発行)

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

初回起動時に設定ウィンドウが開くので、Anthropic APIキーを入力して「保存」を押してください。
キーはmacOSのキーチェーンに保存されます(環境変数 `ANTHROPIC_API_KEY` でも可)。

## 使い方

| 操作 | 動作 |
|---|---|
| キャラクターをクリック | チャット吹き出しを開く / 閉じる |
| キャラクターをドラッグ | 好きな位置に移動 |
| メニューバーの 😊 アイコン | 表示切替 / 設定 / 終了 |
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
   ClaudeClient ── POST https://api.anthropic.com/v1/messages
                   (model: claude-opus-4-8, stream: true / SSE)
```

- キャラクターは画像アセットを使わず、SwiftUIの図形だけで描画しています
- 会話履歴はアプリ内に保持し、毎リクエストで全履歴を送信します(Messages APIはステートレス)
- 応答はServer-Sent Eventsでストリーミング受信し、1トークンずつ吹き出しに表示します

## カスタマイズ

| 変えたいもの | 場所 |
|---|---|
| キャラクターの性格・口調 | `Sources/DesktopMate/ClaudeClient.swift` の `systemPrompt` |
| 使用モデル | 同ファイルの `model` (既定: `claude-opus-4-8`) |
| キャラクターの見た目 | `Sources/DesktopMate/MascotView.swift` |
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
