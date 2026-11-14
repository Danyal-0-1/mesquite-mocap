// =========================================================================
//  SCENARIOS  --  Checks/Scenarios.swift
//
//  Scripted takes, run through the app's real pipeline, so the exact lines
//  the phone would send can be replayed into the hub mock and the page
//  (../tools/mock_hub.js --replay). Each line of a scenario file is JSON:
//    {"t": s, "wire": "<message>" | null, "naive": "<message without the
//     zero nudge>" | null, "calibrate": bool,
//     "truth": {"d": [right, up, back] m since the T-pose, "q": [x,y,z,w]
//               pelvis turn since the T-pose, in the wearer's T-pose axes}}
// =========================================================================
import Foundation

struct ScenarioLine: Encodable {
    var t: Double
    var wire: String?
    var naive: String?
    var calibrate: Bool
    var truth: Truth
    struct Truth: Encodable { var d: [Double]; var q: [Double] }
}

/// The page-facing story of a take: T-pose, Zero, page calibration, then
/// motion. `motion(s)` gives the wearer at time s (seconds after the start).
func runScenario(tree: PageTree, lever: Vec3?, mount: Mount, seconds: Double,
                 motion: (Double, Wearer) -> Wearer) -> [ScenarioLine] {
    let start = standing(heading: 1.1)
    let world = ARWorld(cameraInRoom: .yaw(-0.5), at: Vec3(0.6, 1.3, -0.2))  // session began in the hand
    var pipeline = PosePipeline(frame: HipsFrame(lever: lever ?? mount.leverCam, hipHeight: 1.0, facing: .forward),
                                tree: tree, rateHz: 30)
    var lines: [ScenarioLine] = []
    var calibrated = false
    for i in 0...Int(seconds * 60) {
        let t = Double(i) / 60
        if abs(t - 1.0) < 1e-9 { pipeline.requestZero() }
        let w = motion(t, start)
        let c = world.camera(w, mount)
        let out = pipeline.process(FrameInput(time: 100 + t, camPosition: c.p, camRotation: c.q, tracking: .normal))
        var wire: String?
        var naive: String?
        if let m = out.message {
            pipeline.markSent(100 + t)
            wire = m.text
            if let rep = out.reported, m.hasPosition {
                let wp = tree.wire(position: rep.position, orientation: rep.orientation)
                naive = naiveText(m, wp.position)
            }
        }
        // Truth in the wearer's T-pose axes: right, up, back.
        let d = start.pelvis.conjugate.rotate(w.hip - start.hip)
        let q = (start.pelvis.conjugate * w.pelvis).normalized
        let cal = !calibrated && t >= 1.5
        if cal { calibrated = true }
        lines.append(ScenarioLine(t: t, wire: wire, naive: naive, calibrate: cal,
                                  truth: .init(d: [d.x, d.y, d.z], q: [q.x, q.y, q.z, q.w])))
    }
    return lines
}

/// The same message with plain rounding of the position: what a sender that
/// ignores the page's truthiness guard would put on the wire.
private func naiveText(_ m: HubMessage.Encoded, _ p: Vec3) -> String? {
    guard let pd = HubMessage.levels[m.level].p, let close = m.text.range(of: ",\"px\":") else { return nil }
    let head = m.text[..<close.lowerBound]
    guard let px = HubMessage.fixed(p.x, pd), let py = HubMessage.fixed(p.y, pd), let pz = HubMessage.fixed(p.z, pd)
    else { return nil }
    return head + ",\"px\":\(px),\"py\":\(py),\"pz\":\(pz)}"
}

/// The take the page check uses for trees. Zero at 1 s and the page's
/// T-pose at 1.5 s, then still until 3 s so the page's smoothing settles:
/// walk 1 m forward (3-5 s), lean 20 degrees forward (6-7 s), hold, stand up
/// (8-9 s), turn 90 degrees left on the spot (9-11 s), hold to 12 s.
func walkLeanTurn(_ t: Double, _ s: Wearer) -> Wearer {
    var w = s
    w.hip = s.hip + s.pelvis.rotate(Vec3(0, 0, -1.0 * smoothstep((t - 3) / 2)))
    let lean = rad(-20) * (smoothstep(t - 6) - smoothstep(t - 8))
    let turn = rad(90) * smoothstep((t - 9) / 2)
    w.pelvis = .yaw(turn) * s.pelvis * Quat(axis: Vec3(1, 0, 0), angle: lean)
    return w
}

/// Half a turn on the spot (3-7 s), after the T-pose.
func halfTurn(_ t: Double, _ s: Wearer) -> Wearer {
    var w = s
    w.pelvis = .yaw(.pi * smoothstep((t - 3) / 4)) * s.pelvis
    return w
}

/// Small sway around the Zero point: the coordinates cross 0 again and again.
func swayAtOrigin(_ t: Double, _ s: Wearer) -> Wearer {
    var w = s
    let a = smoothstep(t - 3)
    w.hip = s.hip + s.pelvis.rotate(Vec3(0.02 * sin(2.1 * t) * a, 0.004 * sin(3.3 * t) * a, 0.01 * sin(1.3 * t) * a))
    return w
}

func writeScenarios(to dir: URL) throws {
    try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
    let enc = JSONEncoder()
    enc.outputFormatting = [.sortedKeys, .withoutEscapingSlashes]
    func save(_ name: String, _ lines: [ScenarioLine]) throws {
        var text = ""
        for l in lines { text += String(decoding: try enc.encode(l), as: UTF8.self) + "\n" }
        try Data(text.utf8).write(to: dir.appendingPathComponent(name + ".jsonl"))
        report.note("wrote \(name).jsonl (\(lines.filter { $0.wire != nil }.count) messages)")
    }
    let m = Mount.frontPortrait()
    for tree in PageTree.allCases {
        try save("walk_lean_turn_\(tree.rawValue)", runScenario(tree: tree, lever: nil, mount: m, seconds: 12,
                                                                motion: walkLeanTurn))
    }
    try save("half_turn_lever_measured", runScenario(tree: .velvetFamily, lever: nil, mount: m, seconds: 8,
                                                     motion: halfTurn))
    try save("half_turn_lever_zero", runScenario(tree: .velvetFamily, lever: .zero, mount: m, seconds: 8,
                                                 motion: halfTurn))
    try save("sway_at_origin", runScenario(tree: .velvetFamily, lever: nil, mount: m, seconds: 12,
                                           motion: swayAtOrigin))
}
