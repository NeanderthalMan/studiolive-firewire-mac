// SPDX-License-Identifier: GPL-2.0-or-later
//
// StudioLive Menu — a menu bar icon that says whether the console driver is
// there and streaming, with a dropdown of status and a few fixes.
//
// The driver itself lives inside Core Audio's driver service, where no app can
// reach it except through Core Audio, so everything here is read that way: the
// device list, the driver's custom 'slst' status property, and the aggregate's
// sub-devices. Polled every two seconds; the dropdown is rebuilt as it opens.

import AppKit
import CoreAudio
import ServiceManagement

// MARK: Core Audio

private func prop<T>(_ obj: AudioObjectID, _ sel: AudioObjectPropertySelector, _ initial: T) -> T? {
    var addr = AudioObjectPropertyAddress(mSelector: sel, mScope: kAudioObjectPropertyScopeGlobal,
                                          mElement: kAudioObjectPropertyElementMain)
    var v = initial
    var size = UInt32(MemoryLayout<T>.size)
    let r = withUnsafeMutablePointer(to: &v) { AudioObjectGetPropertyData(obj, &addr, 0, nil, &size, $0) }
    return r == noErr ? v : nil
}

private func list(_ obj: AudioObjectID, _ sel: AudioObjectPropertySelector) -> [AudioObjectID] {
    var addr = AudioObjectPropertyAddress(mSelector: sel, mScope: kAudioObjectPropertyScopeGlobal,
                                          mElement: kAudioObjectPropertyElementMain)
    var size: UInt32 = 0
    guard AudioObjectGetPropertyDataSize(obj, &addr, 0, nil, &size) == noErr, size > 0 else { return [] }
    var ids = [AudioObjectID](repeating: 0, count: Int(size) / MemoryLayout<AudioObjectID>.size)
    return AudioObjectGetPropertyData(obj, &addr, 0, nil, &size, &ids) == noErr ? ids : []
}

private func string(_ obj: AudioObjectID, _ sel: AudioObjectPropertySelector) -> String? {
    prop(obj, sel, Unmanaged<CFString>?.none)??.takeRetainedValue() as String?
}

private let kStatus: AudioObjectPropertySelector = 0x736c_7374   // 'slst', driver/plugin.c

func readDriver() -> DriverStatus? {
    for dev in list(AudioObjectID(kAudioObjectSystemObject), kAudioHardwarePropertyDevices) {
        guard let uid = string(dev, kAudioDevicePropertyDeviceUID), uid.hasPrefix("slfw:"),
              uid != "slfw:aggregate" else { continue }
        guard let plist = prop(dev, kStatus, Unmanaged<CFPropertyList>?.none)??.takeRetainedValue(),
              let d = plist as? [String: Any] else {
            return DriverStatus()           // an older driver without the property: present, nothing more
        }
        return DriverStatus(d)
    }
    return nil
}

func readAggregate() -> AggregateStatus? {
    for dev in list(AudioObjectID(kAudioObjectSystemObject), kAudioHardwarePropertyDevices)
    where string(dev, kAudioDevicePropertyDeviceUID) == "slfw:aggregate" {
        var off: [String] = []
        for sub in list(dev, kAudioObjectPropertyOwnedObjects)
        where prop(sub, kAudioObjectPropertyClass, AudioClassID(0)) == kAudioSubDeviceClassID {
            if string(sub, kAudioDevicePropertyDeviceUID)?.hasPrefix("slfw:") ?? false { continue }
            if prop(sub, kAudioSubDevicePropertyDriftCompensation, UInt32(0)) == 0 {
                off.append(string(sub, kAudioObjectPropertyName) ?? "the output device")
            }
        }
        return AggregateStatus(name: string(dev, kAudioObjectPropertyName) ?? "StudioLive aggregate", uncompensated: off)
    }
    return nil
}

// MARK: The menu

final class Menu: NSObject, NSApplicationDelegate, NSMenuDelegate {
    private let item = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
    private let tracker = HealthTracker()
    private var status: DriverStatus?
    private var aggregate: AggregateStatus?
    private var health = Health.absent

