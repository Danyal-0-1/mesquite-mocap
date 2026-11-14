// =========================================================================
//  LEVER-ARM FIT  --  Core/LeverArmFit.swift
//
//  WHY THIS FILE EXISTS
//  The phone is not at the hip joint. When the wearer turns on the spot the
//  phone swings on a circle around the hip centre, and the page takes the
//  phone's position as the hips' position (js/custom_icm.js, the root
//  target), so the swing is drawn as travel: a 180-degree turn with the
//  phone 12 cm in front of the hip centre moves the hips 24 cm. Removing it
//  needs the lever arm l, the vector from the camera to the hip centre in the
//  phone's own frame, which stays constant while the strap holds.
//
//  MATH: pivot calibration, the standard method for tracked tools. While the
//  wearer turns on the spot the hip centre c stays put, so every sample gives
//      p(t) + R(t) l = c            (3 equations; unknowns l and c)
//  Least squares over N samples, with c eliminated, leaves a 3x3 system
//      [(1 + lambda) I - Rbar^T Rbar] l = lambda l0 - (1/N) sum R^T (p - pbar)
//  where Rbar and pbar are the means of R(t) and p(t) (MATH.md section 4).
//  Turning about the vertical cannot reveal the part of l along the
//  vertical: it adds the same height to every sample and c absorbs it. The
//  small ridge lambda pulls that part to the prior l0 (the manual depth)
//  instead of letting noise choose it; the turned directions are barely
//  affected (by a factor 1/(1 + lambda)).
// =========================================================================
import Foundation

public struct LeverArmFit: Sendable {
    public struct Result: Equatable, Sendable {
        /// Camera -> hip centre, camera frame, metres.
        public var lever: Vec3
        /// The hip centre the fit found, ARKit world, metres.
        public var pivot: Vec3
        /// Root-mean-square distance of the corrected samples from the pivot.
        public var rms: Double
        /// How far the heading travelled (max - min of the unwrapped heading).
        public var headingRange: Double
        public var samples: Int
    }

    public enum Failure: Error, Equatable {
        case tooFewSamples(Int)
        case notEnoughTurn(degrees: Double)
        case implausibleLever(metres: Double)
        case poorFit(rmsMetres: Double)
    }

    /// The manual setting the vertical part falls back to.
    public var prior: Vec3
    public private(set) var count = 0

    // Running sums: the fit needs only these, never the samples themselves,
    // so a long turn costs constant memory.
    private var sumP = Vec3.zero
    private var sumR = [[Double]](repeating: [0, 0, 0], count: 3)    // sum of R, row-major
    private var sumRtP = Vec3.zero                                    // sum of R^T p
    private var sumPP = 0.0                                           // sum of |p|^2
    private var unwrapped: Double?
    private var lastHeading: Double?
    private var minHeading = 0.0
    private var maxHeading = 0.0

    public init(prior: Vec3 = .zero) { self.prior = prior }

    public var headingRange: Double { maxHeading - minHeading }

    public mutating func add(camPosition p: Vec3, camRotation q: Quat) {
        guard p.isFinite, q.isFinite else { return }
        let r = q.normalized
        let (c0, c1, c2) = r.columns
        let cols = [c0, c1, c2]
        for row in 0..<3 {
            for col in 0..<3 {
                let v = cols[col]
                sumR[row][col] += row == 0 ? v.x : (row == 1 ? v.y : v.z)
            }
        }
        sumP = sumP + p
        sumRtP = sumRtP + r.conjugate.rotate(p)
        sumPP += p.dot(p)
        count += 1

        // Unwrapped heading of the camera's view, to measure how far the
        // wearer actually turned.
        if let h = r.rotate(Vec3(0, 0, -1)).heading(minHorizontal: HipsFrame.minHorizontal) {
            if let last = lastHeading, let u = unwrapped {
                let next = u + remainder(h - last, 2 * .pi)
                unwrapped = next
                minHeading = min(minHeading, next)
                maxHeading = max(maxHeading, next)
            } else {
                unwrapped = 0
                minHeading = 0
                maxHeading = 0
            }
            lastHeading = h
        }
    }

