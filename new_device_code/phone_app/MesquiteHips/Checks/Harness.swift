// =========================================================================
//  CHECK HARNESS  --  Checks/Harness.swift
//
//  WHY NOT XCTEST: the checks must run on a Linux laptop and on a Mac with
//  only the Command Line Tools, and both of those can lack a working
//  XCTest. A check here is a named boolean; the run prints every result and
//  exits non-zero if any failed. ../tools/run_checks.sh builds it with
//  swiftc from the app's own Core sources.
// =========================================================================
import Foundation

final class Report {
    private(set) var passed = 0
    private(set) var failed: [String] = []
    private var group = ""

    func section(_ name: String) {
        group = name
        print("\n== \(name)")
    }

    func check(_ ok: Bool, _ name: String, _ detail: @autoclosure () -> String = "") {
        let d = detail()
        if ok {
            passed += 1
            print("  ok    \(name)\(d.isEmpty ? "" : "  [\(d)]")")
        } else {
            failed.append("\(group): \(name)")
            print("  FAIL  \(name)\(d.isEmpty ? "" : "  [\(d)]")")
        }
    }

    func note(_ text: String) { print("        \(text)") }

    func finish() -> Int32 {
        print("\n\(passed) passed, \(failed.count) failed")
        for f in failed { print("  failed: \(f)") }
        return failed.isEmpty ? 0 : 1
    }
}

let report = Report()

func deg(_ r: Double) -> Double { r * 180 / .pi }
func rad(_ d: Double) -> Double { d * .pi / 180 }
func f(_ v: Double, _ n: Int = 3) -> String { String(format: "%.\(n)f", v) }
func fv(_ v: Vec3, _ n: Int = 3) -> String { "(\(f(v.x, n)), \(f(v.y, n)), \(f(v.z, n)))" }

/// A small deterministic random source, so every run checks the same cases.
struct Rand {
    private var state: UInt64
    init(_ seed: UInt64) { state = seed &* 6364136223846793005 &+ 1442695040888963407 }
    mutating func next() -> Double {
        state = state &* 6364136223846793005 &+ 1442695040888963407
        return Double(state >> 11) / Double(1 << 53)
    }
    mutating func uniform(_ a: Double, _ b: Double) -> Double { a + (b - a) * next() }
    mutating func gauss() -> Double {
        let u = max(next(), 1e-300), v = next()
        return (-2 * log(u)).squareRoot() * cos(2 * .pi * v)
    }
    /// A uniformly random rotation (Shoemake).
    mutating func rotation() -> Quat {
        let u1 = next(), u2 = next(), u3 = next()
        let a = (1 - u1).squareRoot(), b = u1.squareRoot()
        return Quat(a * sin(2 * .pi * u2), a * cos(2 * .pi * u2), b * sin(2 * .pi * u3), b * cos(2 * .pi * u3))
    }
}
