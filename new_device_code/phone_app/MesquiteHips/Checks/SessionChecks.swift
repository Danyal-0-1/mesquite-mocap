// =========================================================================
//  SESSION CHECKS  --  Checks/SessionChecks.swift
//
//  The lever-arm fit and the frame-by-frame pipeline, run on simulated
//  60 Hz ARKit takes.
// =========================================================================
import Foundation

/// A slow turn on the spot: `turns` full turns (negative = right) over
/// `seconds`, with a small sway of the hips and 2 mm of position noise.
private func turnTake(_ r: inout Rand, mount: Mount, turns: Double, seconds: Double, sway: Double = 0.01,
                      drift: Double = 0) -> [(Vec3, Quat)] {
    let world = ARWorld(cameraInRoom: .yaw(0.8), at: Vec3(0, 1, 0))
    var out: [(Vec3, Quat)] = []
    let n = Int(seconds * 60)
    for i in 0...n {
        let s = Double(i) / Double(n)
        let heading = 0.4 + 2 * .pi * turns * smoothstep(s)
        let hip = Vec3(sway * sin(9 * s) + drift * s, 0.98 + 0.005 * sin(13 * s), sway * cos(7 * s))
        let tilt = Quat(axis: Vec3(1, 0, 0), angle: rad(3 * sin(5 * s)))
        let w = Wearer(hip: hip, pelvis: .yaw(heading) * tilt)
        var c = world.camera(w, mount)
        c.p = c.p + Vec3(0.002 * r.gauss(), 0.002 * r.gauss(), 0.002 * r.gauss())
        out.append(c)
    }
    return out
}

func checkLeverArm() {
    report.section("lever arm: a slow turn on the spot finds camera -> hip centre")
    var r = Rand(4)
    let mount = Mount.frontPortrait(offset: Vec3(0.015, 0.04, -0.12),
                                    tilt: Quat(axis: Vec3(1, 0, 0), angle: rad(10)))
    let truth = mount.leverCam
    // The part of l a turn cannot see is along the vertical, in camera axes.
    let vertical = mount.camInPelvis.conjugate.rotate(.up)
    func seen(_ e: Vec3) -> Double { (e - e.dot(vertical) * vertical).length }

    var exact = LeverArmFit(prior: Vec3(0, 0, 0.10))
    for (p, q) in turnTake(&r, mount: mount, turns: 1.05, seconds: 20, sway: 0) {
        exact.add(camPosition: p, camRotation: q)
    }
    if case .success(let res) = exact.solve() {
        report.check(seen(res.lever - truth) < 0.003, "still hips, 2 mm noise: within 3 mm",
                     "truth \(fv(truth)) fit \(fv(res.lever)) m, rms \(f(res.rms * 1000, 1)) mm")
        report.note("vertical part (unobservable from turning) stays at the prior: "
                    + "\(f(res.lever.dot(vertical) * 100, 1)) cm vs truth \(f(truth.dot(vertical) * 100, 1)) cm")
    } else { report.check(false, "still hips fit") }

    // Real hips sway while turning; the sway is a bias the fit cannot remove.
    var fit = LeverArmFit(prior: Vec3(0, 0, 0.10))
    for (p, q) in turnTake(&r, mount: mount, turns: 1.05, seconds: 20, sway: 0.01) {
        fit.add(camPosition: p, camRotation: q)
    }
    switch fit.solve() {
    case .success(let res):
        report.check(seen(res.lever - truth) < 0.015, "hips swaying 1 cm while turning: within 1.5 cm",
                     "fit \(fv(res.lever)) m, rms \(f(res.rms * 1000, 1)) mm")
        // With the fitted lever the hips nearly stay put through a half turn.
        var frame = HipsFrame(lever: res.lever, hipHeight: 1, facing: .forward)
        var r2 = Rand(5)
        let take = turnTake(&r2, mount: mount, turns: 0.5, seconds: 4, sway: 0, drift: 0)
        _ = try? frame.setZero(camPosition: take[0].0, camRotation: take[0].1)
        func travel() -> Double {
            take.map { ((frame.report(camPosition: $0.0, camRotation: $0.1)?.position ?? .zero) - Vec3(0, 1, 0)).length }
                .max() ?? .infinity
        }
        let withFit = travel()
        frame.lever = .zero
        let withNone = travel()
        report.check(withFit < 0.04 && withNone > 0.2,
                     "a half turn: false hip travel drops from about 25 cm to under 4 cm",
                     "\(f(withNone * 100, 1)) cm without a lever, \(f(withFit * 100, 1)) cm with the fit")
    case .failure(let e):
        report.check(false, "swaying fit", "\(e)")
    }

    var half = LeverArmFit(prior: Vec3(0, 0, 0.10))
    for (p, q) in turnTake(&r, mount: mount, turns: 0.55, seconds: 10) { half.add(camPosition: p, camRotation: q) }
    if case .success(let res) = half.solve() {
        report.check(seen(res.lever - truth) < 0.02, "a half turn is enough: within 2 cm", "fit \(fv(res.lever)) m")
    } else {
        report.check(false, "half turn fit")
    }

    var still = LeverArmFit()
    for (p, q) in turnTake(&r, mount: mount, turns: 0.1, seconds: 10) { still.add(camPosition: p, camRotation: q) }
    if case .failure(.notEnoughTurn) = still.solve() {
        report.check(true, "a 36 degree twist is refused (not enough turn)")
    } else { report.check(false, "a 36 degree twist is refused") }

    var walking = LeverArmFit()
    for (p, q) in turnTake(&r, mount: mount, turns: 1, seconds: 15, drift: 0.6) { walking.add(camPosition: p, camRotation: q) }
    switch walking.solve() {
    case .failure(.poorFit), .failure(.implausibleLever):
        report.check(true, "a turn that walks 60 cm is refused", "\(walking.solve())")
    default:
        report.check(false, "a turn that walks 60 cm is refused", "\(walking.solve())")
    }

    var few = LeverArmFit()
    for (p, q) in turnTake(&r, mount: mount, turns: 1, seconds: 0.5) { few.add(camPosition: p, camRotation: q) }
    if case .failure(.tooFewSamples) = few.solve() {
        report.check(true, "half a second of samples is refused")
    } else { report.check(false, "half a second of samples is refused") }
}

