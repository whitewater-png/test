import Foundation

/// マスコットの人格(全プロバイダー共通のシステムプロンプト)。
enum AssistantPersona {
    static let systemPrompt = """
    あなたはデスクトップに住むキャラクター「セナ」です。\
    ユーザーのPC画面の隅にいて、話しかけられたら親しい相棒としておしゃべりします。\
    全体の雰囲気は、ファイナルファンタジーVIIIのリノア・ハーティリーのような、\
    明るくて芯のある女の子です。

    # 性格
    - 天真爛漫でポジティブ、それでいて芯が強い。
    - 理屈よりも温かい包容力で寄り添う。仲間との絆や大切なものを守る気持ちを大事にする。
    - 正直でストレートに話す。オブラートには包まない。ときどき軽やかなユーモアを混ぜる。

    # 話し方
    - 一人称は「私」。明るく親しみやすい20代女性の自然なタメ口(〜だよ / 〜だね / 〜かな / 〜だし！)。敬語は使わない。
    - 「〜じゃない」「〜でしょ」「〜よ」などの自然な女性語を使う。少し茶目っ気のある語尾。
    - 挨拶は「おハロー！」「やっほー！」など。前向きな視点で励ます。
    - 相手の名前が分かるときは、親しみを込めて名前で呼ぶ。
    - 返事は基本的に短めに(1〜3文)。長い説明を求められたときだけ詳しく話す。

    # 誠実さ
    - 事実に関する質問には正直に答える。確信がないときは「たぶん〜だと思う」と、\
      推測であることを正直に伝える。知らないことは知らないと言う。

    # 話し方の例
    - 「おハロー！ 今日もいい調子? 私はバッチリだよ！」
    - 「大丈夫、私がついてるよ。なんとかなるって！」
    - 「ねえ、ちゃんと私のこと見ててよね? 目を離しちゃダメだよ」
    - 「ほら、笑って！ 暗い顔してたら、幸せが逃げちゃうよ?」
    """
}

/// 1ターン分の会話(バックエンドに渡す中立表現)。
struct ChatTurn {
    let role: ChatRole
    let text: String
}

struct BackendError: Error, LocalizedError {
    let message: String
    var statusCode: Int?

    init(message: String, statusCode: Int? = nil) {
        self.message = message
        self.statusCode = statusCode
    }

    var errorDescription: String? { message }
}

/// AIプロバイダーの抽象。会話履歴を送り、応答テキストのチャンクを順次返す。
protocol ChatBackend {
    func streamReply(
        history: [ChatTurn],
        apiKey: String,
        model: String
    ) async throws -> AsyncThrowingStream<String, Error>
}

// MARK: - 共通ヘルパー

/// リクエストを送り、HTTP 200 のときだけSSEバイト列を返す。
/// 非200のときはボディを読み取り、プロバイダーのエラーメッセージを添えて投げる。
private func openValidatedStream(_ request: URLRequest) async throws -> URLSession.AsyncBytes {
    let (bytes, response) = try await URLSession.shared.bytes(for: request)
    guard let http = response as? HTTPURLResponse else {
        throw BackendError(message: "サーバーから不正な応答を受信しました")
    }
    guard http.statusCode == 200 else {
        var body = ""
        for try await line in bytes.lines { body += line }
        throw BackendError(
            message: parseProviderError(body) ?? "APIエラー (HTTP \(http.statusCode))",
            statusCode: http.statusCode
        )
    }
    return bytes
}

/// Anthropic / OpenAI / Google いずれも `{"error":{"message":...}}` 形式のためまとめて解釈する。
private func parseProviderError(_ body: String) -> String? {
    struct ErrorEnvelope: Decodable {
        struct Detail: Decodable { let message: String? }
        let error: Detail?
    }
    guard let data = body.data(using: .utf8) else { return body.isEmpty ? nil : body }
    if let decoded = try? JSONDecoder().decode(ErrorEnvelope.self, from: data),
       let message = decoded.error?.message {
        return message
    }
    return body.isEmpty ? nil : body
}

/// SSEの1行から `data:` ペイロードを取り出す(先頭の空白も除去)。
private func ssePayload(_ line: String) -> String? {
    guard line.hasPrefix("data:") else { return nil }
    var payload = String(line.dropFirst(5))
    if payload.first == " " { payload.removeFirst() }
    return payload
}

// MARK: - Anthropic (Claude Messages API)

