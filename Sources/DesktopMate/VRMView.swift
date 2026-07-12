import AppKit
import SwiftUI
import WebKit

/// VRMモデルを透明なWKWebView(three.js + three-vrm)で表示するビュー。
/// viewer.html を model.vrm と同じディレクトリにコピーし、
/// そこから相対パスで ./model.vrm を読み込む。
struct VRMView: NSViewRepresentable {
    /// この値が変わると再読み込みする(VRM差し替え時)
    let reloadToken: Int

    func makeNSView(context: Context) -> WKWebView {
        let webView = WKWebView(frame: .zero, configuration: WKWebViewConfiguration())

        // 背景を透過させる(キャラクターだけを浮かせる)
        webView.underPageBackgroundColor = .clear
        if webView.responds(to: NSSelectorFromString("setDrawsBackground:")) {
            webView.setValue(false, forKey: "drawsBackground")
        }
        webView.wantsLayer = true
        webView.layer?.isOpaque = false
        webView.layer?.backgroundColor = NSColor.clear.cgColor

        context.coordinator.load(into: webView)
        return webView
    }

    func updateNSView(_ webView: WKWebView, context: Context) {
        if context.coordinator.lastToken != reloadToken {
            context.coordinator.lastToken = reloadToken
            context.coordinator.load(into: webView)
        }
    }

    func makeCoordinator() -> Coordinator {
        Coordinator(reloadToken: reloadToken)
    }

    final class Coordinator {
        var lastToken: Int

        init(reloadToken: Int) {
            self.lastToken = reloadToken
        }

        func load(into webView: WKWebView) {
            guard let bundled = Bundle.module.url(forResource: "viewer", withExtension: "html") else {
                return
            }
            // viewer.html を model.vrm と同じディレクトリに置き、そのディレクトリへ読み取り許可を与える。
            // こうすると viewer.html 内の fetch('./model.vrm') が通る。
            let dir = VRMStore.directory
            let destination = dir.appendingPathComponent("viewer.html")
            try? FileManager.default.removeItem(at: destination)
            do {
                try FileManager.default.copyItem(at: bundled, to: destination)
            } catch {
                return
            }
            webView.loadFileURL(destination, allowingReadAccessTo: dir)
        }
    }
}
