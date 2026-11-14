// =========================================================================
//  THE REPORTED HIPS FRAME  --  Core/HipsFrame.swift
//
//  WHY THIS FILE EXISTS
//  ARKit's world is a convention of the moment the session started: its
//  origin is wherever the phone was, and its -Z is wherever the camera
//  happened to point (Apple, ARConfiguration.WorldAlignment.gravity). The
//  page never registers that frame to the body (system_assessment/
//  07_phone_slam/REPORT.md, PHN-04), so the app does it, at a moment the
//  wearer chooses: ZERO, taken in the same T-pose as the page's own
//  "Set T-pose".
//
//  After a Zero the app reports, in metres, +Y up:
//    origin  on the floor directly under the hip centre (so y = hip height)
//    -Z      the wearer's forward at that moment
//    +X      the wearer's right
//  and the pelvis orientation RELATIVE to that moment (identity at Zero).
//
//  MATH (MATH.md section 3):
//    hip(t) = p_cam(t) + R_cam(t) l            l = lever arm, camera frame
//    A      = yaw(-psi0)                       psi0 = heading of the wearer's
//                                              forward at Zero
//    p_rep  = A (hip(t) - hip(T0)) + (0, h, 0)
//    q_rep  = A q_cam(t) q_cam(T0)^-1 A^-1
// =========================================================================
import Foundation

/// Which way the phone's rear camera looks, relative to the wearer. The page's
/// wear guide shows the phone on the front of the belt, camera looking forward
/// (trees/velvet/wearguide/threejsscene.js, loadAllPods).
public enum CameraFacing: String, CaseIterable, Codable, Sendable {
    case forward, backward, left, right

    /// The turn (about +Y) from the camera's horizontal view direction to the
    /// wearer's forward.
    public var toWearerForward: Double {
        switch self {
        case .forward: return 0
        case .backward: return .pi
        case .left: return -.pi / 2      // camera looks left: forward is a right turn away
        case .right: return .pi / 2
        }
    }
}

/// What a Zero fixed.
public struct ZeroState: Equatable, Codable, Sendable {
    /// Counts Zeros in this session. Every Zero starts a new reported frame,
    /// so the page's T-pose must be redone after one (HOW_IT_WORKS.md).
    public var epoch: Int
    public var camPosition: Vec3
    public var camRotation: Quat
    /// A: ARKit world -> reported frame. A pure turn about the vertical.
    public var align: Quat
    /// psi0: the heading of the wearer's forward in ARKit's world.
    public var heading: Double
}

public enum ZeroError: Error, Equatable {
    /// The camera looked (nearly) straight up or down: no forward direction.
    case cameraNotLevel
    case notFinite
}

public struct HipsFrame: Sendable {
    /// From the camera to the hip centre, in the camera's own frame, metres.
    /// Constant while the phone is strapped down; see LeverArmFit.
    public var lever: Vec3 = .zero
    /// The reported height at Zero, metres. The page places the hips at
    /// 100 * py; 1.00 m matches the rig (MATH.md section 3.3).
    public var hipHeight: Double = 1.0
    public var facing: CameraFacing = .forward
    public private(set) var zero: ZeroState?

    /// MATH: the camera's view direction must keep at least this much
    /// horizontal length (a tilt of up to ~72 degrees from level) to give a
    /// trustworthy forward direction.
    public static let minHorizontal = 0.3

    public init(lever: Vec3 = .zero, hipHeight: Double = 1.0, facing: CameraFacing = .forward) {
        self.lever = lever
        self.hipHeight = hipHeight
        self.facing = facing
    }

    public func hip(camPosition p: Vec3, camRotation q: Quat) -> Vec3 {
        p + q.rotate(lever)
    }

    /// Fix the reported frame at this camera pose. Call it in the T-pose.
    @discardableResult
    public mutating func setZero(camPosition p: Vec3, camRotation q: Quat) throws -> ZeroState {
        guard p.isFinite, q.isFinite, q.norm > 0 else { throw ZeroError.notFinite }
        let r = q.normalized
        // The rear camera looks along its own -Z (Apple, ARCamera.transform).
        let view = r.rotate(Vec3(0, 0, -1))
        guard let viewHeading = view.heading(minHorizontal: Self.minHorizontal) else {
            throw ZeroError.cameraNotLevel
        }
        let psi0 = remainder(viewHeading + facing.toWearerForward, 2 * .pi)
        let state = ZeroState(epoch: (zero?.epoch ?? 0) + 1, camPosition: p, camRotation: r,
                              align: .yaw(-psi0), heading: psi0)
        zero = state
        return state
    }

    /// Forget the Zero, e.g. when the AR session restarts with a new world.
    public mutating func clearZero() { zero = nil }

    /// The pose to report for one camera sample, or nil before the first Zero.
    public func report(camPosition p: Vec3, camRotation q: Quat) -> (position: Vec3, orientation: Quat)? {
        guard let z = zero else { return nil }
        let r = q.normalized
        // The origin uses the CURRENT lever, so changing the lever after a
        // Zero keeps the Zero pose itself at (0, h, 0) instead of jumping it.
        let origin = hip(camPosition: z.camPosition, camRotation: z.camRotation)
        let position = z.align.rotate(hip(camPosition: p, camRotation: r) - origin) + Vec3(0, hipHeight, 0)
        let orientation = (z.align * r * z.camRotation.conjugate * z.align.conjugate).normalized
        return (position, orientation)
    }
}

/// How the page's suit tree relabels the phone's axes. Each tree's "Hips"
/// entry (trees/<id>/meta.json) sends the orientation through "axis" and the
/// position through "posswap" when present (js/custom_icm.js,
/// getTransformedQuaternion and getTransformedPosition). Those two maps
/// disagree in most trees at 7bdcf1c, so the app turns what it sends to make
/// BOTH come out right for the tree you picked in the page (MATH.md 3.4).
public enum PageTree: String, CaseIterable, Codable, Sendable {
    /// velvet, spiny, honey, velvetGreen (and bc): axis XYZ -+-, no posswap.
    case velvetFamily
    /// smooth, or any tree whose posswap equals its axis map XYZ -+-.
    case matched
    /// thorny: axis XYZ +++, no posswap.
    case thorny

    /// Turn the orientation by 180 degrees about +Y (by conjugation).
    public var turnsOrientation: Bool { self == .thorny }
    /// Turn the position by 180 degrees about +Y.
    public var turnsPosition: Bool { self != .matched }

    public var title: String {
        switch self {
        case .velvetFamily: return "Velvet, Spiny, Honey, VelvetGreen"
        case .matched: return "Smooth (or a tree with a matching posswap)"
        case .thorny: return "Thorny"
        }
    }

    /// What goes on the wire for a reported pose.
    public func wire(position p: Vec3, orientation q: Quat) -> (position: Vec3, orientation: Quat) {
        // A 180-degree turn about +Y maps (x, y, z) -> (-x, y, -z) for a
        // vector, and the same on a quaternion's vector part for conjugation.
        let pos = turnsPosition ? Vec3(-p.x, p.y, -p.z) : p
        let ori = turnsOrientation ? Quat(-q.x, q.y, -q.z, q.w) : q
        return (pos, ori)
    }
}