final class AnthropicBackend: ChatBackend {
    func streamReply(
        history: [ChatTurn],
        apiKey: String,
        model: String
    ) async throws -> AsyncThrowingStream<String, Error> {
        var request = URLRequest(url: URL(string: "https://api.anthropic.com/v1/messages")!)
        request.httpMethod = "POST"
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        request.setValue(apiKey, forHTTPHeaderField: "x-api-key")
        request.setValue("2023-06-01", forHTTPHeaderField: "anthropic-version")
        request.timeoutInterval = 120

        struct Message: Encodable {
            let role: String
            let content: String
        }
        struct Body: Encodable {
            let model: String
            let max_tokens: Int
            let system: String
            let stream: Bool
            let messages: [Message]
        }

        let body = Body(
            model: model,
            max_tokens: 4096,
            system: AssistantPersona.systemPrompt,
            stream: true,
            messages: history.map {
                Message(role: $0.role == .user ? "user" : "assistant", content: $0.text)
            }
        )
        request.httpBody = try JSONEncoder().encode(body)

        let bytes = try await openValidatedStream(request)

        struct Event: Decodable {
            let type: String
            let delta: Delta?
            struct Delta: Decodable {
                let type: String?
                let text: String?
            }
        }

        return AsyncThrowingStream { continuation in
            let task = Task {
                do {
                    for try await line in bytes.lines {
                        guard let payload = ssePayload(line),
                              let data = payload.data(using: .utf8),
                              let event = try? JSONDecoder().decode(Event.self, from: data)
                        else { continue }

                        switch event.type {
                        case "content_block_delta":
                            if event.delta?.type == "text_delta", let text = event.delta?.text {
                                continuation.yield(text)
                            }
                        case "message_stop":
                            continuation.finish()
                            return
                        case "error":
                            continuation.finish(
                                throwing: BackendError(message: "ストリーミング中にエラーが発生しました")
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
            continuation.onTermination = { _ in task.cancel() }
        }
    }
}

// MARK: - OpenAI (Chat Completions API)

final class OpenAIBackend: ChatBackend {
    func streamReply(
        history: [ChatTurn],
        apiKey: String,
        model: String
    ) async throws -> AsyncThrowingStream<String, Error> {
        var request = URLRequest(url: URL(string: "https://api.openai.com/v1/chat/completions")!)
        request.httpMethod = "POST"
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        request.setValue("Bearer \(apiKey)", forHTTPHeaderField: "Authorization")
        request.timeoutInterval = 120

        struct Message: Encodable {
            let role: String
            let content: String
        }
        struct Body: Encodable {
            let model: String
            let stream: Bool
            let max_tokens: Int
            let messages: [Message]
        }

        var messages = [Message(role: "system", content: AssistantPersona.systemPrompt)]
        messages += history.map {
            Message(role: $0.role == .user ? "user" : "assistant", content: $0.text)
        }
        let body = Body(model: model, stream: true, max_tokens: 4096, messages: messages)
        request.httpBody = try JSONEncoder().encode(body)

        let bytes = try await openValidatedStream(request)

        struct Chunk: Decodable {
            struct Choice: Decodable {
                struct Delta: Decodable { let content: String? }
                let delta: Delta?
            }
            let choices: [Choice]
        }

        return AsyncThrowingStream { continuation in
            let task = Task {
                do {
                    for try await line in bytes.lines {
                        guard let payload = ssePayload(line) else { continue }
                        if payload == "[DONE]" {
                            continuation.finish()
                            return
                        }
                        guard let data = payload.data(using: .utf8),
                              let chunk = try? JSONDecoder().decode(Chunk.self, from: data)
                        else { continue }

                        if let text = chunk.choices.first?.delta?.content, !text.isEmpty {
                            continuation.yield(text)
                        }
                    }
                    continuation.finish()
                } catch {
                    continuation.finish(throwing: error)
                }
            }
            continuation.onTermination = { _ in task.cancel() }
        }
    }
}

// MARK: - Google (Gemini generateContent API)

final class GoogleBackend: ChatBackend {
    /// モデル名が無効だった場合に試す既知の有効モデル。
    /// 将来のモデル名変更で「動かない」状態を避けるための自己修復用。
    private static let fallbackModels = ["gemini-2.0-flash", "gemini-1.5-flash"]

    /// 入力されたモデル名をGemini APIのID形式へ正規化する。
    /// - 前後の空白と「models/」接頭辞を除去
    /// - 表示名(スペースや大文字を含む)は小文字化しスペースをハイフンに
    ///   例: "Gemini 3.1 Flash Lite" → "gemini-3.1-flash-lite"
    static func normalizedModelName(_ raw: String) -> String {
        var name = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        if name.hasPrefix("models/") {
            name.removeFirst("models/".count)
        }
        if name.contains(" ") || name.contains(where: { $0.isUppercase }) {
            name = name.lowercased().replacingOccurrences(of: " ", with: "-")
        }
        return name
    }

    /// このエラーなら別のモデル候補を試すべきか。
    /// モデル未検出(404)や、モデル名の形式エラー(400でメッセージが model に言及)が対象。
    static func shouldTryNextModel(_ error: BackendError) -> Bool {
        if error.statusCode == 404 { return true }
        if error.statusCode == 400, error.message.lowercased().contains("model") { return true }
        return false
    }

    func streamReply(
        history: [ChatTurn],
        apiKey: String,
        model: String
    ) async throws -> AsyncThrowingStream<String, Error> {
        struct Part: Encodable { let text: String }
        struct Content: Encodable {
            let role: String
            let parts: [Part]
        }
        struct SystemInstruction: Encodable { let parts: [Part] }
        struct GenerationConfig: Encodable { let maxOutputTokens: Int }
        struct Body: Encodable {
            let systemInstruction: SystemInstruction
            let contents: [Content]
            let generationConfig: GenerationConfig
        }

        let contents = history.map {
            // Gemini のロールは user / model
            Content(role: $0.role == .user ? "user" : "model", parts: [Part(text: $0.text)])
        }
        let body = Body(
            systemInstruction: SystemInstruction(parts: [Part(text: AssistantPersona.systemPrompt)]),
            contents: contents,
            generationConfig: GenerationConfig(maxOutputTokens: 4096)
        )
        let encodedBody = try JSONEncoder().encode(body)

        func makeRequest(for model: String) throws -> URLRequest {
            // alt=sse を付けると Server-Sent Events 形式で返る
            let urlString =
                "https://generativelanguage.googleapis.com/v1beta/models/\(model):streamGenerateContent?alt=sse"
            guard let url = URL(string: urlString) else {
                throw BackendError(message: "モデル名が不正です: \(model)")
            }
            var request = URLRequest(url: url)
            request.httpMethod = "POST"
            request.setValue("application/json", forHTTPHeaderField: "Content-Type")
            request.setValue(apiKey, forHTTPHeaderField: "x-goog-api-key")
            request.timeoutInterval = 120
            request.httpBody = encodedBody
            return request
        }

        // 入力モデル名を正規化(例:「Gemini 3.1 Flash Lite」→「gemini-3.1-flash-lite」)
        // → 既知の有効モデル の順に試す。モデル未検出/名前形式エラーのときだけ次へ。
        let requested = Self.normalizedModelName(model)
        var candidates = [requested]
        for fallback in Self.fallbackModels where !candidates.contains(fallback) {
            candidates.append(fallback)
        }

        var bytes: URLSession.AsyncBytes?
        var lastError: Error?
        for candidate in candidates {
            do {
                bytes = try await openValidatedStream(try makeRequest(for: candidate))
                // 別モデルに切り替わった/正規化された場合は設定を更新して次回から直接使う
                if candidate != model {
                    AppSettings.setModel(candidate, for: .google)
                }
                break
            } catch let error as BackendError where Self.shouldTryNextModel(error) {
                lastError = error
                continue
            }
        }

        guard let bytes else {
            throw lastError ?? BackendError(message: "Geminiに接続できませんでした")
        }

        struct Chunk: Decodable {
            struct Candidate: Decodable {
                struct Content: Decodable {
                    struct Part: Decodable { let text: String? }
                    let parts: [Part]?
                }
                let content: Content?
            }
            let candidates: [Candidate]?
        }

        return AsyncThrowingStream { continuation in
            let task = Task {
                do {
                    for try await line in bytes.lines {
                        guard let payload = ssePayload(line),
                              let data = payload.data(using: .utf8),
                              let chunk = try? JSONDecoder().decode(Chunk.self, from: data)
                        else { continue }

                        if let parts = chunk.candidates?.first?.content?.parts {
                            let text = parts.compactMap { $0.text }.joined()
                            if !text.isEmpty { continuation.yield(text) }
                        }
                    }
                    continuation.finish()
                } catch {
                    continuation.finish(throwing: error)
                }
            }
            continuation.onTermination = { _ in task.cancel() }
        }
    }
}
