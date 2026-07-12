// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "DesktopMate",
    platforms: [
        .macOS(.v13)
    ],
    targets: [
        .executableTarget(
            name: "DesktopMate",
            path: "Sources/DesktopMate"
        )
    ]
)
