import SwiftUI

struct SettingsView: View {
    @State private var apiKey: String = AppSettings.loadAPIKey() ?? ""
    @State private var statusText: String?

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Text("Anthropic APIキー")
                .font(.headline)

            Text("マスコットとの会話にはClaude APIを使用します。[Anthropic Console](https://console.anthropic.com/settings/keys) で発行したAPIキーを入力してください。キーはmacOSのキーチェーンに安全に保存されます。")
                .font(.callout)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)

            SecureField("sk-ant-…", text: $apiKey)
                .textFieldStyle(.roundedBorder)

            HStack {
                Button("保存") {
                    let trimmed = apiKey.trimmingCharacters(in: .whitespacesAndNewlines)
                    if AppSettings.saveAPIKey(trimmed) {
                        statusText = trimmed.isEmpty ? "APIキーを削除しました" : "保存しました ✓"
                    } else {
                        statusText = "キーチェーンへの保存に失敗しました"
                    }
                }
                .keyboardShortcut(.defaultAction)

                if let statusText {
                    Text(statusText)
                        .font(.callout)
                        .foregroundStyle(.secondary)
                }

                Spacer()
            }

            Divider()

            Text("環境変数 `ANTHROPIC_API_KEY` が設定されている場合は、キーチェーンに保存がなければそちらが使われます。")
                .font(.caption)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
        }
        .padding(20)
        .frame(width: 420)
    }
}
