// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "DesktopMate",
    platforms: [
        .macOS(.v14)
    ],
    targets: [
        .executableTarget(
            name: "DesktopMate",
            path: "Sources/DesktopMate",
            resources: [
                // 既定のキャラクター画像を同梱する
                .process("Resources")
            ]
        )
    ]
)
