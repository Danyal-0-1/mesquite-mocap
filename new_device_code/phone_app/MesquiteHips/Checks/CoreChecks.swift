// =========================================================================
//  CORE CHECKS  --  Checks/CoreChecks.swift
//
//  Each group checks one claim the app's design rests on, against the
//  simulator's truth. MATH.md cites these groups by name.
// =========================================================================
import Foundation

// ---------------------------------------------------------------- geometry

func checkGeometry() {
    report.section("geometry: quaternion conventions match three.js and ARKit")
    var r = Rand(1)
    var worstRound = 0.0, worstCompose = 0.0
    for _ in 0..<2000 {
        let q = r.rotation()
        let (c0, c1, c2) = q.columns
        worstRound = max(worstRound, Quat(columns: c0, c1, c2).angle(to: q))
        let a = r.rotation(), b = r.rotation()
        let v = Vec3(r.uniform(-1, 1), r.uniform(-1, 1), r.uniform(-1, 1))
        worstCompose = max(worstCompose, ((a * b).rotate(v) - a.rotate(b.rotate(v))).length)
    }
    report.check(worstRound < 1e-9, "matrix -> quaternion round trip (Shepperd, 2000 rotations)",
                 "worst \(worstRound) rad")
    report.check(worstCompose < 1e-12, "a * b rotates by b first, then a (Hamilton, as three.js)",
                 "worst \(worstCompose)")
    let left = Quat.yaw(rad(90)).rotate(Vec3(0, 0, -1))
    report.check((left - Vec3(-1, 0, 0)).length < 1e-12, "yaw(+90 deg) turns -Z (forward) to -X (left)")
    report.check(abs((Vec3(-1, 0, 0).heading() ?? 0) - .pi / 2) < 1e-12, "heading of -X is +90 deg")
}

// ---------------------------------------------------------------- the wire message

func checkHubMessage() {
    report.section("hub message: the 95-byte JSON line the hub and page accept")
    let cases: [(Double, Int, Bool, String)] = [
        (0.5, 4, false, "0.5"), (1, 4, false, "1"), (-0.00004, 4, false, "0"),
        (-0.00004, 4, true, "-0.0001"), (0, 3, true, "0.001"), (-1.23456, 3, false, "-1.235"),
        (12.3, 2, false, "12.3"), (0.1, 3, false, "0.1"), (-0.5, 4, false, "-0.5"),
        (0.70710678, 4, false, "0.7071"), (99.995, 2, false, "100"), (-0.0004, 3, true, "-0.001"),
    ]
    var bad: [String] = []
    for (v, d, nz, want) in cases {
        let got = HubMessage.fixed(v, d, avoidZero: nz) ?? "nil"
        if got != want { bad.append("\(v)@\(d)\(nz ? "nz" : "") -> \(got), want \(want)") }
    }
    report.check(bad.isEmpty, "number formatting: shortest form, no -0, no exponent, zero nudged",
                 bad.joined(separator: "; "))

    let idle = HubMessage.encode(orientation: .identity, position: Vec3(0, 1, 0))
    let want = #"{"bone":"Hips","x":0,"y":0,"z":0,"w":1,"px":0.001,"py":1,"pz":0.001}"#
    report.check(idle?.text == want, "the Zero pose encodes as expected", idle?.text ?? "nil")

    // The longest message there is: every component negative and 4 digits long.
    let worstQ = Quat(-0.1234, -0.4321, -0.5678, -0.6889).normalized
    let worst = HubMessage.encode(orientation: worstQ, position: Vec3(-9.876, -1.234, -5.678))
    report.check(worst.map { $0.bytes <= 95 && $0.level >= 1 } ?? false,
                 "a worst-case pose falls back a level and still fits",
                 worst.map { "\($0.bytes) bytes, level \($0.level)" } ?? "nil")

    var r = Rand(2)
    var n = 0, over = 0, notJSON = 0, keyErr = 0, zeroPos = 0, precErr = 0, exponent = 0
    var levelCount = [0, 0, 0, 0], maxBytes = 0
    for box in [5.0, 50.0, 500.0] {
        for _ in 0..<60_000 {
            let q = r.rotation()
            let p = Vec3(r.uniform(-box, box), r.uniform(-box / 5, box / 5), r.uniform(-box, box))
            guard let m = HubMessage.encode(orientation: q, position: p) else { over += 1; continue }
            n += 1
            maxBytes = max(maxBytes, m.bytes)
            if m.bytes > 95 { over += 1 }
            levelCount[m.level] += 1
            // Past `{"bone":"Hips"` (14 characters) only numbers remain.
            if m.text.dropFirst(14).contains(where: { $0 == "e" || $0 == "E" }) { exponent += 1 }
            guard let obj = (try? JSONSerialization.jsonObject(with: Data(m.text.utf8))) as? [String: Any] else {
                notJSON += 1; continue
            }
            let keys = Set(obj.keys)
            let expected: Set<String> = m.hasPosition ? ["bone", "x", "y", "z", "w", "px", "py", "pz"]
                                                      : ["bone", "x", "y", "z", "w"]
            if keys != expected || (obj["bone"] as? String) != "Hips" { keyErr += 1 }
            let lv = HubMessage.levels[m.level]
            let qs = 0.5 / pow(10, Double(lv.q)) + 1e-12
            let u = q.normalized
            for (k, v) in [("x", u.x), ("y", u.y), ("z", u.z), ("w", u.w)] {
                if abs(((obj[k] as? NSNumber)?.doubleValue ?? .nan) - v) > qs { precErr += 1 }
            }
            if let pd = lv.p {
                let ps = 1 / pow(10, Double(pd)) + 1e-12      // half a step, or one step if nudged
                for (k, v) in [("px", p.x), ("py", p.y), ("pz", p.z)] {
                    let got = (obj[k] as? NSNumber)?.doubleValue ?? .nan
                    if got == 0 { zeroPos += 1 }
                    if abs(got - v) > ps { precErr += 1 }
                }
            }
        }
    }
    report.check(over == 0 && maxBytes <= 95, "180,000 random poses all fit in 95 bytes", "longest \(maxBytes)")
    report.check(notJSON == 0 && keyErr == 0, "every message is JSON with exactly the page's keys")
    report.check(exponent == 0, "no number is written with an exponent")
    report.check(zeroPos == 0, "no position coordinate is ever exactly 0 (custom_icm.js:931)")
    report.check(precErr == 0, "every value is within its level's rounding step", "\(precErr) outside")
    report.note("levels used: \(levelCount) (0 = 4/3 decimals ... 3 = orientation only); "
                + "boxes of 5, 50 and 500 m")
    let o = HubMessage.encode(orientation: .identity, position: nil)
    report.check(o?.text == #"{"bone":"Hips","x":0,"y":0,"z":0,"w":1}"# && o?.hasPosition == false,
                 "orientation-only message has no px, py, pz", o?.text ?? "nil")
    report.check(HubMessage.encode(orientation: Quat(.nan, 0, 0, 1), position: nil) == nil,
                 "a non-finite quaternion is never encoded")
}