// ---------------------------------------------------------------- the pipeline

private func frames(seconds: Double, rate: Double = 60, jitter: Double = 0.0015, _ r: inout Rand) -> [Double] {
    (0..<Int(seconds * rate)).map { Double($0) / rate + jitter * r.uniform(-1, 1) + 100 }
}

func checkPipeline() {
    report.section("pipeline: rate, tracking policy, Zero and sign continuity")
    var r = Rand(6)
    for (rate, expect) in [(15.0, 150), (30.0, 300), (60.0, 600)] {
        var p = PosePipeline(rateHz: rate)
        var sentTimes: [Double] = []
        for t in frames(seconds: 10, &r) {
            let out = p.process(FrameInput(time: t, camPosition: .zero, camRotation: .identity, tracking: .normal))
            if out.message != nil { p.markSent(t); sentTimes.append(t) }
        }
        let gaps = zip(sentTimes.dropFirst(), sentTimes).map { $0 - $1 }
        let worst = gaps.map { abs($0 - 1 / rate) }.max() ?? 1
        report.check(abs(sentTimes.count - expect) <= 1 && worst < 0.004,
                     "\(Int(rate)) Hz from 60 Hz frames with 1.5 ms jitter",
                     "\(sentTimes.count) messages in 10 s, worst gap error \(f(worst * 1000, 1)) ms")
    }

    var p = PosePipeline()
    let base = FrameInput(time: 0, camPosition: Vec3(0, 0, 0), camRotation: .identity, tracking: .limited(.initializing))
    var f = base
    var out = p.process(f)
    report.check(out.message == nil && out.zero == nil, "initializing: nothing is sent, no Zero yet")
    f.time = 1; f.tracking = .normal
    out = p.process(f)
    report.check(out.zero?.epoch == 1 && !out.zeroWasRequested && out.message?.hasPosition == true,
                 "first normal frame: automatic Zero (epoch 1) and a full message")
    p.markSent(1)
    f.time = 2; f.tracking = .limited(.excessiveMotion)
    out = p.process(f)
    report.check(out.message != nil && out.message?.hasPosition == false,
                 "excessive motion: orientation only, the page holds the root", out.message?.text ?? "nil")
    f.time = 3; f.tracking = .limited(.relocalizing)
    report.check(p.process(f).message == nil, "relocalizing: nothing is sent")
    f.time = 4; f.tracking = .notAvailable
    report.check(p.process(f).message == nil, "not available: nothing is sent")
    p.requestZero()
    f.time = 5; f.tracking = .limited(.insufficientFeatures)
    report.check(p.process(f).zero == nil, "a requested Zero waits for normal tracking")
    f.time = 6; f.tracking = .normal
    out = p.process(f)
    report.check(out.zero?.epoch == 2 && out.zeroWasRequested, "then happens on the next normal frame (epoch 2)")

    // Sign continuity through a full turn: consecutive quaternions never flip.
    var q = PosePipeline(rateHz: 60)
    var last: Quat?
    var flips = 0
    for i in 0...720 {
        let a = rad(Double(i))
        let o = q.process(FrameInput(time: Double(i) / 60, camPosition: .zero,
                                     camRotation: .yaw(a) * Mount.frontPortrait().camInPelvis, tracking: .normal))
        if let rep = o.reported?.orientation {
            if let l = last, rep.dot(l) < 0 { flips += 1 }
            last = rep
        }
        if o.message != nil { q.markSent(Double(i) / 60) }
    }
    report.check(flips == 0, "two full turns: the quaternion's sign never flips between frames")
}
