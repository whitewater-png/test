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
                .contentShape(Rectangle())
                .onTapGesture {
                    withAnimation(.spring(response: 0.35, dampingFraction: 0.8)) {
                        chat.isChatOpen.toggle()
                    }
                }
                .help("クリックでおしゃべり / ドラッグで移動")
        }
        .padding(.horizontal, 10)
        .padding(.bottom, 14)
        .frame(
            width: MascotWindow.contentSize.width,
            height: MascotWindow.contentSize.height,
            alignment: .bottom
        )
    }
}
