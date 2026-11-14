// =========================================================================
//  THE PHONE CONTROLLER  --  App/PhoneController.swift
//
//  WHY THIS FILE EXISTS
//  It is the glue, and only the glue: ARKit in, the hub link and the log
//  out, a status for the screen. Every decision about WHAT to send is made
//  by the tested Core (PosePipeline); this file decides only WHEN things
//  run.
//
//  CONCURRENCY: three queues, each owning its state.
//    main         the @Published UI state, the countdowns, the cues
//    frameQueue   ARKit's delegate queue: the pipeline, the log, the fit.
//                 An ARFrame is reduced to a FrameInput at once and never
//                 kept: ARKit stops delivering frames if the app holds them.
//    HubLink      its own queue (Transport/HubLink.swift)
//  Settings are copied from main to frameQueue whenever they change.
// =========================================================================
#if os(iOS)
import ARKit
import Foundation
import SwiftUI
import UIKit

/// What the screen shows; copied out of the frame queue ten times a second.
struct LiveStatus: Equatable {
    var tracking = "not started"
    var trackingOK = false
    var link = "stopped"
    var linkOK = false
    var sendHz = 0
    var sent = 0
    var skippedBusy = 0
    var lastBytes = 0
    var lastLevel = -1
    var levelCounts = [0, 0, 0, 0]
    var hipsCm: Vec3?
    var headingDeg: Double?
    var epoch = 0
    var logBytes = 0
}

final class PhoneController: NSObject, ObservableObject, ARSessionDelegate {
    @Published var settings: AppSettings {
        didSet { settingsChanged(from: oldValue) }
    }
    @Published private(set) var running = false
    @Published private(set) var live = LiveStatus()
    @Published private(set) var countdown: Int?
    @Published private(set) var activity: String?
    @Published private(set) var notice: String?
    @Published private(set) var pocketMode = false
    @Published private(set) var logURL: URL?

    private let session = ARSession()
    private let frameQueue = DispatchQueue(label: "mesquite.frames", qos: .userInteractive)
    private let link = HubLink()
    private let cues = Cues()
    private static let settingsKey = "settings.v1"

    // main only
    private var countdownItem: DispatchWorkItem?
    private var resumeOnActive = false
    private var savedBrightness: CGFloat?

    // frameQueue only
    private var pipeline: PosePipeline
    private var log: SessionLog?
    private var fit: LeverArmFit?
    private var fitStart: Double?
    private var fitHalfSaid = false
    private var lastTracking: Tracking?
    private var lastFrameTime = 0.0
    private var lastUI = 0.0
    private var lastFlush = 0.0
    private var sentTimes: [Double] = []
    private var levelCounts = [0, 0, 0, 0]
    private var lastMessage: HubMessage.Encoded?
    private var epoch = 0

    override init() {
        let stored = Self.loadSettings()
        settings = stored
        pipeline = stored.makePipeline()
        super.init()
        session.delegate = self
        session.delegateQueue = frameQueue
        link.onState = { [weak self] state in
            guard let self else { return }
            self.frameQueue.async { self.log?.event(self.lastFrameTime, "link " + Self.describe(state)) }
        }
    }

    // ---------------------------------------------------------------- commands (main)

    func start() {
        guard !running else { return }
        guard ARWorldTrackingConfiguration.isSupported else {
            notice = "This device cannot run ARKit world tracking."
            return
        }
        guard let url = URL(string: settings.hubURL), url.scheme == "ws", url.host != nil else {
            notice = "The hub address must look like ws://192.168.4.1:80/ws"
            return
        }
        notice = nil
        let s = settings
        let config = Self.configuration()
        // UIKit and the configuration are read here, on main; the frame
        // queue only gets plain values.
        let v = config.videoFormat
        let device: [(String, String)] = [
            ("app", (Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String) ?? "?"),
            ("device", Self.machine()),
            ("ios", UIDevice.current.systemVersion),
            ("video", "\(Int(v.imageResolution.width))x\(Int(v.imageResolution.height))@\(v.framesPerSecond)"),
        ]
        frameQueue.async {
            self.pipeline = s.makePipeline()
            self.epoch = 0
            self.levelCounts = [0, 0, 0, 0]
            self.sentTimes = []
            self.lastTracking = nil
            self.openLog(settings: s, device: device)
        }
        session.run(config, options: [.resetTracking, .removeExistingAnchors])
        link.start(url: url)
        running = true
        UIApplication.shared.isIdleTimerDisabled = true
        cues.prepare(spoken: s.spokenCues)
        cues.say("Streaming. Zero in T-pose when you are ready.")
    }

