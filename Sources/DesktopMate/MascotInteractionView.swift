import AppKit
import SwiftUI

/// キャラクター上のクリックとドラッグをAppKit側で確実に処理するオーバーレイ。
///
/// - ドラッグ: `NSWindow.performDrag(with:)` でウィンドウごと移動する。
///   (`isMovableByWindowBackground` は NSHostingView 上では効かないことが多いため)
/// - クリック: mouseDown から mouseUp までの移動量が小さければタップとみなし、
///   `onTap` を呼ぶ(チャットの開閉)。ドラッグと競合しない。
struct MascotInteractionView: NSViewRepresentable {
    /// タップ時に呼ばれる。ウィンドウリサイズに使えるよう所属ウィンドウを渡す。
    let onTap: (NSWindow?) -> Void

    func makeNSView(context: Context) -> InteractionNSView {
        let view = InteractionNSView()
        view.onTap = onTap
        return view
    }

    func updateNSView(_ nsView: InteractionNSView, context: Context) {
        nsView.onTap = onTap
    }

    final class InteractionNSView: NSView {
        var onTap: ((NSWindow?) -> Void)?

        /// この距離(pt)以上動いたらドラッグ、未満ならタップとみなす
        private static let dragThreshold: CGFloat = 3.0

        private var mouseDownEvent: NSEvent?
        private var isDraggingWindow = false

        // アプリが非アクティブでも最初のクリックで反応できるようにする
        override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }

        override func mouseDown(with event: NSEvent) {
            mouseDownEvent = event
            isDraggingWindow = false
        }

        override func mouseDragged(with event: NSEvent) {
            guard !isDraggingWindow, let downEvent = mouseDownEvent else { return }

            let dx = event.locationInWindow.x - downEvent.locationInWindow.x
            let dy = event.locationInWindow.y - downEvent.locationInWindow.y
            guard (dx * dx + dy * dy).squareRoot() >= Self.dragThreshold else { return }

            // ここからはウィンドウ移動として扱う
            // (performDrag 開始後の mouseUp は届かないことがあるため、
            // フラグでタップ判定を確実に打ち切っておく)
            isDraggingWindow = true
            window?.performDrag(with: downEvent)
        }

        override func mouseUp(with event: NSEvent) {
            if !isDraggingWindow, mouseDownEvent != nil {
                onTap?(window)
            }
            mouseDownEvent = nil
            isDraggingWindow = false
        }
    }
}