// ---------------------------------------------------------------- the reported frame

/// Run one wearer motion through the app's frame after a Zero at `start`.
private func reportAfterZero(mount: Mount, facing: CameraFacing, startHeading: Double, sessionHeading: Double,
                             leverKnown: Bool = true, moves: (Wearer) -> Wearer) -> (Vec3, Quat)? {
    let start = standing(heading: startHeading)
    // The session started earlier, with the phone in the hand pointing elsewhere.
    let world = ARWorld(cameraInRoom: .yaw(sessionHeading), at: Vec3(0.4, 1.2, 0.3))
    var frame = HipsFrame(lever: leverKnown ? mount.leverCam : .zero, hipHeight: 1.0, facing: facing)
    let c0 = world.camera(start, mount)
    guard (try? frame.setZero(camPosition: c0.p, camRotation: c0.q)) != nil else { return nil }
    let c1 = world.camera(moves(start), mount)
    return frame.report(camPosition: c1.p, camRotation: c1.q)
}

/// The wearer's own axes at the start, in the room.
private func walk(_ w: Wearer, forward: Double = 0, right: Double = 0, up: Double = 0) -> Wearer {
    var n = w
    n.hip = w.hip + w.pelvis.rotate(Vec3(right, up, -forward))
    return n
}

func checkHipsFrame() {
    report.section("reported frame: -Z is the wearer's forward at Zero, +X right, y = hip height")
    var r = Rand(3)
    var worstWalk = 0.0, worstTurn = 0.0, worstLean = 0.0, worstSpin = 0.0
    let mounts: [(String, Mount, CameraFacing)] = [
        ("front portrait", .frontPortrait(tilt: Quat(axis: Vec3(1, 0, 0), angle: rad(8))), .forward),
        ("front landscape", .frontLandscape(), .forward),
        ("back", .backPortrait(), .backward),
        ("left hip", .leftSide(), .left),
    ]
    for (_, m, facing) in mounts {
        for _ in 0..<50 {
            let h0 = r.uniform(-.pi, .pi), hs = r.uniform(-.pi, .pi)
            // 1 m forward, 0.5 m right, 0.1 m up
            if let (p, _) = reportAfterZero(mount: m, facing: facing, startHeading: h0, sessionHeading: hs,
                                            moves: { walk($0, forward: 1, right: 0.5, up: 0.1) }) {
                worstWalk = max(worstWalk, (p - Vec3(0.5, 1.1, -1)).length)
            } else { worstWalk = .infinity }
            // turn 90 deg left on the spot
            if let (p, q) = reportAfterZero(mount: m, facing: facing, startHeading: h0, sessionHeading: hs,
                                            moves: { w in var n = w; n.pelvis = .yaw(rad(90)) * w.pelvis; return n }) {
                worstTurn = max(worstTurn, (p - Vec3(0, 1, 0)).length)
                worstSpin = max(worstSpin, q.angle(to: .yaw(rad(90))))
            } else { worstTurn = .infinity }
            // lean 20 deg forward about the wearer's right axis
            if let (_, q) = reportAfterZero(mount: m, facing: facing, startHeading: h0, sessionHeading: hs,
                                            moves: { w in var n = w; n.pelvis = w.pelvis * Quat(axis: Vec3(1, 0, 0), angle: rad(-20)); return n }) {
                worstLean = max(worstLean, q.angle(to: Quat(axis: Vec3(1, 0, 0), angle: rad(-20))))
            } else { worstLean = .infinity }
        }
    }
    report.check(worstWalk < 1e-9, "walking 1 m forward, 0.5 m right, 0.1 m up reads (0.5, 1.1, -1) m",
                 "4 mounts x 50 random headings; worst \(worstWalk) m")
    report.check(worstTurn < 1e-9, "turning on the spot leaves the hips where they are (lever known)",
                 "worst \(worstTurn) m")
    report.check(worstSpin < 1e-9, "a left turn reads as yaw +90 deg", "worst \(deg(worstSpin)) deg")
    report.check(worstLean < 1e-9, "a forward lean reads as -20 deg about +X (up tips toward -Z)",
                 "worst \(deg(worstLean)) deg")

    let m = Mount.frontPortrait()
    if let (p, _) = reportAfterZero(mount: m, facing: .forward, startHeading: 0.3, sessionHeading: -1, leverKnown: false,
                                    moves: { w in var n = w; n.pelvis = .yaw(.pi) * w.pelvis; return n }) {
        report.check((p - Vec3(0, 1, 0)).length > 0.2,
                     "without the lever arm a half turn reads as travel",
                     "\(f((p - Vec3(0, 1, 0)).length * 100, 1)) cm for a 12 cm offset")
    }
    var frame = HipsFrame()
    let down = Quat(axis: Vec3(1, 0, 0), angle: rad(-89))       // camera looking at the floor
    report.check((try? frame.setZero(camPosition: .zero, camRotation: down)) == nil,
                 "Zero refuses a camera looking at the floor (no forward direction)")
    // Changing the lever after a Zero keeps the Zero pose at (0, h, 0).
    var g = HipsFrame(lever: Vec3(0, 0, 0.05), hipHeight: 1.0, facing: .forward)
    let q0 = Quat.yaw(0.7) * Mount.frontPortrait().camInPelvis
    _ = try? g.setZero(camPosition: Vec3(1, 1, 1), camRotation: q0)
    g.lever = Vec3(0.02, -0.01, 0.13)
    let p0 = g.report(camPosition: Vec3(1, 1, 1), camRotation: q0)?.position ?? Vec3(9, 9, 9)
    report.check((p0 - Vec3(0, 1, 0)).length < 1e-12, "a new lever arm does not move the Zero point")
}

