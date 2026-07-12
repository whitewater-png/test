import AppKit
import SwiftUI

final class AppDelegate: NSObject, NSApplicationDelegate {
    private var mascotWindow: MascotWindow?
    private var statusItem: NSStatusItem?
    private var settingsWindow: NSWindow?

    func applicationDidFinishLaunching(_ notification: Notification) {
        // Dockアイコンなしの常駐アプリとして動作する
        NSApp.setActivationPolicy(.accessory)

        setupStatusItem()

        let window = MascotWindow()
        window.makeKeyAndOrderFront(nil)
        mascotWindow = window

        NSApp.activate()

        if AppSettings.apiKey(for: AppSettings.selectedProvider) == nil {
            openSettings()
        }
    }

    private func setupStatusItem() {
        let item = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
        if let button = item.button {
            button.image = NSImage(
                systemSymbolName: "face.smiling",
                accessibilityDescription: "DesktopMate"
            )
        }

        let menu = NSMenu()
        menu.addItem(
            withTitle: "マスコットを表示 / 非表示",
            action: #selector(toggleMascot),
            keyEquivalent: "m"
        )
        menu.addItem(
            withTitle: "設定 (APIキー)…",
            action: #selector(openSettings),
            keyEquivalent: ","
        )
        menu.addItem(.separator())
        menu.addItem(
            withTitle: "DesktopMateを終了",
            action: #selector(quit),
            keyEquivalent: "q"
        )
        for menuItem in menu.items {
            menuItem.target = self
        }
        item.menu = menu
        statusItem = item
    }

    @objc private func toggleMascot() {
        guard let window = mascotWindow else { return }
        if window.isVisible {
            window.orderOut(nil)
        } else {
            window.makeKeyAndOrderFront(nil)
            NSApp.activate()
        }
    }

    @objc func openSettings() {
        if let window = settingsWindow {
            window.makeKeyAndOrderFront(nil)
            NSApp.activate()
            return
        }

        let hosting = NSHostingController(rootView: SettingsView())
        let window = NSWindow(contentViewController: hosting)
        window.title = "DesktopMate 設定"
        window.styleMask = [.titled, .closable]
        window.setContentSize(NSSize(width: 460, height: 560))
        window.center()
        window.isReleasedWhenClosed = false
        window.makeKeyAndOrderFront(nil)
        NSApp.activate()
        settingsWindow = window
    }

    @objc private func quit() {
        NSApp.terminate(nil)
    }
}
