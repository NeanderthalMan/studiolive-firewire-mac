// SPDX-License-Identifier: GPL-2.0-or-later
//
// menubar/Status.swift: what the icon and dropdown say, from the driver's
// status dictionary and the aggregate's drift state. Compiled together with
// Status.swift; the exit status is the failure count.

import Foundation

var failures = 0, checks = 0
func check(_ cond: Bool, _ what: String, line: Int = #line) {
    checks += 1
    if !cond { failures += 1; FileHandle.standardError.write("FAIL test_menu.swift:\(line): \(what)\n".data(using: .utf8)!) }
}

let idle: [String: Any] = ["console": "StudioLive", "channels": 40, "sample rate": 44100, "running": 0,
                           "clients": 0, "read underruns": 0]
var live = idle
live["running"] = 1; live["clients"] = 1
live["frames"] = 44100 * 3725; live["frames lost"] = 0; live["DBC breaks"] = 0; live["worst call-back gap us"] = 4200

// Parsing
let s = DriverStatus(live)
check(s.console == "StudioLive" && s.channels == 40 && s.sampleRate == 44100, "identity")
check(s.running && s.clients == 1 && s.frames == Int64(44100 * 3725), "stream fields")
check(s.worstCallbackGapMs == 4.2, "gap in ms: \(String(describing: s.worstCallbackGapMs))")
check(DriverStatus(idle).frames == nil && DriverStatus(idle).worstCallbackGapMs == nil, "idle has no stream fields")

// Health
let t0 = Date(timeIntervalSince1970: 1_000_000)
var h = HealthTracker(window: 60)
check(h.update(nil, aggregate: nil, now: t0) == .absent, "no console is absent")
check(h.update(DriverStatus(idle), aggregate: nil, now: t0) == .idle, "present, not running is idle")
check(h.update(s, aggregate: nil, now: t0) == .streaming, "running is streaming")

var lost = s; lost.framesLost = 8
if case .trouble(let why) = h.update(lost, aggregate: nil, now: t0.addingTimeInterval(2)) {
    check(why.first?.contains("8 frames lost") ?? false, "names the loss: \(why)")
} else { check(false, "a loss is trouble") }
check(h.update(lost, aggregate: nil, now: t0.addingTimeInterval(30)) != .streaming, "still trouble 30 s later")
check(h.update(lost, aggregate: nil, now: t0.addingTimeInterval(70)) == .streaming, "clears after the window")

var under = lost; under.readUnderruns = 3
if case .trouble(let why) = h.update(under, aggregate: nil, now: t0.addingTimeInterval(72)) {
    check(why.first?.contains("3 frames read before they arrived") ?? false, "names the underrun: \(why)")
} else { check(false, "an underrun is trouble") }

// A new stream starts its counters over: not trouble, and not "negative loss".
h = HealthTracker(window: 60)
_ = h.update(lost, aggregate: nil, now: t0)
check(h.update(s, aggregate: nil, now: t0.addingTimeInterval(2)) == .streaming, "counters falling is a new stream")

// The aggregate's drift state is trouble even while idle: it will click.
h = HealthTracker()
let bad = AggregateStatus(name: "StudioLive + PreSonus AudioBox iOne", uncompensated: ["PreSonus AudioBox iOne"])
if case .trouble(let why) = h.update(DriverStatus(idle), aggregate: bad, now: t0) {
    check(why.first?.contains("not drift-compensated") ?? false, "names the device: \(why)")
} else { check(false, "drift off is trouble") }
let good = AggregateStatus(name: bad.name, uncompensated: [])
check(h.update(s, aggregate: good, now: t0) == .streaming, "drift on is fine")

// The dropdown
let lines = describe(s, good, .streaming)
check(lines.first == "StudioLive: 40 channels at 44.1 kHz", "header: \(lines.first ?? "")")
check(lines.contains("Streaming to 1 app"), "clients line: \(lines)")
check(lines.contains("Stream up 1:02:05"), "uptime: \(lines)")
check(lines.contains("Worst call-back gap 4.2 ms"), "gap line: \(lines)")
check(lines.contains("StudioLive + PreSonus AudioBox iOne: drift compensation on"), "aggregate line: \(lines)")
check(describe(nil, nil, .absent) == ["No StudioLive console on the FireWire bus"], "absent")
var at48 = DriverStatus(idle); at48.sampleRate = 48000
check(describe(at48, nil, .idle).first == "StudioLive: 40 channels at 48 kHz", "whole kHz")

print("menu".padding(toLength: 10, withPad: " ", startingAt: 0) + " \(checks) checks, \(failures) failed")
exit(Int32(min(failures, 100)))