    func stop() {
        guard running else { return }
        cancelCountdown()
        session.pause()
        link.stop()
        frameQueue.async {
            self.fit = nil
            self.log?.event(self.lastFrameTime, "stop")
            self.log?.close()
            self.log = nil
        }
        running = false
        resumeOnActive = false
        setPocket(false)
        UIApplication.shared.isIdleTimerDisabled = false
        live = LiveStatus()
    }

    /// Count down, then Zero on the next good frame. Stand in the T-pose,
    /// facing the direction the avatar should face; then press the page's
    /// "Set T-pose" while still standing in it.
    func zeroWithCountdown() {
        guard running else { return }
        let n = settings.countdownSeconds
        runCountdown(activity: "Zero", seconds: n,
                     intro: "Zero in \(n) seconds. Stand in T-pose, facing forward.") {
            self.frameQueue.async { self.pipeline.requestZero() }
        }
    }

    /// Count down, then collect a slow turn on the spot and fit the lever arm.
    func measureLever() {
        guard running else { return }
        let n = settings.countdownSeconds
        let prior = Vec3(0, 0, settings.leverDepthCm / 100)
        runCountdown(activity: "Lever arm", seconds: n,
                     intro: "Lever arm in \(n) seconds. Then turn slowly on the spot, once around.") {
            self.cues.say("Turn now. Slowly, once around, feet in one place.")
            self.frameQueue.async {
                self.fit = LeverArmFit(prior: prior)
                self.fitStart = nil
                self.fitHalfSaid = false
            }
        }
    }

    func forgetMeasuredLever() { settings.measuredLever = nil }

    func cancelCountdown() {
        countdownItem?.cancel()
        countdownItem = nil
        countdown = nil
        activity = nil
    }

    func setPocket(_ on: Bool) {
        guard on != pocketMode else { return }
        pocketMode = on
        guard let screen = UIApplication.shared.connectedScenes
            .compactMap({ ($0 as? UIWindowScene)?.screen }).first else { return }
        if on {
            savedBrightness = screen.brightness
            screen.brightness = 0.02
        } else if let b = savedBrightness {
            screen.brightness = b
            savedBrightness = nil
        }
    }

    func scenePhaseChanged(_ phase: ScenePhase) {
        switch phase {
        case .background:
            // ARKit cannot run in the background; leave cleanly and come back.
            guard running else { return }
            resumeOnActive = true
            cancelCountdown()
            session.pause()
            link.stop()
            frameQueue.async {
                self.log?.event(self.lastFrameTime, "app in background: tracking paused")
                self.log?.flush()
            }
        case .active:
            guard resumeOnActive, let url = URL(string: settings.hubURL) else { return }
            resumeOnActive = false
            // No .resetTracking: ARKit tries to relocalize into the same
            // world, so the Zero can survive a short interruption.
            session.run(Self.configuration(), options: [])
            link.start(url: url)
            notice = "Tracking resumed. If the avatar jumped, Zero again and redo the page's T-pose."
            frameQueue.async { self.log?.event(self.lastFrameTime, "app active: tracking resumed") }
        default:
            frameQueue.async { self.log?.flush() }
        }
    }

    // ---------------------------------------------------------------- ARSessionDelegate (frameQueue)

    func session(_ session: ARSession, didUpdate frame: ARFrame) {
        process(Self.input(from: frame))
    }

    func session(_ session: ARSession, didFailWithError error: Error) {
        let text: String
        if let ar = error as? ARError, ar.code == .cameraUnauthorized {
            text = "Camera access is off. Turn it on in Settings, Mesquite Hips, Camera."
        } else {
            text = "Tracking failed: \(error.localizedDescription)"
        }
        log?.event(lastFrameTime, "session failed: \(error.localizedDescription)")
        DispatchQueue.main.async {
            self.notice = text
            self.cues.failure(text)
        }
    }

    func sessionWasInterrupted(_ session: ARSession) {
        log?.event(lastFrameTime, "session interrupted")
    }

    func sessionInterruptionEnded(_ session: ARSession) {
        log?.event(lastFrameTime, "session interruption ended")
    }

    func sessionShouldAttemptRelocalization(_ session: ARSession) -> Bool { true }

    // ---------------------------------------------------------------- per frame (frameQueue)