    /// Solve. The defaults ask for a half turn or more, a lever under 50 cm
    /// and a fit within 3 cm RMS; HOW_IT_WORKS.md says how to turn.
    public func solve(minSamples: Int = 60, minHeadingRange: Double = .pi,
                      ridge lambda: Double = 1e-3, maxLever: Double = 0.5,
                      maxRMS: Double = 0.03) -> Swift.Result<Result, Failure> {
        guard count >= minSamples else { return .failure(.tooFewSamples(count)) }
        guard headingRange >= minHeadingRange else {
            return .failure(.notEnoughTurn(degrees: headingRange * 180 / .pi))
        }
        let n = Double(count)
        let pBar = (1 / n) * sumP
        var rBar = sumR
        for i in 0..<3 { for j in 0..<3 { rBar[i][j] /= n } }

        // M = (1 + lambda) I - Rbar^T Rbar
        var m = [[Double]](repeating: [0, 0, 0], count: 3)
        for i in 0..<3 {
            for j in 0..<3 {
                var s = 0.0
                for k in 0..<3 { s += rBar[k][i] * rBar[k][j] }
                m[i][j] = (i == j ? 1 + lambda : 0) - s
            }
        }
        // rhs = lambda l0 - (1/N) sum R^T p + Rbar^T pbar
        let rtPBar = Vec3(rBar[0][0] * pBar.x + rBar[1][0] * pBar.y + rBar[2][0] * pBar.z,
                          rBar[0][1] * pBar.x + rBar[1][1] * pBar.y + rBar[2][1] * pBar.z,
                          rBar[0][2] * pBar.x + rBar[1][2] * pBar.y + rBar[2][2] * pBar.z)
        let rhs = lambda * prior - (1 / n) * sumRtP + rtPBar
        let lever = solve3(m, rhs)
        guard lever.isFinite, lever.length <= maxLever else {
            return .failure(.implausibleLever(metres: lever.length))
        }
        // c = pbar + Rbar l
        let rl = Vec3(rBar[0][0] * lever.x + rBar[0][1] * lever.y + rBar[0][2] * lever.z,
                      rBar[1][0] * lever.x + rBar[1][1] * lever.y + rBar[1][2] * lever.z,
                      rBar[2][0] * lever.x + rBar[2][1] * lever.y + rBar[2][2] * lever.z)
        let pivot = pBar + rl
        // sum |p + R l - c|^2 expanded into the running sums:
        //   sum|p|^2 + N|l|^2 + N|c|^2 + 2 (sum R^T p).l - 2 (sum p).c - 2 N (Rbar l).c
        let sq = sumPP + n * lever.dot(lever) + n * pivot.dot(pivot) + 2 * sumRtP.dot(lever)
            - 2 * sumP.dot(pivot) - 2 * n * rl.dot(pivot)
        let rms = (max(0, sq) / n).squareRoot()
        guard rms <= maxRMS else { return .failure(.poorFit(rmsMetres: rms)) }
        return .success(Result(lever: lever, pivot: pivot, rms: rms,
                               headingRange: headingRange, samples: count))
    }

    /// Cramer's rule. M is symmetric positive definite for lambda > 0 (the
    /// eigenvalues of Rbar^T Rbar are at most 1), so the determinant is at
    /// least lambda^3 and never zero.
    private func solve3(_ m: [[Double]], _ b: Vec3) -> Vec3 {
        func det(_ a: [[Double]]) -> Double {
            a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1])
                - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0])
                + a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0])
        }
        let d = det(m)
        let bv = [b.x, b.y, b.z]
        var out = [0.0, 0.0, 0.0]
        for c in 0..<3 {
            var a = m
            for r in 0..<3 { a[r][c] = bv[r] }
            out[c] = det(a) / d
        }
        return Vec3(out[0], out[1], out[2])
    }
}
