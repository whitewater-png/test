import AppKit
import SwiftUI
import UniformTypeIdentifiers

struct SettingsView: View {
    @State private var provider: LLMProvider = AppSettings.selectedProvider
    @State private var apiKey: String = ""
    @State private var model: String = ""
    @State private var statusText: String?

    @State private var hasCustomImage = MascotImageStore.hasCustomImage()
    @State private var removeWhiteBackground = MascotImageStore.removeWhiteBackground

    @State private var hasVRM = VRMStore.hasVRM()

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 18) {
                providerSection

                Divider()

                imageSection

                Divider()

                vrmSection
            }
            .padding(20)
        }
        .frame(width: 460, height: 620)
        .onAppear { loadForProvider(provider) }
    }

    // MARK: - AIプロバイダー設定

    private var providerSection: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("AIプロバイダー設定")
                .font(.headline)

            Picker("プロバイダー", selection: $provider) {
                ForEach(LLMProvider.allCases) { provider in
                    Text(provider.displayName).tag(provider)
                }
            }
            .onChange(of: provider) { _, newValue in
                statusText = nil
                loadForProvider(newValue)
            }

            VStack(alignment: .leading, spacing: 6) {
                Text("APIキー").font(.subheadline)
                SecureField(provider.keyPlaceholder, text: $apiKey)
                    .textFieldStyle(.roundedBorder)
                Text(provider.keyHelp)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }

            VStack(alignment: .leading, spacing: 6) {
                Text("モデル").font(.subheadline)
                TextField(provider.defaultModel, text: $model)
                    .textFieldStyle(.roundedBorder)
                Text("空欄にすると既定 (\(provider.defaultModel)) を使います。")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            HStack {
                Button("保存") { save() }
                    .keyboardShortcut(.defaultAction)
                if let statusText {
                    Text(statusText)
                        .font(.callout)
                        .foregroundStyle(.secondary)
                }
                Spacer()
            }
        }
    }

    // MARK: - キャラクター画像

    private var imageSection: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("キャラクター画像")
                .font(.headline)

            Text("お気に入りの画像 (PNG / JPEG) を選ぶと、マスコットがその絵に変わります。")
                .font(.caption)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)

            HStack {
                Button("画像を選択…") { pickImage() }
                if hasCustomImage {
                    Button("デフォルトに戻す") {
                        MascotImageStore.clear()
                        hasCustomImage = false
                    }
                }
            }

            Toggle("白い背景を透過する", isOn: $removeWhiteBackground)
                .onChange(of: removeWhiteBackground) { _, newValue in
                    MascotImageStore.removeWhiteBackground = newValue
                }
            Text("背景が白い一枚絵でも、白地を抜いてキャラクターだけを浮かせます。")
                .font(.caption)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
        }
    }

    // MARK: - 3Dモデル (VRM)

    private var vrmSection: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("3Dモデル (VRM / GLB)")
                .font(.headline)

            Text("VRM/GLB/glTF ファイルを選ぶと、3Dアバターとして表示します(画像より優先)。VRM拡張があればまばたき等も動き、素のGLBは3Dモデルとして表示します。")
                .font(.caption)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)

            HStack {
                Button("モデルを選択…") { pickVRM() }
                if hasVRM {
                    Button("モデルを外す") {
                        VRMStore.clear()
                        hasVRM = false
                    }
                }
            }

            Text("※ 3D描画ライブラリを起動時にインターネットから読み込みます(要ネット接続)。表示位置やサイズはモデルにより調整が必要な場合があります。.gltf は外部ファイル参照があると読み込めないことがあります(自己完結の .glb 推奨)。")
                .font(.caption)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
        }
    }

    // MARK: - アクション

    private func loadForProvider(_ provider: LLMProvider) {
        apiKey = AppSettings.storedKey(for: provider) ?? ""
        model = AppSettings.model(for: provider)
    }

    private func save() {
        AppSettings.selectedProvider = provider
        AppSettings.setModel(model, for: provider)
        let trimmedKey = apiKey.trimmingCharacters(in: .whitespacesAndNewlines)
        if AppSettings.saveAPIKey(trimmedKey, for: provider) {
            statusText = "保存しました ✓"
        } else {
            statusText = "キーチェーンへの保存に失敗しました"
        }
    }

    private func pickImage() {
        let panel = NSOpenPanel()
        panel.allowedContentTypes = [.png, .jpeg, .image]
        panel.allowsMultipleSelection = false
        panel.canChooseDirectories = false
        panel.prompt = "設定"
        guard panel.runModal() == .OK, let url = panel.url else { return }

        do {
            try MascotImageStore.importImage(from: url)
            hasCustomImage = true
            statusText = "キャラクター画像を設定しました ✓"
        } catch {
            statusText = "画像の読み込みに失敗しました"
        }
    }

    private func pickVRM() {
        let panel = NSOpenPanel()
        // .vrm / .glb / .gltf を選べるようにする(解決できた型だけ許可)
        let types = ["vrm", "glb", "gltf"].compactMap { UTType(filenameExtension: $0) }
        if !types.isEmpty {
            panel.allowedContentTypes = types
        }
        panel.allowsMultipleSelection = false
        panel.canChooseDirectories = false
        panel.prompt = "設定"
        guard panel.runModal() == .OK, let url = panel.url else { return }

        do {
            try VRMStore.importVRM(from: url)
            hasVRM = true
            statusText = "3Dモデルを設定しました ✓"
        } catch {
            statusText = "モデルの読み込みに失敗しました"
        }
    }
}
