import Foundation

/// 対応するAIプロバイダー。
/// APIキー・モデル・エンドポイントの違いを吸収する。
enum LLMProvider: String, CaseIterable, Identifiable {
    case anthropic
    case openai
    case google

    var id: String { rawValue }

    var displayName: String {
        switch self {
        case .anthropic: return "Anthropic (Claude)"
        case .openai: return "OpenAI (GPT)"
        case .google: return "Google (Gemini)"
        }
    }

    /// 既定のモデルID(ユーザーが設定画面で変更可能)
    var defaultModel: String {
        switch self {
        case .anthropic: return "claude-opus-4-8"
        case .openai: return "gpt-4o"
        case .google: return "gemini-2.0-flash"
        }
    }

    /// Keychainに保存する際のアカウント名(プロバイダーごとに分ける)
    var keychainAccount: String { "api-key-\(rawValue)" }

    /// APIキーが未設定のときに参照する環境変数名(先頭が優先)
    var envVarNames: [String] {
        switch self {
        case .anthropic: return ["ANTHROPIC_API_KEY"]
        case .openai: return ["OPENAI_API_KEY"]
        case .google: return ["GEMINI_API_KEY", "GOOGLE_API_KEY"]
        }
    }

    var keyPlaceholder: String {
        switch self {
        case .anthropic: return "sk-ant-…"
        case .openai: return "sk-…"
        case .google: return "AIza…"
        }
    }

    /// 設定画面に表示するAPIキーの取得先案内
    var keyHelp: String {
        switch self {
        case .anthropic:
            return "Anthropic Console (console.anthropic.com/settings/keys) で発行。"
        case .openai:
            return "OpenAI Platform (platform.openai.com/api-keys) で発行。"
        case .google:
            return "Google AI Studio (aistudio.google.com/apikey) で発行。"
        }
    }

    /// このプロバイダー用のバックエンドを生成する
    func makeBackend() -> ChatBackend {
        switch self {
        case .anthropic: return AnthropicBackend()
        case .openai: return OpenAIBackend()
        case .google: return GoogleBackend()
        }
    }
}
