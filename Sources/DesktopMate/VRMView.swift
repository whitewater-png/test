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
        // モデルのバイト列は fetch ではなく、このメッセージハンドラ経由で JS に渡す。
        config.userContentController.addScriptMessageHandler(
            context.coordinator, contentWorld: .page, name: "model"
        )

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

    final class Coordinator: NSObject, WKURLSchemeHandler, WKScriptMessageHandlerWithReply {
        var lastToken: Int

        init(reloadToken: Int) {
            self.lastToken = reloadToken
        }

        func load(into webView: WKWebView) {
            guard let url = URL(string: "\(VRMView.scheme)://app/viewer.html") else { return }
            webView.load(URLRequest(url: url))
        }

        // MARK: - WKScriptMessageHandlerWithReply
        // JS から要求されたら、モデルのバイト列を base64 文字列で返す。

        func userContentController(
            _ userContentController: WKUserContentController,
            didReceive message: WKScriptMessage,
            replyHandler: @escaping (Any?, String?) -> Void
        ) {
            guard let data = try? Data(contentsOf: VRMStore.modelURL) else {
                replyHandler(nil, "モデルファイルが見つかりません")
                return
            }
            replyHandler(data.base64EncodedString(), nil)
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
                // viewer.html の //__VRM_BUNDLE__ マーカーを、同梱の bundle.js
                // (three.js + three-vrm + 描画ロジック)で差し替えてインライン配信する。
                // これによりサブリソース読込もCDNアクセスも不要になる。
                if let htmlURL = Bundle.module.url(forResource: "viewer", withExtension: "html"),
                   var html = try? String(contentsOf: htmlURL, encoding: .utf8) {
                    if let bundleURL = Bundle.module.url(forResource: "bundle", withExtension: "js"),
                       let js = try? String(contentsOf: bundleURL, encoding: .utf8) {
                        html = html.replacingOccurrences(of: "//__VRM_BUNDLE__", with: js)
                    }
                    payload = html.data(using: .utf8)
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
