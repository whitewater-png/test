import AppKit
import Combine
import SwiftUI

enum MascotMood {
    case idle
    case thinking
    case talking
}

/// デスクトップに住むマスコット。
/// ユーザーが画像を設定していればそれを、なければ図形で描いたまんまるキャラを表示する。
struct MascotView: View {
    let mood: MascotMood

    @State private var isBobbing = false
    @State private var isBlinking = false
    @State private var customImage: NSImage?

    var body: some View {
        Group {
            if let image = customImage {
                imageMascot(image)
            } else {
                drawnMascot
            }
        }
        .onAppear {
            reloadImage()
            isBobbing = true
        }
        // 設定画面で画像や透過設定が変わったら反映する
        .onReceive(NotificationCenter.default.publisher(for: MascotImageStore.didChangeNotification)) { _ in
            reloadImage()
        }
        .task {
            await blinkLoop()
        }
    }

    private func reloadImage() {
        customImage = MascotImageStore.loadProcessedImage()
    }

    // MARK: - ユーザー画像のマスコット

    private func imageMascot(_ image: NSImage) -> some View {
        ZStack {
            // 影(接地感)
            Ellipse()
                .fill(Color.black.opacity(0.18))
                .frame(width: 120, height: 20)
                .offset(y: 120)
                .blur(radius: 3)

            Image(nsImage: image)
                .resizable()
                .scaledToFit()
                .frame(width: 190, height: 250)
                .offset(y: isBobbing ? -5 : 5)
                .animation(
                    .easeInOut(duration: 1.8).repeatForever(autoreverses: true),
                    value: isBobbing
                )

            if mood == .thinking {
                ThinkingDotsView()
                    .offset(x: 60, y: -120)
            }
        }
        .frame(width: 200, height: 260)
    }

    // MARK: - 図形で描いたマスコット(画像未設定時)

    private var drawnMascot: some View {
        ZStack {
            // 影(接地感)
            Ellipse()
                .fill(Color.black.opacity(0.18))
                .frame(width: 110, height: 20)
                .offset(y: 66)
                .blur(radius: 3)

            ZStack {
                // 体
                Ellipse()
                    .fill(
                        LinearGradient(
                            colors: [
                                Color(red: 1.0, green: 0.93, blue: 0.86),
                                Color(red: 1.0, green: 0.80, blue: 0.70),
                            ],
                            startPoint: .top,
                            endPoint: .bottom
                        )
                    )
                    .frame(width: 130, height: 120)
                    .overlay(
                        Ellipse()
                            .stroke(Color(red: 0.85, green: 0.55, blue: 0.45).opacity(0.6), lineWidth: 2)
                    )
                    .shadow(color: .black.opacity(0.12), radius: 6, y: 4)

                // 顔
                VStack(spacing: 8) {
                    HStack(spacing: 34) {
                        eye
                        eye
                    }
                    mouth
                }
                .offset(y: -2)

                // ほっぺ
                HStack(spacing: 78) {
                    cheek
                    cheek
                }
                .offset(y: 16)

                // 考え中のインジケーター
                if mood == .thinking {
                    ThinkingDotsView()
                        .offset(x: 54, y: -60)
                }
            }
            .offset(y: isBobbing ? -5 : 5)
            .animation(
                .easeInOut(duration: 1.8).repeatForever(autoreverses: true),
                value: isBobbing
            )
        }
        .frame(width: 200, height: 260)
    }

    private var eye: some View {
        Capsule()
            .fill(Color(red: 0.25, green: 0.18, blue: 0.16))
            .frame(width: 12, height: isBlinking ? 3 : 16)
            .animation(.easeInOut(duration: 0.08), value: isBlinking)
    }

    private var cheek: some View {
        Ellipse()
            .fill(Color(red: 1.0, green: 0.6, blue: 0.6).opacity(0.45))
            .frame(width: 18, height: 10)
    }

    @ViewBuilder
    private var mouth: some View {
        switch mood {
        case .talking:
            // 話し中: 開いた口
            Ellipse()
                .fill(Color(red: 0.55, green: 0.28, blue: 0.28))
                .frame(width: 16, height: 12)
        case .thinking:
            // 考え中: すぼめた口
            Circle()
                .fill(Color(red: 0.55, green: 0.28, blue: 0.28))
                .frame(width: 7, height: 7)
        case .idle:
            // 通常: にっこり
            SmileShape()
                .stroke(
                    Color(red: 0.45, green: 0.25, blue: 0.22),
                    style: StrokeStyle(lineWidth: 2.5, lineCap: .round)
                )
                .frame(width: 24, height: 10)
        }
    }

    private func blinkLoop() async {
        while !Task.isCancelled {
            let interval = Double.random(in: 2.0...5.0)
            try? await Task.sleep(nanoseconds: UInt64(interval * 1_000_000_000))
            guard !Task.isCancelled else { return }
            isBlinking = true
            try? await Task.sleep(nanoseconds: 120_000_000)
            isBlinking = false
        }
    }
}

/// にっこり口の曲線
struct SmileShape: Shape {
    func path(in rect: CGRect) -> Path {
        var path = Path()
        path.move(to: CGPoint(x: rect.minX, y: rect.minY))
        path.addQuadCurve(
            to: CGPoint(x: rect.maxX, y: rect.minY),
            control: CGPoint(x: rect.midX, y: rect.maxY + rect.height)
        )
        return path
    }
}

/// 「考え中…」の点々アニメーション
struct ThinkingDotsView: View {
    @State private var phase = 0

    var body: some View {
        HStack(spacing: 4) {
            ForEach(0..<3, id: \.self) { index in
                Circle()
                    .fill(Color.secondary)
                    .frame(width: 6, height: 6)
                    .opacity(phase == index ? 1.0 : 0.3)
            }
        }
        .padding(8)
        .background(.thinMaterial, in: Capsule())
        .task {
            while !Task.isCancelled {
                try? await Task.sleep(nanoseconds: 300_000_000)
                phase = (phase + 1) % 3
            }
        }
    }
}
