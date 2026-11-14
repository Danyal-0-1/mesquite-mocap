// =========================================================================
//  THE POSE PIPELINE  --  Core/PosePipeline.swift
//
//  WHY THIS FILE EXISTS
//  Everything between "ARKit produced a frame" and "this text goes to the
//  hub" lives here, in plain Swift, so the whole decision path is tested on
//  a desktop. The iOS layer (App/PhoneController.swift) only converts an
//  ARFrame into a FrameInput and hands the result to the link and the log.
//
//  One call per ARKit frame (~60 Hz):
//    1. tracking state -> what may be sent (policy below)
//    2. Zero: automatic on the first good frame, then whenever requested
//    3. hip pose in the reported frame (HipsFrame), sign kept continuous
//    4. turned for the page's tree (PageTree), encoded (HubMessage)
//    5. rate gate: at most `rateHz` messages a second
//
//  POLICY: what may leave the phone in each tracking state.
//    normal                               -> orientation and position
//    limited (excessive motion,           -> orientation only. VIO's position
//             insufficient features)         is the first thing to degrade, the
//                                            gyroscope still holds rotation, and
//                                            the page keeps the root where it is
//                                            when px, py, pz are absent
//    limited (initializing, relocalizing) -> nothing: the pose may belong to a
//    not available                           world that is about to change
// =========================================================================
import Foundation

public enum LimitedReason: String, Codable, Sendable {
    case initializing, excessiveMotion, insufficientFeatures, relocalizing, other
}

public enum Tracking: Equatable, Sendable {
    case normal
    case limited(LimitedReason)
    case notAvailable

    public var label: String {
        switch self {
        case .normal: return "normal"
        case .limited(let r): return "limited:" + r.rawValue
        case .notAvailable: return "not_available"
        }
    }

    /// What the policy above allows.
    public var allows: (orientation: Bool, position: Bool) {
        switch self {
        case .normal: return (true, true)
        case .limited(.excessiveMotion), .limited(.insufficientFeatures): return (true, false)
        case .limited, .notAvailable: return (false, false)
        }
    }
}

/// One ARKit frame, reduced to what the pipeline needs.
public struct FrameInput: Sendable {
    /// ARFrame.timestamp: seconds on the device's uptime clock.
    public var time: Double
    public var camPosition: Vec3
    public var camRotation: Quat
    public var tracking: Tracking

    public init(time: Double, camPosition: Vec3, camRotation: Quat, tracking: Tracking) {
        self.time = time
        self.camPosition = camPosition
        self.camRotation = camRotation
        self.tracking = tracking
    }
}

public struct FrameOutput: Sendable {
    /// The pose in the reported frame (before the tree turn), nil before Zero.
    public var reported: (position: Vec3, orientation: Quat)?
    /// The message, when one is due. The caller sends it and then calls
    /// `markSent` if the link took it.
    public var message: HubMessage.Encoded?
    /// A Zero that happened on this frame, and whether the wearer asked for
    /// it (the first one of a session is automatic).
    public var zero: ZeroState?
    public var zeroWasRequested = false
    public var zeroError: ZeroError?
}

public struct PosePipeline: Sendable {
    public var frame: HipsFrame
    public var tree: PageTree
    /// Messages per second. The pods send ~32 Hz; the page smooths per
    /// message, so a similar rate keeps the root as smooth as the limbs.
    public var rateHz: Double

    private var lastSent = -Double.infinity
    private var lastOrientation: Quat?
    private var zeroRequested = false

    public init(frame: HipsFrame = HipsFrame(), tree: PageTree = .velvetFamily, rateHz: Double = 30) {
        self.frame = frame
        self.tree = tree
        self.rateHz = rateHz
    }

    /// Ask for a Zero on the next frame with normal tracking.
    public mutating func requestZero() { zeroRequested = true }

    /// A new AR session means a new world: the old Zero no longer applies.
    public mutating func resetWorld() {
        frame.clearZero()
        lastOrientation = nil
        lastSent = -.infinity
    }

    /// MATH: a frame is due once 1/rate has passed, less a quarter of a
    /// 60 Hz frame, so 30 Hz takes exactly every second 60 Hz frame despite
    /// timestamp jitter, and 60 Hz takes every frame.
    public func isDue(_ time: Double) -> Bool {
        time - lastSent >= 1 / max(1, rateHz) - 0.25 / 60
    }

    public mutating func markSent(_ time: Double) { lastSent = time }

    public mutating func process(_ f: FrameInput) -> FrameOutput {
        var out = FrameOutput()
        if f.tracking == .normal && (zeroRequested || frame.zero == nil) {
            do {
                out.zero = try frame.setZero(camPosition: f.camPosition, camRotation: f.camRotation)
                out.zeroWasRequested = zeroRequested
                zeroRequested = false
                lastOrientation = nil
            } catch let e as ZeroError {
                // Keep the request pending only for a Zero the wearer asked
                // for; the automatic one simply retries on the next frame.
                if zeroRequested { zeroRequested = false; out.zeroError = e }
            } catch {}
        }
        guard var reported = frame.report(camPosition: f.camPosition, camRotation: f.camRotation) else {
            return out
        }
        // MATH: q and -q are the same rotation. Keep each sample on the same
        // side as the last, so the stream never flips sign between frames;
        // nothing downstream needs it, but logs and any filter stay smooth.
        if let last = lastOrientation, reported.orientation.dot(last) < 0 {
            reported.orientation = reported.orientation.negated
        }
        lastOrientation = reported.orientation
        out.reported = reported

        let allowed = f.tracking.allows
        guard allowed.orientation, isDue(f.time) else { return out }
        let wire = tree.wire(position: reported.position, orientation: reported.orientation)
        out.message = HubMessage.encode(orientation: wire.orientation,
                                        position: allowed.position ? wire.position : nil)
        return out
    }
}
