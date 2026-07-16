# 引き継ぎ書 (開発セッション向け)

このファイルは開発セッションの引き継ぎ用メモです。新しいセッションで作業を再開するとき、
最初にこれを読んでください。ユーザー向けの説明は README.md / DISTRIBUTION.md にあります。

最終更新: 2026-07-16 (座り腕/手首/胸の再調整コミット時点)

## 1. プロジェクト概要

Mac用デスクトップマスコット「DesktopMate」。透明な最前面ウィンドウにキャラクター(セナ)が浮かび、
クリックで吹き出しチャット、AI(Anthropic/OpenAI/Google)がストリーミング応答する。
Swift Package Manager 製 (SwiftUI + AppKit、macOS 14+、ユーザー環境は macOS Tahoe 26)。

- キャラ表示の優先順: VRM/GLB (WKWebView + three.js) → ユーザー画像 → 図形描画(既定)
- ブランチ: `claude/desktop-ai-character-mac-513xfn` にプッシュする(他ブランチ禁止)
- この開発環境(Linuxコンテナ)では Swift はコンパイルできない。Swift変更はユーザーの実機ビルド頼み

## 2. 構成ファイル(重要なもの)

| ファイル | 役割 |
|---|---|
| `web/glue.js` | **VRMアニメーションの本体**(three.js + @pixiv/three-vrm)。編集後は必ず再バンドル |
| `Sources/DesktopMate/Resources/bundle.js` | glue.js のesbuild出力(コミット対象)。CDN依存なし |
| `Sources/DesktopMate/Resources/viewer.html` | WKWebViewに配信するHTML。bundle.jsとモデルbase64をSwiftが差し込む |
| `Sources/DesktopMate/VRMView.swift` | WKWebView + カスタムスキーム配信。mood/カーソル/しぐさをJSへ送る |
| `Sources/DesktopMate/AppDelegate.swift` | メニューバー(表示切替/しぐさ/設定/終了) |
| `Sources/DesktopMate/ChatBackends.swift` | ペルソナ(セナ=リノア風)と3プロバイダのSSE実装 |
| `scripts/rebuild.sh` | ユーザーがビルド反映に使う(`git pull` 後に実行してもらう) |
| `scripts/build-app.sh` / `package.sh` | .app生成(既定=単一アーキ)/配布物(.zip/.dmg、ユニバーサル) |

## 3. glue.js の再バンドル手順(必須)

```sh
cd web
# node_modules が無ければ: npm install --no-save three@0.160.0 @pixiv/three-vrm@3.5.5 esbuild
node --check glue.js
npx esbuild glue.js --bundle --format=iife --minify --platform=browser \
  --outfile=../Sources/DesktopMate/Resources/bundle.js
```
`web/glue.js` と `bundle.js` は**必ずペアでコミット**する。

## 4. ヘッドレス検証ハーネス(この環境でVRMを実描画する方法)

Linux上でWKWebViewは動かないが、Playwright + Chromium で bundle.js を実描画検証できる。

```sh
SCRATCH=<スクラッチディレクトリ>
# ハーネス生成: viewer.html に bundle.js とモデルbase64を差し込む
node <<'NODE'
const fs=require('fs'),path=require('path');
const root='/home/user/test', S=process.env.SCRATCH;
let base=fs.readFileSync(path.join(root,'Sources/DesktopMate/Resources/viewer.html'),'utf8');
const bundle=fs.readFileSync(path.join(root,'Sources/DesktopMate/Resources/bundle.js'),'utf8');
const b64=fs.readFileSync('<VRM/GLBファイル>').toString('base64');
base=base.replace('//__VRM_BUNDLE__',()=>bundle).replace('__MODEL_B64_TOKEN__',()=>b64);
const probe=`<script>window.__READY__=false;window.__onReady__=function(){window.__READY__=true;};</script>`;
fs.writeFileSync(path.join(S,'harness_move.html'), base.replace('</head>',probe+'</head>'));
NODE
```
- Playwright: import `/opt/node22/lib/node_modules/playwright/index.mjs`、
  executablePath `/opt/pw-browsers/chromium-1194/chrome-linux/chrome`、
  args `['--no-sandbox','--enable-unsafe-swiftshader','--use-gl=angle','--use-angle=swiftshader']`
