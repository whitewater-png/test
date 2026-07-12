import AppKit
import SwiftUI

struct ContentView: View {
    @StateObject private var chat = ChatViewModel()

    var body: some View {
        VStack(spacing: 10) {
            Spacer(minLength: 0)

            if chat.isChatOpen {
                ChatBubbleView(chat: chat)
                    .transition(.scale(scale: 0.85, anchor: .bottom).combined(with: .opacity))
            }

            MascotView(mood: chat.mood)
                .overlay(
                    // クリック(チャット開閉)とドラッグ(ウィンドウ移動)をAppKit側で処理する
                    MascotInteractionView { window in
                        toggleChat(in: window)
                    }
                )
                .help("クリックでおしゃべり / ドラッグで移動")
        }
        .padding(.horizontal, 10)
        .padding(.bottom, 14)
        .frame(
            width: contentSize.width,
            height: contentSize.height,
            alignment: .bottom
        )
        // ウィンドウリサイズの途中でも内容が下端中央に張り付くようにする
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .bottom)
    }

    /// チャットの開閉状態に応じたコンテンツサイズ(ウィンドウサイズと一致させる)
    private var contentSize: NSSize {
        chat.isChatOpen ? MascotWindow.expandedSize : MascotWindow.compactSize
    }

    /// チャットを開閉する。透明部分がクリックを奪わないよう、
    /// 開くときは先にウィンドウを広げ、閉じるときはアニメーション完了後に縮める。
    private func toggleChat(in window: NSWindow?) {
        let mascotWindow = window as? MascotWindow
        let animation = Animation.spring(response: 0.35, dampingFraction: 0.8)

        if chat.isChatOpen {
            withAnimation(animation) {
                chat.isChatOpen = false
            } completion: {
                mascotWindow?.applyChatLayout(isOpen: false)
            }
        } else {
            mascotWindow?.applyChatLayout(isOpen: true)
            // チャット入力欄にフォーカスできるようキーウィンドウにする
            mascotWindow?.makeKey()
            withAnimation(animation) {
                chat.isChatOpen = true
            }
        }
    }
}
