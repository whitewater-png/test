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

    /// チャットの状態(考え中/発話中)。VRMの表情・動きに連動させる。
    let mood: MascotMood

    func makeNSView(context: Context) -> WKWebView {
        let config = WKWebViewConfiguration()
        config.setURLSchemeHandler(context.coordinator, forURLScheme: VRMView.scheme)

        let webView = WKWebView(frame: .zero, configuration: config)

        // 背景を透過させる(キャラクターだけを浮かせる)。
        // WKWebView は既定で不透明な白い背景を描くため、これを止めないと
        // キャラの周りが白い箱になる。macOS には公開APIが無く、WebKit は
        // drawsBackground をプライベートの _drawsBackground ivar で保持している。
        // responds(to: setDrawsBackground:) は環境によって false を返す(公開
        // セッターメソッドが無いため)が、KVC は ivar へ直接書き込めるので
        // ガードせず設定する(WebKit が長年サポートしている定番手法)。
        webView.setValue(false, forKey: "drawsBackground")
        webView.underPageBackgroundColor = .clear
        webView.wantsLayer = true
        webView.layer?.isOpaque = false
        webView.layer?.backgroundColor = NSColor.clear.cgColor

        context.coordinator.load(into: webView)
        context.coordinator.startPointerTracking(webView)
        return webView
    }

    static func dismantleNSView(_ nsView: WKWebView, coordinator: Coordinator) {
        coordinator.stopPointerTracking()
    }

    func updateNSView(_ webView: WKWebView, context: Context) {
        if context.coordinator.lastToken != reloadToken {
            context.coordinator.lastToken = reloadToken
            context.coordinator.lastMoodKey = ""   // 再読み込み後に状態を再送する
            context.coordinator.load(into: webView)
        }
        pushState(to: webView, coordinator: context.coordinator)
    }

    /// チャット状態の変化を JS(__setState__)へ送り、表情・動きに反映する。
    private func pushState(to webView: WKWebView, coordinator: Coordinator) {
        let moodStr: String
        let talking: Bool
        switch mood {
        case .idle: moodStr = "idle"; talking = false
        case .thinking: moodStr = "thinking"; talking = false
        case .talking: moodStr = "talking"; talking = true
        }
        let key = "\(moodStr)-\(talking)"
        guard coordinator.lastMoodKey != key else { return }
        coordinator.lastMoodKey = key

        let js = "window.__setState__ && window.__setState__({mood:'\(moodStr)',talking:\(talking)})"
        webView.evaluateJavaScript(js, completionHandler: nil)
    }

    func makeCoordinator() -> Coordinator {
        Coordinator(reloadToken: reloadToken)
    }

    final class Coordinator: NSObject, WKURLSchemeHandler {
        var lastToken: Int
        var lastMoodKey: String = ""

        private weak var webView: WKWebView?
        private var pointerTimer: Timer?

        init(reloadToken: Int) {
            self.lastToken = reloadToken
        }

        deinit { pointerTimer?.invalidate() }

        func load(into webView: WKWebView) {
            guard let url = URL(string: "\(VRMView.scheme)://app/viewer.html") else { return }
            webView.load(URLRequest(url: url))
        }

        // MARK: - マウスカーソル追従

        /// デスクトップ上のカーソル位置を定期的に読み取り、キャラの近くにある時だけ
        /// 方向を JS(__setPointer__)へ送る。近くにいない間はキャラ自身の
        /// 待機アニメーション(歩く/振り向く等)に任せる。
        func startPointerTracking(_ webView: WKWebView) {
            self.webView = webView
            pointerTimer?.invalidate()
            let timer = Timer(timeInterval: 1.0 / 20.0, repeats: true) { [weak self] _ in
                self?.tickPointer()
            }
            RunLoop.main.add(timer, forMode: .common)
            pointerTimer = timer
        }

        func stopPointerTracking() {
            pointerTimer?.invalidate()
            pointerTimer = nil
        }

        private func tickPointer() {
            guard let webView = webView, let win = webView.window, win.isVisible else { return }
            let mouse = NSEvent.mouseLocation              // 画面座標(左下原点)
            let f = win.frame
            let refX = f.midX
            let refY = f.midY + f.height * 0.18            // 頭のあたりを基準にする
            let dx = mouse.x - refX
            let dy = mouse.y - refY
            let dist = (dx * dx + dy * dy).squareRoot()
            guard dist < 440 else { return }               // 近くにいる時だけ追う
            let scale: CGFloat = 260
            let nx = max(-1, min(1, dx / scale))
            let ny = max(-1, min(1, dy / scale))
            let js = "window.__setPointer__ && window.__setPointer__({x:\(String(format: "%.3f", nx)),y:\(String(format: "%.3f", ny))})"
            webView.evaluateJavaScript(js, completionHandler: nil)
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
                // viewer.html に、同梱の bundle.js(three.js + three-vrm + 描画ロジック)と
                // モデルのバイト列(base64)を差し込んでインライン配信する。
                // これで fetch・メッセージハンドラ・CDN・サブリソース読込を全て排除し、
                // 唯一「このHTMLをスキーム配信する」仕組みだけで完結する。
                if let htmlURL = Bundle.module.url(forResource: "viewer", withExtension: "html"),
                   var html = try? String(contentsOf: htmlURL, encoding: .utf8) {
                    if let bundleURL = Bundle.module.url(forResource: "bundle", withExtension: "js"),
                       let js = try? String(contentsOf: bundleURL, encoding: .utf8) {
                        html = html.replacingOccurrences(of: "//__VRM_BUNDLE__", with: js)
                    }
                    let modelB64 = (try? Data(contentsOf: VRMStore.modelURL))?.base64EncodedString() ?? ""
                    html = html.replacingOccurrences(of: "__MODEL_B64_TOKEN__", with: modelB64)
                    payload = html.data(using: .utf8)
                }
                mimeType = "text/html; charset=utf-8"
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
