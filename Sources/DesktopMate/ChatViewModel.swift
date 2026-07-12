import Foundation
import SwiftUI

enum ChatRole {
    case user
    case assistant
}

struct ChatMessage: Identifiable {
    let id = UUID()
    let role: ChatRole
    var text: String
}

@MainActor
final class ChatViewModel: ObservableObject {
    @Published var isChatOpen = false
    @Published var messages: [ChatMessage] = []
    @Published var inputText = ""
    @Published var errorMessage: String?
    /// APIリクエスト中(最初のトークンが届くまで)
    @Published var isWaitingForFirstToken = false
    /// 応答テキストをストリーミング受信中
    @Published var isStreaming = false
    /// 自動スクロールのトリガー
    @Published var scrollTick = 0

    private let client = ClaudeClient()
    private var currentTask: Task<Void, Never>?

    var isResponding: Bool {
        isWaitingForFirstToken || isStreaming
    }

    var mood: MascotMood {
        if isWaitingForFirstToken { return .thinking }
        if isStreaming { return .talking }
        return .idle
    }

    func send() {
        let text = inputText.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !text.isEmpty, !isResponding else { return }

        guard let apiKey = AppSettings.resolveAPIKey() else {
            errorMessage = "APIキーが未設定です。メニューバーのアイコン → 「設定」からAnthropic APIキーを登録してください。"
            return
        }

        inputText = ""
        errorMessage = nil
        messages.append(ChatMessage(role: .user, text: text))
        scrollTick += 1

        let history = messages.map { message in
            ClaudeClient.APIMessage(
                role: message.role == .user ? "user" : "assistant",
                content: message.text
            )
        }

        isWaitingForFirstToken = true

        currentTask = Task { [weak self] in
            guard let self else { return }
            do {
                let stream = try await self.client.streamReply(
                    history: history,
                    apiKey: apiKey
                )

                var assistantIndex: Int?
                for try await chunk in stream {
                    if assistantIndex == nil {
                        self.isWaitingForFirstToken = false
                        self.isStreaming = true
                        self.messages.append(ChatMessage(role: .assistant, text: ""))
                        assistantIndex = self.messages.count - 1
                    }
                    if let index = assistantIndex {
                        self.messages[index].text += chunk
                        self.scrollTick += 1
                    }
                }

                // 一文字も返らずに終了した場合(セーフティ拒否など)
                if assistantIndex == nil {
                    self.errorMessage = "応答を取得できませんでした。もう一度試してみてください。"
                }
            } catch {
                self.errorMessage = "エラー: \(error.localizedDescription)"
            }
            self.isWaitingForFirstToken = false
            self.isStreaming = false
        }
    }

    func clearConversation() {
        currentTask?.cancel()
        currentTask = nil
        messages.removeAll()
        errorMessage = nil
        isWaitingForFirstToken = false
        isStreaming = false
    }
}
