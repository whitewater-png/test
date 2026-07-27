# Blender → VRM の手順

デスクトップに立たせる子は **VRM 1.0（または 0.x）** で書き出します。

## 1. アドオンを入れる

[VRM Add-on for Blender](https://vrm-addon-for-blender.info/) をインストール。
`File → Export` に `VRM (.vrm)` が増えます。

## 2. 書き出す前にチェック

```bash
blender your-character.blend --background --python blender/vrm_export_check.py
```

見るのはこの4点です。

| 項目 | なぜ大事か |
| --- | --- |
| Humanoidボーン | VRMAモーションはボーン名ではなく **humanoid の役割** に流し込まれます。ここが埋まっていないと踊れません |
| トランスフォーム適用 | スケール・回転が残っていると、書き出し後に浮いたり潰れたりします（`Ctrl+A`） |
| シェイプキー | `blink` で瞬き、`aa` で口パク、`happy` / `sad` などで表情。無くても表示はできますが無表情になります |
| メッシュ・マテリアル数 | 常時表示のオーバーレイなので、マテリアルはできるだけ束ねた方が軽いです |

## 3. VRM Add-on 側の設定

- **Humanoid** パネル … 必須ボーンを全部割り当てる（赤が残っていたら書き出さない）
- **Expressions** パネル … `happy` / `angry` / `sad` / `relaxed` / `surprised` / `blink` / `aa` を自分のシェイプキーに紐付け
- **Spring Bone** パネル … 髪・スカート・リボンに設定すると、動いたときに揺れます
- **First Person** パネル … 一人称視点は使わないので既定のままでOK

## 4. 配置

書き出した `.vrm` を `models/` に置くだけ。ファイル名がそのままキャラ名になります。
複数置いた場合は先頭が既定で、`?model=名前` で切り替えられます。

## モーション（.vrma）について

踊りは VRM Animation（`.vrma`）で、`motions/` に入れたものが自動で読み込まれます。
ファイル名がそのままモーション名 = Claudeに渡す名前です。

```
motions/
  idle.vrma          ← このファイル名だけ特別。あればループ再生されます
  pokedance.vrma
  shikanoko.vrma
```

**販売元の書き出しツールによっては `specVersion` が抜けていて読み込めないファイルがあります。**
このプロジェクトは配信時に自動で直すので基本そのまま置けばいいですが、
ファイル自体を直したいときは:

```bash
npm run fix-vrma            # motions/ を全部チェックして修正（元ファイルは .bak で残る）
npm run fix-vrma -- --check # 何が壊れてるか見るだけ
```