    private func process(_ f: FrameInput) {
        lastFrameTime = f.time
        if f.tracking != lastTracking {
            log?.event(f.time, "tracking " + f.tracking.label)
            lastTracking = f.tracking
        }

        let out = pipeline.process(f)
        if let z = out.zero {
            epoch = z.epoch
            log?.event(f.time, String(format: "zero epoch=%d heading_deg=%.2f", z.epoch, z.heading * 180 / .pi))
            if out.zeroWasRequested {
                DispatchQueue.main.async {
                    self.notice = "Zeroed. Now press the page's Set T-pose while you hold the pose."
                    self.cues.success("Zeroed. Now set T-pose on the computer.")
                }
            }
        }
        if let e = out.zeroError {
            log?.event(f.time, "zero failed: \(e)")
            DispatchQueue.main.async {
                self.notice = "Zero failed: hold the phone so the camera looks out level, not at the floor."
                self.cues.failure("Zero failed. The camera must look straight out.")
            }
        }

        var sent = false
        if let m = out.message, link.send(m.text) {
            pipeline.markSent(f.time)
            sent = true
            sentTimes.append(f.time)
            levelCounts[m.level] += 1
            lastMessage = m
        }
        log?.frame(f, epoch: epoch, reported: out.reported, sent: sent, message: out.message)

        if fit != nil, f.tracking == .normal { stepFit(f) }

        if f.time - lastFlush > 0.5 {
            log?.flush()
            lastFlush = f.time
        }
        if f.time - lastUI > 0.1 {
            lastUI = f.time
            publish(f, reported: out.reported)
        }
    }

    private func stepFit(_ f: FrameInput) {
        guard var current = fit else { return }
        current.add(camPosition: f.camPosition, camRotation: f.camRotation)
        fit = current
        let start = fitStart ?? f.time
        fitStart = start
        // The wearer cannot see the screen: say when half the turn is done.
        if !fitHalfSaid && current.headingRange >= .pi {
            fitHalfSaid = true
            DispatchQueue.main.async { self.cues.say("Half way.") }
        }
        // Finish after one full turn (plus a little), or after 30 s with
        // whatever turn was made.
        let done = current.headingRange >= 2.1 * .pi || f.time - start > 30
        guard done else { return }
        fit = nil
        switch current.solve() {
        case .success(let r):
            log?.event(f.time, String(format: "lever fit l_cam=(%.4f,%.4f,%.4f) m rms=%.4f m turn_deg=%.0f n=%d",
                                      r.lever.x, r.lever.y, r.lever.z, r.rms, r.headingRange * 180 / .pi, r.samples))
            DispatchQueue.main.async {
                self.settings.measuredLever = r.lever
                let cm = Int((r.lever.length * 100).rounded())
                self.notice = String(format: "Lever arm %.1f cm (fit within %.1f cm).", r.lever.length * 100, r.rms * 100)
                self.cues.success("Lever arm measured, \(cm) centimetres.")
            }
        case .failure(let why):
            log?.event(f.time, "lever fit failed: \(why)")
            DispatchQueue.main.async {
                self.notice = "Lever arm not measured: \(Self.describe(why))"
                self.cues.failure("Lever arm not measured. \(Self.describe(why))")
            }
        }
    }

    private func publish(_ f: FrameInput, reported: (position: Vec3, orientation: Quat)?) {
        sentTimes.removeAll { $0 < f.time - 1 }
        let (linkState, stats) = link.snapshot
        var s = LiveStatus()
        s.tracking = f.tracking.label.replacingOccurrences(of: "_", with: " ")
        s.trackingOK = f.tracking == .normal
        s.link = Self.describe(linkState)
        s.linkOK = linkState == .ready
        s.sendHz = sentTimes.count
        s.sent = stats.sent
        s.skippedBusy = stats.skippedBusy
        s.lastBytes = lastMessage?.bytes ?? 0
        s.lastLevel = lastMessage?.level ?? -1
        s.levelCounts = levelCounts
        if let r = reported {
            s.hipsCm = 100 * r.position
            s.headingDeg = r.orientation.rotate(Vec3(0, 0, -1)).heading().map { $0 * 180 / .pi }
        }
        s.epoch = epoch
        s.logBytes = log?.bytesWritten ?? 0
        DispatchQueue.main.async { if self.running { self.live = s } }
    }

    private func openLog(settings s: AppSettings, device: [(String, String)]) {
        log?.close()
        log = nil
        guard s.logging,
              let docs = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first else {
            DispatchQueue.main.async { self.logURL = nil }
            return
        }
        let start = Date()
        // ARFrame.timestamp runs on the uptime clock; this offset turns it
        // into Unix time for lining the log up with a page recording.
        let offset = start.timeIntervalSince1970 - ProcessInfo.processInfo.systemUptime
        let lever = s.lever
        let header: [(String, String)] = device + [
            ("world_alignment", "gravity"),
            ("hub", s.hubURL),
            ("rate_hz", "\(s.rateHz)"),
            ("page_tree", s.tree.rawValue),
            ("camera_facing", s.facing.rawValue),
            ("hip_height_m", String(format: "%.3f", s.hipHeightCm / 100)),
            ("lever_cam_m", String(format: "%.4f,%.4f,%.4f", lever.x, lever.y, lever.z)),
            ("lever_source", s.measuredLever == nil ? "manual_depth" : "measured"),
        ]
        do {
            let l = try SessionLog(directory: docs, name: SessionLog.fileName(start: start),
                                   wallOffset: offset, header: header)
            log = l
            DispatchQueue.main.async { self.logURL = l.url }
        } catch {
            DispatchQueue.main.async {
                self.logURL = nil
                self.notice = "Could not create the log file: \(error.localizedDescription)"
            }
        }
    }

