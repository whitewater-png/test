import AppKit

/// ユーザーが選んだキャラクター画像の保存/読み込みを担う。
///
/// - 画像は Application Support/DesktopMate/mascot-source に原本コピーして保存する
/// - 表示時に「白背景を透過」オプションがオンなら白地を抜いて浮遊マスコットにする
/// - 画像が未設定なら nil を返し、MascotView は図形描画にフォールバックする
enum MascotImageStore {
    /// 画像や設定が変わったことを MascotView に伝える通知
    static let didChangeNotification = Notification.Name("MascotImageDidChange")

    private static let removeWhiteKey = "mascotRemoveWhiteBackground"

    /// 白背景を透過するか(既定: オン)
    static var removeWhiteBackground: Bool {
        get { UserDefaults.standard.object(forKey: removeWhiteKey) as? Bool ?? true }
        set {
            UserDefaults.standard.set(newValue, forKey: removeWhiteKey)
            NotificationCenter.default.post(name: didChangeNotification, object: nil)
        }
    }

    // MARK: - 保存先

    private static var directory: URL {
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)
            .first ?? FileManager.default.temporaryDirectory
        let dir = base.appendingPathComponent("DesktopMate", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir
    }

    private static var sourceURL: URL {
        directory.appendingPathComponent("mascot-source")
    }

    // MARK: - 操作

    static func hasCustomImage() -> Bool {
        FileManager.default.fileExists(atPath: sourceURL.path)
    }

    /// 画像ファイルを取り込む(原本をコピー)。読み込めない形式なら投げる。
    static func importImage(from url: URL) throws {
        let data = try Data(contentsOf: url)
        guard NSImage(data: data) != nil else {
            throw BackendError(message: "画像を読み込めませんでした")
        }
        try data.write(to: sourceURL, options: .atomic)
        NotificationCenter.default.post(name: didChangeNotification, object: nil)
    }

    static func clear() {
        try? FileManager.default.removeItem(at: sourceURL)
        NotificationCenter.default.post(name: didChangeNotification, object: nil)
    }

    /// 表示用の画像を返す。
    /// ユーザーが選んだ画像があればそれ(設定に応じて白背景を透過)、
    /// なければ同梱の既定キャラクター画像を返す。どちらも無ければ nil。
    static func loadProcessedImage() -> NSImage? {
        if hasCustomImage(), let image = NSImage(contentsOf: sourceURL) {
            return removeWhiteBackground ? imageByRemovingWhite(image) : image
        }
        return bundledDefaultImage()
    }

    /// アプリに同梱した既定キャラクター画像(透過PNG)。
    static func bundledDefaultImage() -> NSImage? {
        guard let url = Bundle.module.url(forResource: "mascot-default", withExtension: "png") else {
            return nil
        }
        return NSImage(contentsOf: url)
    }

    // MARK: - 白背景の透過処理

    /// ほぼ白のピクセルを透明にする。白地の一枚絵をそのまま浮遊マスコットにできる。
    private static func imageByRemovingWhite(_ image: NSImage) -> NSImage {
        guard let cgImage = image.cgImage(forProposedRect: nil, context: nil, hints: nil) else {
            return image
        }
        let width = cgImage.width
        let height = cgImage.height
        guard width > 0, height > 0 else { return image }

        let bytesPerRow = width * 4
        var pixels = [UInt8](repeating: 0, count: bytesPerRow * height)
        let colorSpace = CGColorSpaceCreateDeviceRGB()
        guard let context = CGContext(
            data: &pixels,
            width: width,
            height: height,
            bitsPerComponent: 8,
            bytesPerRow: bytesPerRow,
            space: colorSpace,
            bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue
        ) else {
            return image
        }

        context.draw(cgImage, in: CGRect(x: 0, y: 0, width: width, height: height))

        // R,G,B すべてが 240 以上のピクセルを、白に近いほど透明にする(240→不透明, 255→透明)。
        var index = 0
        while index < pixels.count {
            let r = pixels[index]
            let g = pixels[index + 1]
            let b = pixels[index + 2]
            if r >= 240, g >= 240, b >= 240 {
                let minChannel = min(r, min(g, b))
                // 255 - minChannel は 0...15。×17 で 0...255 に伸ばす。
                let alpha = UInt8(max(0, min(255, Int(255 - minChannel) * 17)))
                pixels[index + 3] = alpha
                // premultipliedLast なので RGB も alpha に合わせて縮める
                if alpha < 255 {
                    pixels[index] = UInt8(Int(r) * Int(alpha) / 255)
                    pixels[index + 1] = UInt8(Int(g) * Int(alpha) / 255)
                    pixels[index + 2] = UInt8(Int(b) * Int(alpha) / 255)
                }
            }
            index += 4
        }

        guard let outputCG = context.makeImage() else { return image }
        return NSImage(cgImage: outputCG, size: NSSize(width: width, height: height))
    }
}