- `window.__READY__===true` を waitForFunction で待ってから操作する
- テスト用モデル: `/root/.claude/uploads/<セッションID>/*.glb`(新しいコンテナでは毎回消えている前提で、
  検証開始前にユーザーへ再添付を依頼する)
- **教訓**: 手のひらの向きなど細部は全身スクショでは誤判定する。screenshot の clip で
  **部位ズーム**して判定する(親指の位置=内側なら手のひら下、が決め手)
- **重要な罠**: `__gesture__('sit')` 後に何もしないと、待機の自律行動(`pickIdleBehavior`)が
  数秒でランダムに発火し、`bodyYaw`(振り向き)や `walkAmt`(足踏み)が変化して
  `__armGeo__()`/`__legGeo__()` のワールド座標が**同じパラメータでも実行ごとにブレる**。
  必ず `__debugForce__('idle', 9999)` + `__legLock__=0` + `__setYaw__(0)` を
  **ロード直後・シグネチャ計測の直前の両方**で呼んでから数値を読むこと
  (呼び忘れると knee の X 座標が 0.04→0.08 のように無関係な値へドリフトして見える)

### JSデバッグAPI(glue.js内、アプリ未使用・テスト専用)
- `__setState__({mood,talking})` … チャット状態(Swiftも使用)
- `__setPointer__({x,y})` … カーソル追従(-1..1、Swiftも使用)
- `__gesture__('sit'|'stand'|'wave')` … しぐさ(Swiftも使用)
- `__debugForce__(behavior, dur)` … 自律行動の固定('idle'で乱数行動を止める)
- `__setYaw__(rad)` … 体の向き固定(側面=Math.PI/2)
- `__legLock__=位相` … 歩行位相の固定 / `__footInfo__()` `__armGeo__()` … ワールド座標実測
- `__sitp__={...}` … 座りポーズ全パラメータの上書き(sweep用)

## 5. 実測で確定した符号の知識(重要・再発見に時間がかかる)

このVRMモデル(正規化ボーン)で経験的に確定:
- `upperLeg.rotation.x`: **正=前** / 膝(lowerLeg)は歩行で **負方向がふくらはぎ後畳み**(正だと逆膝)
- カーソル追従: `LOOK_SIGN=1`、縦は `pointer.y` 正で見上げ
- 腕: `baseArmZ=72°` で下ろす(直立/歩行時のみ。座り完了時は `saZ` に乗り換わる)
- 座り腕の各軸の実測挙動(`__armGeo__()`でhand-knee差分を見ながら確定。詳細は§5.1):
  - `armX`(肩の屈曲): 増やすと前方(Z)へ伸びるが、同時にY(高さ)も強く持ち上がる副作用がある
  - `armY`(肩の内転): X(左右)にはほぼ効かない。数値を大きく上げても弱い効果しかない
  - `armZ`(上腕Z): 実はX(左右)の主要な制御軸。1.5→1.8で手が大きく内側へ寄る
  - `loArm`(前腕X): **完全に無効**。このモデルでは前腕のロール(捻り)軸で、手の位置を一切動かさない
  - `loArmY`(前腕Y): X(左右)を強く動かす実用的なレバー。armZより効きが良く、位置調整の主力
  - 前腕Z(`loArmZ`という実験パラメータを一時追加して検証)は armX と同種の動き(前後+高さ)で
    独立した自由度にならず、**コードから削除済み**(このHANDOVER更新時点では存在しない)
- 座り手首・座り腕の確定値(2026-07-16、実描画+ズーム画像+ワールド方向実測で確認済み):
  `armX=0.85, loArmY=0.55`(腕は太ももに届く位置) + `handX=-2.406, handY=-0.296, handZ=-0.654`
  (手首は「反復フィット」= 適用→指/親指のワールド方向を実測→誤差回転で補正、で決定。
  実測: 指=ほぼ水平前方、親指=内側やや上 y=+0.32)。
  旧値の教訓: `handX=1.54/2.2, handY=0.50, handZ=1.31` 系は親指が下/外向きになりNG。
  また**このモデルは親指ボーンが指方向から約40°しか開かない**ため、指を下に向けたまま
  親指を上げるのは幾何的に不可能(指を水平にして両立させる)
