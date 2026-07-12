import Foundation
import Security

/// アプリ設定の保存/解決。
/// - 選択中プロバイダーと各プロバイダーのモデルは UserDefaults に保存
/// - APIキーはプロバイダーごとに Keychain に保存(未設定なら環境変数を参照)
enum AppSettings {
    private static let service = "com.example.DesktopMate"
    private static let defaults = UserDefaults.standard
    private static let providerKey = "selectedProvider"
    private static let modelKeyPrefix = "model."

    // MARK: - プロバイダー選択

    static var selectedProvider: LLMProvider {
        get { LLMProvider(rawValue: defaults.string(forKey: providerKey) ?? "") ?? .anthropic }
        set { defaults.set(newValue.rawValue, forKey: providerKey) }
    }

    // MARK: - モデル

    static func model(for provider: LLMProvider) -> String {
        let stored = defaults.string(forKey: modelKeyPrefix + provider.rawValue)?
            .trimmingCharacters(in: .whitespacesAndNewlines)
        if let stored, !stored.isEmpty { return stored }
        return provider.defaultModel
    }

    static func setModel(_ model: String, for provider: LLMProvider) {
        let trimmed = model.trimmingCharacters(in: .whitespacesAndNewlines)
        defaults.set(trimmed, forKey: modelKeyPrefix + provider.rawValue)
    }

    // MARK: - APIキーの解決

    /// 実際に使うキー: Keychain → 環境変数 の順で解決する。
    static func apiKey(for provider: LLMProvider) -> String? {
        if let stored = loadKey(account: provider.keychainAccount), !stored.isEmpty {
            return stored
        }
        for name in provider.envVarNames {
            if let value = ProcessInfo.processInfo.environment[name], !value.isEmpty {
                return value
            }
        }
        return nil
    }

    /// 設定画面表示用: Keychainに保存済みのキーだけを返す(環境変数は含めない)。
    static func storedKey(for provider: LLMProvider) -> String? {
        loadKey(account: provider.keychainAccount)
    }

    @discardableResult
    static func saveAPIKey(_ value: String, for provider: LLMProvider) -> Bool {
        saveKey(value, account: provider.keychainAccount)
    }

    // MARK: - Keychain プリミティブ

    private static func loadKey(account: String) -> String? {
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: account,
            kSecReturnData as String: true,
            kSecMatchLimit as String: kSecMatchLimitOne,
        ]

        var result: AnyObject?
        let status = SecItemCopyMatching(query as CFDictionary, &result)
        guard status == errSecSuccess,
              let data = result as? Data,
              let value = String(data: data, encoding: .utf8)
        else {
            return nil
        }
        return value
    }

    @discardableResult
    private static func saveKey(_ value: String, account: String) -> Bool {
        deleteKey(account: account)

        let trimmed = value.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return true }

        let attributes: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: account,
            kSecValueData as String: Data(trimmed.utf8),
            // ロック解除中のみアクセス可・iCloudキーチェーンに同期しない
            kSecAttrAccessible as String: kSecAttrAccessibleWhenUnlockedThisDeviceOnly,
        ]
        return SecItemAdd(attributes as CFDictionary, nil) == errSecSuccess
    }

    @discardableResult
    private static func deleteKey(account: String) -> Bool {
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: account,
        ]
        let status = SecItemDelete(query as CFDictionary)
        return status == errSecSuccess || status == errSecItemNotFound
    }
}
