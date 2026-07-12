import SwiftUI

/// キャラクターの上に表示される吹き出しチャットUI。
struct ChatBubbleView: View {
    @ObservedObject var chat: ChatViewModel
    @FocusState private var isInputFocused: Bool

    var body: some View {
        VStack(spacing: 0) {
            header

            Divider()

            messageList

            Divider()

            inputBar
        }
        .frame(width: 316, height: 380)
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 18, style: .continuous))
        .overlay(
            RoundedRectangle(cornerRadius: 18, style: .continuous)
                .stroke(Color.primary.opacity(0.1), lineWidth: 1)
        )
        .shadow(color: .black.opacity(0.2), radius: 12, y: 6)
        .onAppear {
            isInputFocused = true
        }
    }

    private var header: some View {
        HStack {
            Text("セナとおしゃべり")
                .font(.headline)
            Spacer()
            if !chat.messages.isEmpty {
                Button {
                    chat.clearConversation()
                } label: {
                    Image(systemName: "trash")
                }
                .buttonStyle(.plain)
                .foregroundStyle(.secondary)
                // 応答受信中のリセットは受信処理と競合するため無効化する
                .disabled(chat.isResponding)
                .help("会話をリセット")
            }
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 10)
    }

    private var messageList: some View {
        ScrollViewReader { proxy in
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 10) {
                    if chat.messages.isEmpty {
                        Text("おハロー！ なんでも話しかけてね！")
                            .font(.callout)
                            .foregroundStyle(.secondary)
                            .frame(maxWidth: .infinity, alignment: .center)
                            .padding(.top, 24)
                    }

                    ForEach(chat.messages) { message in
                        MessageRowView(message: message)
                            .id(message.id)
                    }

                    if let error = chat.errorMessage {
                        Text(error)
                            .font(.caption)
                            .foregroundStyle(.red)
                            .padding(10)
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .background(
                                Color.red.opacity(0.08),
                                in: RoundedRectangle(cornerRadius: 10)
                            )
                    }
                }
                .padding(12)
            }
            .onChange(of: chat.scrollTick) { _, _ in
                if let lastID = chat.messages.last?.id {
                    withAnimation(.easeOut(duration: 0.15)) {
                        proxy.scrollTo(lastID, anchor: .bottom)
                    }
                }
            }
        }
    }

    private var inputBar: some View {
        HStack(spacing: 8) {
            TextField("メッセージを入力…", text: $chat.inputText)
                .textFieldStyle(.plain)
                .focused($isInputFocused)
                .onSubmit {
                    chat.send()
                }

            Button {
                chat.send()
            } label: {
                Image(systemName: "paperplane.fill")
                    .foregroundStyle(canSend ? Color.accentColor : Color.secondary)
            }
            .buttonStyle(.plain)
            .disabled(!canSend)
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 10)
    }

    private var canSend: Bool {
        !chat.inputText.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
            && !chat.isResponding
    }
}

struct MessageRowView: View {
    let message: ChatMessage

    var body: some View {
        HStack {
            if message.role == .user { Spacer(minLength: 40) }

            Text(message.text.isEmpty ? "…" : message.text)
                .font(.callout)
                .textSelection(.enabled)
                .padding(.horizontal, 12)
                .padding(.vertical, 8)
                .background(bubbleColor, in: RoundedRectangle(cornerRadius: 14, style: .continuous))
                .foregroundStyle(message.role == .user ? Color.white : Color.primary)

            if message.role == .assistant { Spacer(minLength: 40) }
        }
    }

    private var bubbleColor: Color {
        switch message.role {
        case .user:
            return Color.accentColor
        case .assistant:
            return Color.primary.opacity(0.08)
        }
    }
}
