import AppKit
import SwiftUI
import WebKit

/// VRM/GLBモデルを透明なWKWebView(three.js + three-vrm)で表示するビュー。
///
/// WKWebViewは file:// への fetch を CORS でブロックするため、
/// カスタムURLスキーム(desktopmate://)で viewer.html とモデルを同一オリジンから
/// 配信する。これにより GLTFLoader の fetch('./model.vrm') が通る。
struct VRMView: NSViewRepresentable {
    static let scheme = "desktopmate"

    /// この値が変わると再読み込みする(モデル差し替え時)
    let reloadToken: Int

    func makeNSView(context: Context) -> WKWebView {
        let config = WKWebViewConfiguration()
        config.setURLSchemeHandler(context.coordinator, forURLScheme: VRMView.scheme)

        let webView = WKWebView(frame: .zero, configuration: config)

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

    final class Coordinator: NSObject, WKURLSchemeHandler {
        var lastToken: Int

        init(reloadToken: Int) {
            self.lastToken = reloadToken
        }

        func load(into webView: WKWebView) {
            guard let url = URL(string: "\(VRMView.scheme)://app/viewer.html") else { return }
            webView.load(URLRequest(url: url))
        }

        // MARK: - WKURLSchemeHandler

        func webView(_ webView: WKWebView, start urlSchemeTask: WKURLSchemeTask) {
            guard let url = urlSchemeTask.request.url else {
                urlSchemeTask.didFailWithError(URLError(.badURL))
                return
            }

            var payload: Data?
            var mimeType = "application/octet-stream"

            switch url.lastPathComponent {
            case "viewer.html":
                if let htmlURL = Bundle.module.url(forResource: "viewer", withExtension: "html") {
                    payload = try? Data(contentsOf: htmlURL)
                }
                mimeType = "text/html; charset=utf-8"
            case "model.vrm":
                payload = try? Data(contentsOf: VRMStore.modelURL)
                mimeType = "model/gltf-binary"
            default:
                break
            }

            guard let data = payload else {
                urlSchemeTask.didFailWithError(URLError(.fileDoesNotExist))
                return
            }

            let headers = [
                "Content-Type": mimeType,
                "Content-Length": "\(data.count)",
                "Access-Control-Allow-Origin": "*",
            ]
            guard let response = HTTPURLResponse(
                url: url, statusCode: 200, httpVersion: "HTTP/1.1", headerFields: headers
            ) else {
                urlSchemeTask.didFailWithError(URLError(.badServerResponse))
                return
            }

            urlSchemeTask.didReceive(response)
            urlSchemeTask.didReceive(data)
            urlSchemeTask.didFinish()
        }

        func webView(_ webView: WKWebView, stop urlSchemeTask: WKURLSchemeTask) {
            // 同期的に配信を完了しているため、特別な中断処理は不要。
        }
    }
}