// ---------------------------------------------------------------- page trees

/// What the page does with the phone's message for one tree
/// (js/custom_icm.js getTransformedQuaternion / getTransformedPosition):
/// "XYZ -+-" negates x and z; "XYZ +++" leaves them.
private func pageMaps(_ tree: PageTree) -> (orientation: (Quat) -> Quat, position: (Vec3) -> Vec3) {
    let flipQ: (Quat) -> Quat = { Quat(-$0.x, $0.y, -$0.z, $0.w) }
    let flipP: (Vec3) -> Vec3 = { Vec3(-$0.x, $0.y, -$0.z) }
    switch tree {
    case .velvetFamily: return (flipQ, { $0 })
    case .matched: return (flipQ, flipP)
    case .thorny: return ({ $0 }, { $0 })
    }
}

func checkPageTrees() {
    report.section("page trees: walking and leaning both come out along the avatar's facing (+Z)")
    // The avatar faces +Z in the scene (glbs/scene2.glb: the toes point +Z).
    let m = Mount.frontPortrait()
    for tree in PageTree.allCases {
        let maps = pageMaps(tree)
        guard let (pw, _) = reportAfterZero(mount: m, facing: .forward, startHeading: 1.1, sessionHeading: -0.4,
                                            moves: { walk($0, forward: 1) }),
              let (_, ql) = reportAfterZero(mount: m, facing: .forward, startHeading: 1.1, sessionHeading: -0.4,
                                            moves: { w in var n = w; n.pelvis = w.pelvis * Quat(axis: Vec3(1, 0, 0), angle: rad(-20)); return n })
        else { report.check(false, "\(tree.rawValue): zero failed"); continue }
        let wireWalk = tree.wire(position: pw, orientation: .identity).position
        let wireLean = tree.wire(position: Vec3(0, 1, 0), orientation: ql).orientation
        let sceneTravel = maps.position(wireWalk) - maps.position(tree.wire(position: Vec3(0, 1, 0), orientation: .identity).position)
        let sceneUp = maps.orientation(wireLean).rotate(Vec3(0, 1, 0))
        report.check(sceneTravel.z > 0.99 && abs(sceneTravel.x) < 1e-9,
                     "\(tree.rawValue): a forward walk moves the avatar toward +Z", fv(sceneTravel))
        report.check(sceneUp.z > 0.3, "\(tree.rawValue): a forward lean tips the avatar toward +Z", fv(sceneUp))
    }
}