- 胸(chest)は座り時に前後傾パラメータが元々存在しなかった。新規追加: `chestX`(既定 `-0.12`、
  胸を後ろへ開いて背筋を伸ばす方向)。追加前は `chest.rotation.y` のみでX軸は常に0固定だった
- 検証の鉄則: **憶測で符号を反転しない。ワールド座標の実測かズーム画像で確定してから直す**
  (§4の「重要な罠」= idle行動ロック も参照。ロックし忘れると上記の実測がすべて無意味になる)

### 5.1 座り手・腕パラメータ早見表(`__sitp__`で上書き可、単位rad)

| パラメータ | 既定値 | 効果 |
|---|---|---|
| `armX` | 0.85 | 肩の前屈(前方Z reach。上げるとYも一緒に持ち上がる) |
| `armY` | 0.15 | 肩の内転(X左右への効果は弱い) |
| `armZ` | 1.5 | 上腕Z(X左右の主レバー) |
| `loArm` | 1.35 | 前腕X(**無効・ロール軸**。触っても見た目が変わらない) |
| `loArmY` | 0.55 | 前腕Y(X左右の実用レバー、太ももの内側へ寄せる) |
| `handX` | -2.406 | 手首の曲げ(反復フィットで決定。手動調整は非推奨) |
| `handY` | -0.296 | 手首のひねり(同上) |
| `handZ` | -0.654 | 手首の左右振り(同上) |
| `chestX`(新規) | -0.12 | 胸を後ろへ開く(背筋を伸ばす方向) |
| `hips` | 0.0 | 骨盤の前傾 |
| `lean` | 0.06 | 背骨の前傾 |

## 6. 現在の状態(2026-07-16 座り腕/手首/胸の再調整コミット時点)

- 動作OK済み: マルチプロバイダAPI、VRM表示(Load failed解決済)、透過(白箱解決済)、
  口パク/表情/まばたき、待機の自律行動(キョロキョロ/振向き/その場歩き)、カーソル追従、
  人間らしい歩行(膝後畳み・かかと接地・腕逆位相・骨盤/重心)、明るめ照明
- 座り=割座(ぺたん座り): 足裏がお尻脇、すね/お尻が床。両手は§5.1の確定値で太もも上に届き、
  手のひら下向きで指先が太もも面に接触する状態をヘッドレス実描画で確認済み。胸を`chestX`で
  少し開き、背筋の見た目を改善(実機未確認)
- 配布準備済み: 既定画像なし(著作権配慮で図形キャラ)、アイコン、ユニバーサル(package.sh時のみ)、
  LICENSE/THIRD_PARTY_LICENSES.md/DISTRIBUTION.md
- 未確認: 今回の座り手・胸の調整はヘッドレスレンダリングのみで確認。**実機確認待ち**
  (前回の「手のひら下向き確認済み」も実機では再度崩れていた経緯があるため、必ず実機で見てもらう)

## 7. ユーザーの運用ルール・好み

- 修正指示や計画は Fable、実作業は **Sonnet に委任**(SonnetがきついときはOpus)= コスト削減方針
- 実機で見て気になる点をスクショ(または動画)で送ってくる。動画(.mov/HEVC)は
  この環境ではデコード不可 → スクショを依頼する
- 反映手順は毎回案内する: `cd ~/test && git pull && bash scripts/rebuild.sh`
- ペルソナ: セナ(FF8リノア風の明るいタメ口)。個人情報は埋め込まない。「軽い誠実さ」のみ

## 8. 既知の注意点

- レート制限(セッション上限)でサブエージェントが落ちることがある。作業途中の未コミット変更が
  残っていたら、この文書の検証手順で仕上げてからコミットする
- macOS標準bash 3.2では `set -u` + 空配列展開が落ちる(スクリプトでは配列を避ける)
- ヘッドレスのスクショ背景が白いのはツール仕様。透過検証は市松模様CSSを敷いて行う
- 今後のアイデア(README末尾): 音声会話、Live2D、デスクトップを実際に歩き回る(ウィンドウ移動)
