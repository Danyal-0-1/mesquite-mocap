// =========================================================================
//  THE HUB MESSAGE  --  Core/HubMessage.swift
//
//  WHY THIS FILE EXISTS
//  This is the whole contract between the phone and the rest of Mesquite.
//  The page only understands one phone message, the one the original phone
//  sender used:
//
//      {"bone":"Hips","x":..,"y":..,"z":..,"w":..,"px":..,"py":..,"pz":..}
//
//  sent as ONE WebSocket text frame to the hub, which appends '\n' and
//  forwards it down the USB stream (new_device_code/Dongle_Binary/
//  Dongle_Binary.ino, handleWebSocketMessage). The browser parser
//  (js/mesq_parser.js, Branch B) turns it into the object that
//  js/custom_icm.js handleWSMessage() reads.
//
//  PROTOCOL: three limits, all silent when broken, shape every byte here.
//    1. 95 bytes. The hub queues records of at most MESQ_USB_REC_MAX = 96
//       bytes including the '\n' it adds (mesq_hub_core.h:173), and returns
//       without a counter for anything longer (Dongle_Binary.ino:488).
//    2. No exact zeros in px, py, pz. The page tests them for truthiness
//       (js/custom_icm.js:931), so a coordinate of 0 drops the frame
//       (PHN-01). After Zero the hips start AT x = z = 0.
//    3. x, y, z, w must be finite numbers, or the parser drops the line
//       (mesq_parser.js commitJsonLine, nanFrames).
//
//  MATH: precision ladder. The first level that fits in 95 bytes is used:
//    level 0: quaternion 4 decimals (0.011 deg), position 3 decimals (1 mm)
//    level 1: quaternion 3 decimals (0.11 deg),  position 3 decimals
//    level 2: quaternion 3 decimals,             position 2 decimals (1 cm)
//    level 3: quaternion 4 decimals, no position (the page keeps the root)
//  Trailing zeros are stripped, so near the T-pose level 0 nearly always
//  fits. Level 2 is only needed beyond 10 m from the Zero point and level 3
//  beyond 100 m. MATH.md section 5 derives the byte counts.
// =========================================================================
import Foundation

public enum HubMessage {
    /// MESQ_USB_REC_MAX (96) minus the '\n' the hub appends.
    public static let maxBytes = 95

    /// (quaternion decimals, position decimals or nil for orientation only)
    public static let levels: [(q: Int, p: Int?)] = [(4, 3), (3, 3), (3, 2), (4, nil)]

    public struct Encoded: Equatable, Sendable {
        public var text: String
        public var level: Int
        public var bytes: Int { text.utf8.count }
        public var hasPosition: Bool { HubMessage.levels[level].p != nil }
    }

    /// Encode one pose. `position` nil means orientation only (used when
    /// tracking is limited, so the page holds the root where it is).
    /// Returns nil only for a non-finite input.
    public static func encode(orientation q: Quat, position p: Vec3?) -> Encoded? {
        guard q.isFinite, q.norm > 0 else { return nil }
        let u = q.normalized
        let pos = (p?.isFinite ?? false) ? p : nil
        // Orientation-only is the last resort for a pose that has a position,
        // and the only choice for one that has none.
        for (index, level) in levels.enumerated() {
            if pos == nil && level.p != nil { continue }
            guard let text = format(u, pos, level.q, level.p) else { continue }
            if text.utf8.count <= maxBytes { return Encoded(text: text, level: index) }
        }
        return nil
    }

    static func format(_ q: Quat, _ p: Vec3?, _ qDec: Int, _ pDec: Int?) -> String? {
        guard let x = fixed(q.x, qDec), let y = fixed(q.y, qDec),
              let z = fixed(q.z, qDec), let w = fixed(q.w, qDec) else { return nil }
        var s = "{\"bone\":\"Hips\",\"x\":\(x),\"y\":\(y),\"z\":\(z),\"w\":\(w)"
        if let p, let pDec {
            guard let px = fixed(p.x, pDec, avoidZero: true),
                  let py = fixed(p.y, pDec, avoidZero: true),
                  let pz = fixed(p.z, pDec, avoidZero: true) else { return nil }
            s += ",\"px\":\(px),\"py\":\(py),\"pz\":\(pz)"
        }
        return s + "}"
    }

    private static let powersOfTen: [Int64] = [1, 10, 100, 1_000, 10_000, 100_000, 1_000_000]

    /// A JSON number with at most `decimals` decimals, trailing zeros removed:
    /// 0.5 -> "0.5", 1 -> "1", -0.00004 at 4 decimals -> "0" (never "-0").
    /// Integer arithmetic on purpose: no locale, no exponent ("1e-05" is legal
    /// JSON but wastes bytes), and the rounding is the same on every platform.
    ///
    /// `avoidZero`: a value that rounds to 0 is sent as one step of its own
    /// sign instead (0.001 at 3 decimals), because the page drops a position
    /// with a 0 in it (limit 2 above). The error is at most one step.
    static func fixed(_ v: Double, _ decimals: Int, avoidZero: Bool = false) -> String? {
        guard v.isFinite, decimals >= 0, decimals < powersOfTen.count else { return nil }
        let scale = powersOfTen[decimals]
        let scaled = (v * Double(scale)).rounded()
        guard abs(scaled) < 9e15 else { return nil }
        var n = Int64(scaled)
        if n == 0 && avoidZero { n = v < 0 ? -1 : 1 }
        let negative = n < 0
        let a = negative ? -n : n
        var s = negative ? "-" : ""
        s += String(a / scale)
        var fraction = a % scale
        if fraction != 0 {
            var digits = decimals
            while fraction % 10 == 0 {
                fraction /= 10
                digits -= 1
            }
            let f = String(fraction)
            s += "." + String(repeating: "0", count: digits - f.count) + f
        }
        return s
    }
}
