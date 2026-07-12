import Foundation

/// Anthropic Messages API (https://api.anthropic.com/v1/messages) を
/// URLSessionで直接呼び出すクライアント。SSEストリーミングで応答を受信する。
final class ClaudeClient {
    static let model = "claude-opus-4-8"
    static let endpoint = URL(string: "https://api.anthropic.com/v1/messages")!

    static let systemPrompt = """
    あなたはデスクトップに住む小さなマスコットキャラクター「モチ」です。\
    ユーザーのパソコン画面の隅にいて、話しかけられたらおしゃべりする相棒です。

    - 明るくフレンドリーで、ちょっとおっとりした性格です
    - 一人称は「ぼく」、語尾はやわらかく話します
    - 返事は基本的に短く(1〜3文程度)。長い説明を求められたときだけ詳しく答えます
    - 難しい質問にもできる範囲で誠実に答えます
    - 絵文字をたまに使います(使いすぎない)
    """

    struct APIMessage: Encodable {
        let role: String
        let content: String
    }

    struct APIError: Error, LocalizedError {
        let message: String
        var errorDescription: String? { message }
    }

    private struct RequestBody: Encodable {
        let model: String
        let maxTokens: Int
        let system: String
        let stream: Bool
        let messages: [APIMessage]

        enum CodingKeys: String, CodingKey {
            case model
            case maxTokens = "max_tokens"
            case system
            case stream
            case messages
        }
    }

    // SSEイベントのうち、必要なフィールドだけをデコードする
    private struct StreamEvent: Decodable {
        let type: String
        let delta: Delta?

        struct Delta: Decodable {
            let type: String?
            let text: String?
        }
    }

    private struct ErrorResponse: Decodable {
        let error: ErrorDetail?

        struct ErrorDetail: Decodable {
            let message: String?
        }
    }

    /// 会話履歴を送信し、応答テキストのチャンクを順次返すストリームを開く。
    func streamReply(
        history: [APIMessage],
        apiKey: String
    ) async throws -> AsyncThrowingStream<String, Error> {
        var request = URLRequest(url: Self.endpoint)
        request.httpMethod = "POST"
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        request.setValue(apiKey, forHTTPHeaderField: "x-api-key")
        request.setValue("2023-06-01", forHTTPHeaderField: "anthropic-version")
        request.timeoutInterval = 120

        let body = RequestBody(
            model: Self.model,
            maxTokens: 4096,
            system: Self.systemPrompt,
            stream: true,
            messages: history
        )
        request.httpBody = try JSONEncoder().encode(body)

        let (bytes, response) = try await URLSession.shared.bytes(for: request)

        guard let http = response as? HTTPURLResponse else {
            throw APIError(message: "サーバーから不正な応答を受信しました")
        }

        if http.statusCode != 200 {
            // エラーボディを読み取って内容を表示する
            var errorBody = ""
            for try await line in bytes.lines {
                errorBody += line
            }
            let detail = (try? JSONDecoder().decode(ErrorResponse.self, from: Data(errorBody.utf8)))?
                .error?.message
            throw APIError(
                message: detail ?? "APIエラー (HTTP \(http.statusCode))"
            )
        }

        return AsyncThrowingStream { continuation in
            let task = Task {
                do {
                    for try await line in bytes.lines {
                        guard line.hasPrefix("data: ") else { continue }
                        let payload = String(line.dropFirst(6))
                        guard let data = payload.data(using: .utf8),
                              let event = try? JSONDecoder().decode(StreamEvent.self, from: data)
                        else { continue }

                        switch event.type {
                        case "content_block_delta":
                            if event.delta?.type == "text_delta",
                               let text = event.delta?.text {
                                continuation.yield(text)
                            }
                        case "message_stop":
                            continuation.finish()
                            return
                        case "error":
                            continuation.finish(
                                throwing: APIError(message: "ストリーミング中にエラーが発生しました")
                            )
                            return
                        default:
                            break
                        }
                    }
                    continuation.finish()
                } catch {
                    continuation.finish(throwing: error)
                }
            }
            continuation.onTermination = { _ in
                task.cancel()
            }
        }
    }
}
