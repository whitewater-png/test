import Foundation

/// ユーザーが選んだVRMモデルの保存/参照を担う。
/// 画像と同じ Application Support/DesktopMate 配下に model.vrm として保存する。
enum VRMStore {
    /// VRMが変わったことを MascotView に伝える通知
    static let didChangeNotification = Notification.Name("VRMDidChange")

    /// viewer.html / model.vrm を置くディレクトリ(WKWebViewの読み取り許可範囲)
    static var directory: URL {
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)
            .first ?? FileManager.default.temporaryDirectory
        let dir = base.appendingPathComponent("DesktopMate", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir
    }

    static var modelURL: URL {
        directory.appendingPathComponent("model.vrm")
    }

    static func hasVRM() -> Bool {
        FileManager.default.fileExists(atPath: modelURL.path)
    }

    /// VRMファイルを取り込む(原本をコピー)。
    static func importVRM(from url: URL) throws {
        let data = try Data(contentsOf: url)
        try data.write(to: modelURL, options: .atomic)
        NotificationCenter.default.post(name: didChangeNotification, object: nil)
    }

    static func clear() {
        try? FileManager.default.removeItem(at: modelURL)
        NotificationCenter.default.post(name: didChangeNotification, object: nil)
    }
}
