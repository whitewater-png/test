import AppKit
import SwiftUI

/// キャラクターとチャット吹き出しを載せる、透明・最前面のボーダーレスウィンドウ。
///
/// クリック透過対策として、ウィンドウの大きさをチャットの開閉に合わせて切り替える。
/// (透明な余白部分もマウスイベントを捕捉してしまい、下のアプリが操作できなくなるため、
/// チャットを閉じている間はキャラクター分の最小サイズに縮めておく)
final class MascotWindow: NSWindow {
    /// チャットを閉じているとき(キャラクターのみ)のサイズ
    static let compactSize = NSSize(width: 200, height: 260)
    /// チャットを開いているときのサイズ
    static let expandedSize = NSSize(width: 340, height: 680)

    init() {
        let screenFrame = NSScreen.main?.visibleFrame
            ?? NSRect(x: 0, y: 0, width: 1440, height: 900)
        let size = Self.compactSize
        // 画面右下に配置(チャットを開いたときに展開後の幅が画面内に収まるよう、
        // 展開時サイズの右端が「右端から32pt」に来る位置を基準に中央を合わせる)
        let origin = NSPoint(
            x: screenFrame.maxX - 32 - Self.expandedSize.width / 2 - size.width / 2,
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
        // (確実なドラッグは MascotInteractionView の performDrag が担う)
        isMovableByWindowBackground = true

        let hostingView = NSHostingView(rootView: ContentView())
        // ウィンドウサイズは applyChatLayout(isOpen:) で自前管理するため、
        // SwiftUIコンテンツ由来の自動ウィンドウリサイズは無効化しておく
        hostingView.sizingOptions = []
        contentView = hostingView
    }

    // ボーダーレスウィンドウはデフォルトでキーになれないため、
    // チャット入力欄にフォーカスできるよう明示的に許可する
    override var canBecomeKey: Bool { true }

    /// チャットの開閉に合わせてウィンドウサイズを切り替える。
    /// キャラクターが動いて見えないよう、ウィンドウ下端中央の位置を維持する。
    func applyChatLayout(isOpen: Bool) {
        let newSize = isOpen ? Self.expandedSize : Self.compactSize
        guard frame.size != newSize else { return }

        var newFrame = NSRect(
            x: frame.midX - newSize.width / 2,
            y: frame.minY,
            width: newSize.width,
            height: newSize.height
        )

        // 画面端では、展開したチャットがはみ出さないよう画面内に収める
        if let visible = (screen ?? NSScreen.main)?.visibleFrame {
            newFrame.origin.x = min(newFrame.origin.x, visible.maxX - newSize.width)
            newFrame.origin.x = max(newFrame.origin.x, visible.minX)
            newFrame.origin.y = min(newFrame.origin.y, visible.maxY - newSize.height)
            newFrame.origin.y = max(newFrame.origin.y, visible.minY)
        }

        setFrame(newFrame, display: true, animate: false)
    }
}
