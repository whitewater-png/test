# デスクトップの子 — VRM Desktop Mascot × Claude Code

Blenderで作ったキャラクターをデスクトップに立たせて、話しかけると返事をして、
踊って、頼めば調べ物までしてくれる、というやつ。

- **表示** … three.js + [@pixiv/three-vrm](https://github.com/pixiv/three-vrm)。背景透過のオーバーレイウィンドウなので、壁紙の上に直接立ちます
- **踊り** … VRM Animation（`.vrma`）を `motions/` に入れるだけ。待機中は20〜40秒おきに勝手に踊り出します
- **頭脳** … **Claude Code CLI をヘッドレスで叩きます**。だから「これ調べて」で本当にWeb検索が走るし、プロジェクト内のファイルも読めます。CLIが無い環境では Anthropic Messages API に自動フォールバック
- **声** … スペース長押しで音声入力（Web Speech API）、返事は VOICEVOX があればそれ、無ければOSの読み上げ。口パクは音声波形から取ります

```
┌──────────── Electron ────────────┐
│  mascot window   stage window    │   どちらも同じ web/index.html
│  (透過・最前面)   (視点操作用)     │
└───────────┬──────────────────────┘
            │ HTTP + SSE
┌───────────▼──────────────────────┐
│  server/bridge.mjs               │
│   ├ /api/chat  → Claude Code CLI │  ← 無ければ Messages API
│   ├ /assets/motions → VRMA自動修復│
│   └ /assets/models  → .vrm       │
└──────────────────────────────────┘
```

## つかいかた

```bash
npm install                 # 依存を入れて web/vendor/ にESMをコピーします
npm run make-motions        # モーション（.vrma）を生成 — 手持ちが無くてもここで揃います
# models/ に .vrm を置く
npm start                   # Electron（マスコット＋ステージ）
```

ブラウザだけで見たいときは:

```bash
npm run serve               # → http://127.0.0.1:4747
```

| 操作 | |
| --- | --- |
| **スペース長押し** | 話しかける（離すと送信） |
| **ドラッグ** | 視点回転（ステージウィンドウのみ） |
| **入力欄にモーション名** | Claudeを経由せずそのまま踊ります |
| **⌘/Ctrl+Shift+M** | マスコットの表示・非表示 |

URLパラメータ:

| | |
| --- | --- |
| `?model=名前` | `models/` に複数置いたときの切り替え |
| `?fit=full` \| `upper` \| `head` | 全身 / 上半身 / 顔アップ（既定は全身） |
| `?autodance=off` | 勝手に踊るのを止める |

## キャラクターを用意する

**VRM 0.x / 1.0 どちらでも読めます。** 拡張子は `.vrm` でも `.glb` でも構いません
（VRMの実体はglbなので、書き出しツールによっては `.glb` で出てきます）。
`models/` に置くだけで、カメラはモデルのバウンディングボックスから自動で合わせます。

VRM 0.x の表情名は自動で読み替えられます:

| VRM 0.x | このアプリ |
| --- | --- |
| `joy` | `happy` |
| `sorrow` | `sad` |
| `fun` | `relaxed` |
| `a` | 口パク（`aa`） |
| `blink` | 瞬き |

VRM 0.x には `surprised` が無いので、そこだけは反応しません（表情が変わらないだけで、エラーにはなりません）。

Blender側の手順・書き出し前チェックは [`blender/README.md`](blender/README.md) にまとめてあります。
チェックスクリプトも置いてあります:

```bash
blender your-character.blend --background --python blender/vrm_export_check.py
```

> **ライセンスに注意。** `models/` と `motions/` は `.gitignore` 済みです。
> 再配布不可のモデルをうっかりコミットしないための保険なので、外さないでください。

## モーション

`motions/` に `.vrma` を置くだけです。ファイル名がそのままモーション名になり、
起動時にClaudeのシステムプロンプトへ一覧が渡されます。

```
motions/
  idle.vrma        ← この名前だけ特別。あればループ待機モーションになります
  dance.vrma
  wave.vrma
```

### 手持ちが無いとき

**`.vrma` をコードから生成できます。** 買う必要も、モーキャプも要りません。

```bash
npm run make-motions            # idle / wave / nod / shake / dance / spin を生成
npm run make-motions -- --list  # 一覧
npm run make-motions -- dance   # 個別に作り直す
```

中身は [`scripts/make-motions.mjs`](scripts/make-motions.mjs) の1ファイルで、
「正規化時間 0〜1 を受け取ってボーンの角度を返す関数」を書くだけで増やせます。

```js
dance: {
  duration: 4,
  loop: true,
  frame(t) {
    const pose = relaxedPose();          // Tポーズではなく自然に立った状態から
    add(pose, 'head', [0, 6 * sin(t, 2), 0]);
    return { pose, hips: [0, -0.04 * Math.abs(sin(t, 4)), 0] };
  },
},
```

回転軸の向きは実機レンダリングで測ってあり、スクリプト先頭にメモしてあります
（VRM 0.x はレターゲット時にY軸180°反転が入るので、ボーンを直接触った勘は当てになりません）。

| 関節 | 曲げる軸 |
| --- | --- |
| 肩を上げる | `leftUpperArm` +Z / `rightUpperArm` -Z |
| 肘を曲げる | `leftLowerArm` -Y / `rightLowerArm` +Y |
| 膝を曲げる | `lowerLeg` -X（左右とも） |

### 既製品が欲しいとき

- **[VRMアニメーション7種セット（.vrma）](https://booth.pm/ja/items/5512385)** … pixiv / VRoid Project が公式に**無料配布**しているもの。挨拶・Vサイン・回る・屈伸など7種
- **Mixamo → Blender → `.vrma`** … Mixamoの無料モーションをBlenderに読み込み、[VRM Add-on for Blender](https://vrm-addon-for-blender.info/en-us/ui/export_scene.vrma/) の `File → Export → VRM Animation (.vrma)` で書き出す

**販売されているVRMAには `specVersion` が抜けていて読み込めないものがあります。**
配信時にメモリ上で自動修復するので基本はそのまま置けますが、ファイルごと直したいときは:

```bash
npm run fix-vrma            # motions/ を全部修復（元ファイルは .bak で保存）
npm run fix-vrma -- --check # 壊れているか見るだけ
```

## 体の動かし方（タグプロトコル）

Claudeには「返事の最後にタグを付けて」と伝えてあります。タグは読み上げ文から除去されて、
そのままキャラクターの制御に使われます。

```
うん、それ調べてきたよ！ [[emotion:happy]] [[motion:pokedance]]
```

- `[[emotion:...]]` … `neutral` / `happy` / `angry` / `sad` / `relaxed` / `surprised`
- `[[motion:...]]` … `motions/` にあるファイル名。存在しない名前はサーバー側で捨てます

## 設定（環境変数）

| 変数 | 既定 | |
| --- | --- | --- |
| `MASCOT_PORT` | `4747` | ブリッジのポート |
| `MASCOT_BACKEND` | `auto` | `cli` / `api` で固定できます |
| `MASCOT_CLAUDE_BIN` | `claude` | Claude Code CLI のパス |
| `MASCOT_ALLOWED_TOOLS` | `WebSearch,WebFetch,Read,Glob,Grep` | CLIに許可するツール |
| `MASCOT_PROJECT_DIR` | このリポジトリ | Claude Code の作業ディレクトリ |
| `ANTHROPIC_API_KEY` | — | フォールバック用（`ant auth login` のプロファイルでも可） |

VOICEVOX を `http://127.0.0.1:50021` で起動しておくと、自動で検出してそちらの声で喋ります。
そのとき口パクは合成音声の波形から取るので、ちゃんと合います。

## 動作の前提

- Node.js 20+
- 音声入力は Chromium系（Web Speech API）。未対応環境では入力欄が代わりになります
- 透過ウィンドウは macOS / Windows で安定。Linuxはコンポジタ次第です
