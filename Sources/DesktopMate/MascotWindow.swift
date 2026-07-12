import AppKit
import SwiftUI

/// キャラクターとチャット吹き出しを載せる、透明・最前面のボーダーレスウィンドウ。
final class MascotWindow: NSWindow {
    static let contentSize = NSSize(width: 340, height: 600)

    init() {
        let screenFrame = NSScreen.main?.visibleFrame
            ?? NSRect(x: 0, y: 0, width: 1440, height: 900)
        let size = Self.contentSize
        // 画面右下に配置
        let origin = NSPoint(
            x: screenFrame.maxX - size.width - 32,
            y: screenFrame.minY + 24
        )

        super.init(
            contentRect: NSRect(origin: origin, size: size),
            styleMask: [.borderless],
            backing: .buffered,
            defer: false
        )

        isOpaque = false
        backgroundColor = .clear
        hasShadow = false
        level = .floating
        collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary]
        // キャラクター部分をドラッグしてウィンドウごと移動できるようにする
        isMovableByWindowBackground = true

        contentView = NSHostingView(rootView: ContentView())
    }

    // ボーダーレスウィンドウはデフォルトでキーになれないため、
    // チャット入力欄にフォーカスできるよう明示的に許可する
    override var canBecomeKey: Bool { true }
}
