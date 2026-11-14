// =========================================================================
//  GEOMETRY  --  Core/Geometry.swift
//
//  WHY THIS FILE EXISTS
//  ARKit hands the app simd_float4x4 transforms, but simd is Apple-only.
//  Keeping the math in plain Double structs means every formula the app
//  relies on also compiles and is tested on Linux (`swift test`), where the
//  iOS SDK is used to build the app but nothing can run it.
//
//  CONVENTIONS -- the same as three.js, which is what the page uses:
//    - A quaternion is stored (x, y, z, w); w is the scalar part.
//    - a * b is the Hamilton product: rotate by b first, then by a
//      (three.js multiplyQuaternions(a, b); ARKit's simd_quatf agrees).
//    - A unit quaternion q rotates a vector as v' = q v q^-1.
//    - Frames are right-handed and +Y is up everywhere: ARKit's
//      gravity-aligned world, the reported hips frame and the page's scene.
//    - Angles are radians unless a name says otherwise.
// =========================================================================
import Foundation

public struct Vec3: Equatable, Codable, Sendable {
    public var x: Double
    public var y: Double
    public var z: Double

    public init(_ x: Double, _ y: Double, _ z: Double) {
        self.x = x
        self.y = y
        self.z = z
    }

    public static let zero = Vec3(0, 0, 0)
    public static let up = Vec3(0, 1, 0)

    public static func + (a: Vec3, b: Vec3) -> Vec3 { Vec3(a.x + b.x, a.y + b.y, a.z + b.z) }
    public static func - (a: Vec3, b: Vec3) -> Vec3 { Vec3(a.x - b.x, a.y - b.y, a.z - b.z) }
    public static prefix func - (a: Vec3) -> Vec3 { Vec3(-a.x, -a.y, -a.z) }
    public static func * (s: Double, a: Vec3) -> Vec3 { Vec3(s * a.x, s * a.y, s * a.z) }

    public func dot(_ b: Vec3) -> Double { x * b.x + y * b.y + z * b.z }
    public func cross(_ b: Vec3) -> Vec3 { Vec3(y * b.z - z * b.y, z * b.x - x * b.z, x * b.y - y * b.x) }
    public var length: Double { dot(self).squareRoot() }
    public var isFinite: Bool { x.isFinite && y.isFinite && z.isFinite }

    /// The horizontal heading of this direction, about +Y, measured from -Z:
    /// 0 for -Z, +pi/2 for -X. That is the turn a person facing -Z makes to
    /// face this way, positive to the LEFT (right-hand rule about +Y).
    /// Nil when the direction is too close to vertical to have a heading.
    public func heading(minHorizontal: Double = 1e-9) -> Double? {
        let h = Vec3(x, 0, z)
        guard h.length >= minHorizontal else { return nil }
        return atan2(-x, -z)
    }
}

public struct Quat: Equatable, Codable, Sendable {
    public var x: Double
    public var y: Double
    public var z: Double
    public var w: Double

    public init(_ x: Double, _ y: Double, _ z: Double, _ w: Double) {
        self.x = x
        self.y = y
        self.z = z
        self.w = w
    }

    public static let identity = Quat(0, 0, 0, 1)

    /// A rotation of `angle` about `axis` (right-hand rule). `axis` need not be unit.
    public init(axis: Vec3, angle: Double) {
        let s = sin(angle / 2) / axis.length
        self.init(axis.x * s, axis.y * s, axis.z * s, cos(angle / 2))
    }

    /// A turn about the vertical. Positive turns -Z toward -X: a LEFT turn for
    /// someone facing -Z.
    public static func yaw(_ angle: Double) -> Quat { Quat(axis: .up, angle: angle) }

    public static func * (a: Quat, b: Quat) -> Quat {
        Quat(a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
             a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
             a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
             a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z)
    }

    /// The inverse of a UNIT quaternion.
    public var conjugate: Quat { Quat(-x, -y, -z, w) }
    public var negated: Quat { Quat(-x, -y, -z, -w) }
    public var norm: Double { (x * x + y * y + z * z + w * w).squareRoot() }
    public var isFinite: Bool { x.isFinite && y.isFinite && z.isFinite && w.isFinite }

    public var normalized: Quat {
        let n = norm
        return n > 0 ? Quat(x / n, y / n, z / n, w / n) : .identity
    }

    public func dot(_ b: Quat) -> Double { x * b.x + y * b.y + z * b.z + w * b.w }

    /// The angle between two orientations, 0...pi. q and -q are the same
    /// orientation, hence the absolute value. atan2 of the relative turn,
    /// not acos of the dot product: acos loses half its digits near 1, which
    /// would put a 1e-8 rad floor under every small angle.
    public func angle(to b: Quat) -> Double {
        let d = normalized.conjugate * b.normalized
        return 2 * atan2(Vec3(d.x, d.y, d.z).length, abs(d.w))
    }

    /// v' = q v q^-1 for a unit q, expanded: t = 2 (u x v), v' = v + w t + u x t.
    public func rotate(_ v: Vec3) -> Vec3 {
        let u = Vec3(x, y, z)
        let t = 2 * u.cross(v)
        return v + w * t + u.cross(t)
    }

    /// From an orthonormal rotation matrix given by its three columns: the
    /// images of +X, +Y and +Z. That is exactly how ARKit lays out a
    /// simd_float4x4 -- columns 0...2 are the camera's axes in world
    /// coordinates and column 3 is its position.
    ///
    /// MATH: Shepperd's method. It divides by the largest of the four
    /// candidate terms, so the square root never nears zero. The branches are
    /// those of three.js Quaternion.setFromRotationMatrix.
    public init(columns c0: Vec3, _ c1: Vec3, _ c2: Vec3) {
        let m00 = c0.x, m10 = c0.y, m20 = c0.z
        let m01 = c1.x, m11 = c1.y, m21 = c1.z
        let m02 = c2.x, m12 = c2.y, m22 = c2.z
        let trace = m00 + m11 + m22
        let q: Quat
        if trace > 0 {
            let s = 0.5 / (trace + 1).squareRoot()
            q = Quat((m21 - m12) * s, (m02 - m20) * s, (m10 - m01) * s, 0.25 / s)
        } else if m00 > m11 && m00 > m22 {
            let s = 2 * (1 + m00 - m11 - m22).squareRoot()
            q = Quat(0.25 * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s)
        } else if m11 > m22 {
            let s = 2 * (1 + m11 - m00 - m22).squareRoot()
            q = Quat((m01 + m10) / s, 0.25 * s, (m12 + m21) / s, (m02 - m20) / s)
        } else {
            let s = 2 * (1 + m22 - m00 - m11).squareRoot()
            q = Quat((m02 + m20) / s, (m12 + m21) / s, 0.25 * s, (m10 - m01) / s)
        }
        self = q.normalized
    }

    /// The columns of the rotation matrix: the images of +X, +Y, +Z.
    public var columns: (Vec3, Vec3, Vec3) {
        (rotate(Vec3(1, 0, 0)), rotate(Vec3(0, 1, 0)), rotate(Vec3(0, 0, 1)))
    }
}