    func applicationDidFinishLaunching(_ n: Notification) {
        let menu = NSMenu()
        menu.delegate = self
        item.menu = menu
        poll()
        Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { [weak self] _ in self?.poll() }
    }

    private func poll() {
        status = readDriver()
        aggregate = status == nil ? nil : readAggregate()
        health = tracker.update(status, aggregate: aggregate)
        let (symbol, tip): (String, String)
        switch health {
        case .absent:    (symbol, tip) = ("waveform.slash", "StudioLive: no console")
        case .idle:      (symbol, tip) = ("waveform", "StudioLive: idle")
        case .streaming: (symbol, tip) = ("waveform.circle.fill", "StudioLive: streaming")
        case .trouble:   (symbol, tip) = ("exclamationmark.triangle.fill", "StudioLive: trouble")
        }
        item.button?.image = NSImage(systemSymbolName: symbol, accessibilityDescription: tip)
        item.button?.image?.isTemplate = true
        item.button?.toolTip = tip
    }

    func menuNeedsUpdate(_ menu: NSMenu) {
        poll()
        menu.removeAllItems()
        for (i, line) in describe(status, aggregate, health).enumerated() {
            let m = NSMenuItem(title: line, action: nil, keyEquivalent: "")
            m.isEnabled = false
            if i == 0 { m.attributedTitle = NSAttributedString(string: line, attributes: [.font: NSFont.boldSystemFont(ofSize: 0)]) }
            menu.addItem(m)
        }
        menu.addItem(.separator())
        if let a = aggregate, !a.uncompensated.isEmpty {
            menu.addItem(action("Turn On Drift Compensation", #selector(fixAggregate)))
        }
        menu.addItem(action("Open Audio MIDI Setup", #selector(openAMS)))
        menu.addItem(action("Copy Diagnostics", #selector(copyDiagnostics)))
        menu.addItem(.separator())
        let login = action("Open at Login", #selector(toggleLogin))
        login.state = SMAppService.mainApp.status == .enabled ? .on : .off
        menu.addItem(login)
        menu.addItem(action("Quit StudioLive Menu", #selector(NSApplication.terminate(_:)), key: "q"))
    }

    private func action(_ title: String, _ sel: Selector, key: String = "") -> NSMenuItem {
        let m = NSMenuItem(title: title, action: sel, keyEquivalent: key)
        m.target = sel == #selector(NSApplication.terminate(_:)) ? NSApp : self
        return m
    }

    /// mkaggregate --sync, bundled beside this app: drift compensation on and
    /// every sub-device at the console's rate — one fix, one place.
    @objc private func fixAggregate() {
        guard let tool = Bundle.main.url(forResource: "mkaggregate", withExtension: nil) else { return }
        let p = Process()
        let out = Pipe()
        p.executableURL = tool
        p.arguments = ["--sync"]
        p.standardOutput = out
        p.standardError = out
        try? p.run()
        p.waitUntilExit()
        let text = String(data: out.fileHandleForReading.readDataToEndOfFile(), encoding: .utf8) ?? ""
        let alert = NSAlert()
        alert.messageText = p.terminationStatus == 0 ? "Drift compensation is on" : "Could not fix the aggregate"
        alert.informativeText = text + "\nIt applies the next time Studio One starts audio: switch its audio device away and back, or restart it."
        NSApp.activate(ignoringOtherApps: true)
        alert.runModal()
        poll()
    }

    @objc private func openAMS() {
        NSWorkspace.shared.open(URL(fileURLWithPath: "/System/Applications/Utilities/Audio MIDI Setup.app"))
    }

    @objc private func copyDiagnostics() {
        let text = describe(status, aggregate, health).joined(separator: "\n")
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(text, forType: .string)
    }

    @objc private func toggleLogin() {
        do {
            if SMAppService.mainApp.status == .enabled { try SMAppService.mainApp.unregister() }
            else { try SMAppService.mainApp.register() }
        } catch {
            let alert = NSAlert(error: error)
            NSApp.activate(ignoringOtherApps: true)
            alert.runModal()
        }
    }
}

let app = NSApplication.shared
let delegate = Menu()
app.delegate = delegate
app.setActivationPolicy(.accessory)     // menu bar only: no Dock icon, no app menu
app.run()
