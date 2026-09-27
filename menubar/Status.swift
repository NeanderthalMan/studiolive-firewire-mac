// SPDX-License-Identifier: GPL-2.0-or-later
//
// What the menu bar says, worked out from what Core Audio reports. No AppKit
// and no Core Audio here, so tests/test_menu.swift can drive it directly.

import Foundation

/// The driver's 'slst' device property, as published by driver/plugin.c.
struct DriverStatus: Equatable {
    var console = ""
    var channels = 0
    var sampleRate = 0
    var running = false
    var clients = 0
    var readUnderruns: Int64 = 0
    /// Present only while a stream is open.
    var frames: Int64?
    var dbcBreaks: Int64 = 0
    var framesLost: Int64 = 0
    var worstCallbackGapMs: Double?

    init() {}

    init(_ d: [String: Any]) {
        func int(_ k: String) -> Int64? { (d[k] as? NSNumber)?.int64Value }
        console = d["console"] as? String ?? ""
        channels = Int(int("channels") ?? 0)
        sampleRate = Int(int("sample rate") ?? 0)
        running = (int("running") ?? 0) != 0
        clients = Int(int("clients") ?? 0)
        readUnderruns = int("read underruns") ?? 0
        frames = int("frames")
        dbcBreaks = int("DBC breaks") ?? 0
        framesLost = int("frames lost") ?? 0
        worstCallbackGapMs = int("worst call-back gap us").map { Double($0) / 1000 }
    }
}

/// The StudioLive aggregate, if there is one: the drift state of each sub-device
/// other than the console, which is the clock.
struct AggregateStatus: Equatable {
    var name: String
    var uncompensated: [String]
}

enum Health: Equatable {
    case absent                 // no console on the bus, or the driver is not loaded
    case idle                   // console present, nothing recording from it
    case streaming
    case trouble([String])      // streaming, or set up, with something wrong
}

/// Remembers the counters between polls, so "trouble" means audio went wrong
/// recently rather than at some point since the stream opened. Counters that
/// fall mean a new stream: that is a fresh start, not an improvement.
final class HealthTracker {
    /// How long a loss keeps the icon in the trouble state.
    let window: TimeInterval
    private var last: DriverStatus?
    private var lastBad: (Date, String)?

    init(window: TimeInterval = 60) { self.window = window }

    func update(_ status: DriverStatus?, aggregate: AggregateStatus?, now: Date = Date()) -> Health {
        guard let s = status else { last = nil; lastBad = nil; return .absent }
        if let p = last {
            if s.framesLost > p.framesLost {
                lastBad = (now, "\(s.framesLost - p.framesLost) frames lost on the FireWire stream")
            } else if s.readUnderruns > p.readUnderruns {
                lastBad = (now, "\(s.readUnderruns - p.readUnderruns) frames read before they arrived")
            }
        }
        last = s

        var problems: [String] = []
        if let (at, what) = lastBad, now.timeIntervalSince(at) < window { problems.append(what) }
        if let a = aggregate, !a.uncompensated.isEmpty {
            problems.append("\(a.uncompensated.joined(separator: ", ")) is not drift-compensated: expect a click every minute or two")
        }
        if !problems.isEmpty { return .trouble(problems) }
        return s.running ? .streaming : .idle
    }
}

/// The lines of the dropdown's status section, and of "Copy Diagnostics".
func describe(_ s: DriverStatus?, _ a: AggregateStatus?, _ h: Health) -> [String] {
    guard let s = s else { return ["No StudioLive console on the FireWire bus"] }
    let khz = s.sampleRate % 1000 == 0 ? "\(s.sampleRate / 1000)" : String(format: "%.1f", Double(s.sampleRate) / 1000)
    var lines = ["\(s.console.isEmpty ? "StudioLive" : s.console): \(s.channels) channels at \(khz) kHz"]
    switch h {
    case .absent: break
    case .idle: lines.append("Idle — no app is recording from it")
    case .streaming: lines.append("Streaming to \(s.clients == 1 ? "1 app" : "\(s.clients) apps")")
    case .trouble(let why): lines.append(s.running ? "Streaming, with trouble:" : "Trouble:"); lines += why.map { "  " + $0 }
    }
    if let f = s.frames, s.sampleRate > 0 {
        let secs = Int(f / Int64(s.sampleRate))
        lines.append(String(format: "Stream up %d:%02d:%02d", secs / 3600, secs / 60 % 60, secs % 60))
    }
    lines.append("Frames lost \(s.framesLost) · read underruns \(s.readUnderruns)")
    if let g = s.worstCallbackGapMs { lines.append(String(format: "Worst call-back gap %.1f ms", g)) }
    if let a = a {
        lines.append(a.uncompensated.isEmpty ? "\(a.name): drift compensation on"
                                             : "\(a.name): drift compensation OFF")
    }
    return lines
}