    // ---------------------------------------------------------------- settings

    private func settingsChanged(from old: AppSettings) {
        if let data = try? JSONEncoder().encode(settings) {
            UserDefaults.standard.set(data, forKey: Self.settingsKey)
        }
        let s = settings
        frameQueue.async {
            self.pipeline.rateHz = Double(s.rateHz)
            self.pipeline.tree = s.tree
            self.pipeline.frame.facing = s.facing            // used at the next Zero
            self.pipeline.frame.hipHeight = s.hipHeightCm / 100
            self.pipeline.frame.lever = s.lever
        }
        cues.setSpoken(s.spokenCues)
        if running, s.hubURL != old.hubURL, let url = URL(string: s.hubURL) {
            link.start(url: url)
        }
    }

    private static func loadSettings() -> AppSettings {
        guard let data = UserDefaults.standard.data(forKey: settingsKey),
              let s = try? JSONDecoder().decode(AppSettings.self, from: data) else { return AppSettings() }
        return s
    }

    // ---------------------------------------------------------------- helpers

    private func runCountdown(activity name: String, seconds: Int, intro: String, done: @escaping () -> Void) {
        cancelCountdown()
        activity = name
        countdown = max(1, seconds)
        cues.say(intro)
        scheduleTick(remaining: max(1, seconds), done: done)
    }

    private func scheduleTick(remaining: Int, done: @escaping () -> Void) {
        let item = DispatchWorkItem { [weak self] in
            guard let self else { return }
            let left = remaining - 1
            if left <= 0 {
                self.countdown = nil
                self.activity = nil
                self.countdownItem = nil
                done()
            } else {
                self.countdown = left
                if left <= 3 { self.cues.say("\(left)") }
                self.scheduleTick(remaining: left, done: done)
            }
        }
        countdownItem = item
        DispatchQueue.main.asyncAfter(deadline: .now() + 1, execute: item)
    }

    /// World tracking with nothing extra: no planes, no environment texture,
    /// no light estimate and no view to render. Pose is all the app needs,
    /// and every feature left off is heat and battery saved.
    private static func configuration() -> ARWorldTrackingConfiguration {
        let c = ARWorldTrackingConfiguration()
        c.worldAlignment = .gravity
        c.planeDetection = []
        c.environmentTexturing = .none
        c.isLightEstimationEnabled = false
        return c
    }

    static func input(from frame: ARFrame) -> FrameInput {
        let m = frame.camera.transform
        let c0 = Vec3(Double(m.columns.0.x), Double(m.columns.0.y), Double(m.columns.0.z))
        let c1 = Vec3(Double(m.columns.1.x), Double(m.columns.1.y), Double(m.columns.1.z))
        let c2 = Vec3(Double(m.columns.2.x), Double(m.columns.2.y), Double(m.columns.2.z))
        let p = Vec3(Double(m.columns.3.x), Double(m.columns.3.y), Double(m.columns.3.z))
        return FrameInput(time: frame.timestamp, camPosition: p, camRotation: Quat(columns: c0, c1, c2),
                          tracking: tracking(frame.camera.trackingState))
    }

    static func tracking(_ state: ARCamera.TrackingState) -> Tracking {
        switch state {
        case .normal: return .normal
        case .notAvailable: return .notAvailable
        case .limited(let reason):
            switch reason {
            case .initializing: return .limited(.initializing)
            case .excessiveMotion: return .limited(.excessiveMotion)
            case .insufficientFeatures: return .limited(.insufficientFeatures)
            case .relocalizing: return .limited(.relocalizing)
            @unknown default: return .limited(.other)
            }
        }
    }

    static func describe(_ state: HubLink.State) -> String {
        switch state {
        case .stopped: return "stopped"
        case .connecting: return "connecting"
        case .ready: return "connected"
        case .waiting(let why): return why
        }
    }

    static func describe(_ failure: LeverArmFit.Failure) -> String {
        switch failure {
        case .tooFewSamples: return "tracking was not steady long enough."
        case .notEnoughTurn(let deg): return String(format: "only %.0f degrees of turn; turn at least half way round.", deg)
        case .implausibleLever(let m): return String(format: "the fit gave %.0f cm, too far to be the hips.", m * 100)
        case .poorFit(let rms): return String(format: "the feet moved too much (%.1f cm); keep them in one place.", rms * 100)
        }
    }

    private static func machine() -> String {
        var u = utsname()
        uname(&u)
        return withUnsafeBytes(of: &u.machine) { raw in
            String(decoding: raw.prefix { $0 != 0 }, as: UTF8.self)
        }
    }
}
#endif
