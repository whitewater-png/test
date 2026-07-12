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

        let provider = AppSettings.selectedProvider
        guard let apiKey = AppSettings.apiKey(for: provider) else {
            errorMessage = "\(provider.displayName) のAPIキーが未設定です。メニューバーのアイコン → 「設定」から登録してください。"
            return
        }
        let model = AppSettings.model(for: provider)
        let backend = provider.makeBackend()

        inputText = ""
        errorMessage = nil
        messages.append(ChatMessage(role: .user, text: text))
        scrollTick += 1

        let history = messages.map { ChatTurn(role: $0.role, text: $0.text) }

        isWaitingForFirstToken = true

        currentTask = Task { [weak self] in
            guard let self else { return }
            do {
                let stream = try await backend.streamReply(
                    history: history,
                    apiKey: apiKey,
                    model: model
                )

                // Intのインデックスではなく UUID で追記先メッセージを特定する。
                // 受信中に会話がリセットされても、古いインデックスで
                // 配列外アクセスしてクラッシュすることがないようにするため。
                var assistantID: UUID?
                for try await chunk in stream {
                    if Task.isCancelled { break }

                    if assistantID == nil {
                        self.isWaitingForFirstToken = false
                        self.isStreaming = true
                        let message = ChatMessage(role: .assistant, text: "")
                        assistantID = message.id
                        self.messages.append(message)
                    }
                    if let id = assistantID {
                        guard let index = self.messages.firstIndex(where: { $0.id == id }) else {
                            // 会話がリセットされて追記先が消えた場合は受信を打ち切る
                            break
                        }
                        self.messages[index].text += chunk
                        self.scrollTick += 1
                    }
                }

                // 一文字も返らずに終了した場合(セーフティ拒否など)
                if assistantID == nil, !Task.isCancelled {
                    self.errorMessage = "応答を取得できませんでした。もう一度試してみてください。"
                }
            } catch {
                // リセットによるキャンセルはエラーとして表示しない
                if !Task.isCancelled {
                    self.errorMessage = "エラー: \(error.localizedDescription)"
                }
            }
            // キャンセル時は clearConversation() 側で状態をリセット済み。
            // (直後に始まった新しい送信の状態を上書きしないようにする)
            if !Task.isCancelled {
                self.isWaitingForFirstToken = false
                self.isStreaming = false
            }
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
