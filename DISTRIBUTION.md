# 配布ガイド（DesktopMate）

このアプリを他の人に配る手順です。**未署名（Apple Developer 登録なし）での配布**を前提にしています。

---

## 1. 配布物をつくる（作者＝あなたの作業）

Mac で以下を実行します（Xcode Command Line Tools が必要）。

```bash
cd test
./scripts/package.sh 1.0.0     # 引数はバージョン番号
```

`dist/` に次の2つができます。どちらか（または両方）を配布します。

- `DesktopMate-1.0.0.dmg` … ダブルクリックで開き、アプリを Applications にドラッグ
- `DesktopMate-1.0.0.zip` … 展開すると `DesktopMate.app`

ビルドは **Apple Silicon / Intel 両対応（ユニバーサル）** なので、どちらの Mac でも動きます。

> バンドルID を自分のものにしたい場合:
> `DESKTOPMATE_BUNDLE_ID=com.yourname.desktopmate ./scripts/package.sh 1.0.0`

配布ページ（GitHub Releases など）には、この `.dmg`/`.zip` と一緒に
**「2. 受け取った人の開き方」** を必ず案内してください。

---

## 2. 受け取った人の開き方（未署名アプリの初回起動）

このアプリは Apple の署名・公証をしていないため、初回だけ macOS の Gatekeeper が
警告を出します。**初回だけ**次のいずれかで開けば、以降は普通にダブルクリックで起動できます。

**方法A（おすすめ・右クリック）**
1. `DesktopMate.app` を Finder で **右クリック（Control＋クリック）→「開く」**
2. 「開いてもよろしいですか？」で **「開く」** を押す

**方法B（"壊れている" と出て開けない場合）**
ダウンロードした zip/dmg には隔離属性が付き、環境によっては「壊れているため開けません」と
表示されることがあります。ターミナルで隔離属性を外します。

```bash
xattr -dr com.apple.quarantine /Applications/DesktopMate.app
```

（アプリを置いた場所に合わせてパスを変えてください）

> これらは「未署名アプリだから」出るもので、アプリの不具合ではありません。
> 警告を完全になくすには、作者が Apple Developer Program（年 $99）で
> **Developer ID 署名＋公証(notarization)** を行う必要があります。

---

## 3. 使い始め（メニューバー常駐）

- 起動すると Dock ではなく **メニューバーに 😊 アイコン**が出ます。
- 初回は設定画面が開くので、使う AI プロバイダー（Anthropic / OpenAI / Google）の
  **API キー**を入れて保存します。キーは各自の macOS キーチェーンに保存されます。
- キャラクターは既定で図形描画です。設定から画像や VRM/GLB を選ぶと差し替えられます。

---

## 4. ライセンス表記について

- 本体は MIT ライセンス（`LICENSE`）。
- 同梱する 3D 描画ライブラリ（three.js / @pixiv/three-vrm、いずれも MIT）の表記を
  `THIRD_PARTY_LICENSES.md` にまとめています。配布物にこのファイルも含めてください。
- 他者が作った VRM / 画像を同梱して再配布する場合は、その作者の許諾条件を必ず確認してください。
