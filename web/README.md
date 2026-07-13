# VRM ビューアのフロントエンド

`glue.js` が VRM/GLB を three.js + @pixiv/three-vrm で描画するソースです。
チャット状態(`__setState__({mood, talking})`)に応じて、口パク・表情・頭と体の
動きを反映します。

## バンドルの再生成

`glue.js` を編集したら、単一ファイルにバンドルして `Sources/DesktopMate/Resources/bundle.js`
を更新します(three.js等を同梱し、実行時のCDN依存をなくすため)。

```sh
npm install three@0.160.0 @pixiv/three-vrm@3.5.5 esbuild
npx esbuild glue.js --bundle --format=iife --minify --platform=browser \
  --outfile=../Sources/DesktopMate/Resources/bundle.js
```

アプリ起動時、`VRMView` の URLスキームハンドラが `viewer.html` にこの `bundle.js` と
モデルの base64 を差し込んでインライン配信します(fetch・メッセージハンドラ不使用)。
