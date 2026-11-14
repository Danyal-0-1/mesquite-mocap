// =========================================================================
//  THE SESSION LOG  --  Core/SessionLog.swift
//
//  WHY THIS FILE EXISTS
//  The 95-byte hub message carries a pose and nothing else: no capture
//  time, no tracking state, no sequence. The page records neither (it
//  samples the smoothed avatar on a timer). So the phone keeps its own
//  full-rate record of every ARKit frame -- the raw camera pose, what was
//  reported, whether it was sent -- with the native capture time and a wall
//  clock to line it up with a page recording later.
//
//  FORMAT: CSV, one row per ARKit frame. Lines starting with '#' are
//  comments (pandas: read_csv(path, comment='#')):
//    "# key=value"            session settings, written once at the top
//    "#E,<t>,<text>"          an event: Zero, link state, tracking change
//  Columns (units in the names):
//    t_s        ARFrame.timestamp, device uptime clock, seconds
//    wall_s     Unix time = t_s + offset (offset fixed at session start)
//    epoch      Zero count; rows with different epochs are different frames
//    tracking   normal | limited:<reason> | not_available
//    cam_*      ARKit world camera pose (metres, unit quaternion)
//    hip_*      reported hips pose, empty before the first Zero
//    sent       1 if this frame's message went to the hub
//    level      HubMessage precision level, -1 if nothing was encoded
//    bytes      message length, 0 if none
//  Files are kept in the app's Documents folder, visible in the Files app
//  (Info.plist: UIFileSharingEnabled).
// =========================================================================
import Foundation

public final class SessionLog {
    public let url: URL
    public private(set) var bytesWritten = 0
    private let handle: FileHandle
    private var buffer = ""
    private let wallOffset: Double

    public static let columns = [
        "t_s", "wall_s", "epoch", "tracking",
        "cam_px_m", "cam_py_m", "cam_pz_m", "cam_qx", "cam_qy", "cam_qz", "cam_qw",
        "hip_px_m", "hip_py_m", "hip_pz_m", "hip_qx", "hip_qy", "hip_qz", "hip_qw",
        "sent", "level", "bytes",
    ]

    /// - Parameters:
    ///   - wallOffset: Unix time minus device uptime, measured once now.
    ///   - header: settings written as "# key=value" lines.
    public init(directory: URL, name: String, wallOffset: Double, header: [(String, String)]) throws {
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        url = directory.appendingPathComponent(name)
        guard FileManager.default.createFile(atPath: url.path, contents: nil) else {
            throw CocoaError(.fileWriteUnknown)
        }
        handle = try FileHandle(forWritingTo: url)
        self.wallOffset = wallOffset
        var top = "# Mesquite Hips session log, format 1\n"
        for (k, v) in header { top += "# \(k)=\(v)\n" }
        top += "# wall_s = t_s + \(String(format: "%.6f", wallOffset))\n"
        top += Self.columns.joined(separator: ",") + "\n"
        write(top)
    }

    public static func fileName(start: Date) -> String {
        let f = DateFormatter()
        f.locale = Locale(identifier: "en_US_POSIX")
        f.dateFormat = "yyyyMMdd_HHmmss"
        return "hips_\(f.string(from: start)).csv"
    }

    public func event(_ time: Double, _ text: String) {
        let clean = text.replacingOccurrences(of: "\n", with: " ")
        write("#E,\(String(format: "%.6f", time)),\(clean)\n")
    }

    public func frame(_ f: FrameInput, epoch: Int, reported: (position: Vec3, orientation: Quat)?,
                      sent: Bool, message: HubMessage.Encoded?) {
        // No %@ here: String(format:) with a Swift String is not portable to Linux.
        var row = String(format: "%.6f,%.3f,%d,", f.time, f.time + wallOffset, epoch) + f.tracking.label
        let p = f.camPosition, q = f.camRotation
        row += String(format: ",%.5f,%.5f,%.5f,%.6f,%.6f,%.6f,%.6f", p.x, p.y, p.z, q.x, q.y, q.z, q.w)
        if let r = reported {
            let hp = r.position, hq = r.orientation
            row += String(format: ",%.5f,%.5f,%.5f,%.6f,%.6f,%.6f,%.6f",
                          hp.x, hp.y, hp.z, hq.x, hq.y, hq.z, hq.w)
        } else {
            row += ",,,,,,,"
        }
        row += ",\(sent ? 1 : 0),\(message?.level ?? -1),\(message?.bytes ?? 0)\n"
        write(row)
    }

    private func write(_ s: String) {
        buffer += s
        if buffer.utf8.count >= 32_768 { flush() }
    }

    /// Writes what is buffered. The iOS layer also calls this twice a second
    /// and when the app leaves the foreground, so a crash loses little.
    public func flush() {
        guard !buffer.isEmpty else { return }
        let data = Data(buffer.utf8)
        try? handle.write(contentsOf: data)
        bytesWritten += data.count
        buffer = ""
    }

    public func close() {
        flush()
        try? handle.close()
    }
}
