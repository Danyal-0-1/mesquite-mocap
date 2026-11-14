// =========================================================================
//  SETTINGS  --  Core/Settings.swift
//
//  Everything the wearer can change, with the defaults that match the
//  system as it is in this repository. Codable so the iOS layer stores it
//  as one JSON value in UserDefaults; every field has a default so an older
//  stored value still decodes after a field is added.
// =========================================================================
import Foundation

public struct AppSettings: Codable, Equatable, Sendable {
    /// The hub's WebSocket. Its soft-AP serves 192.168.4.1 (the ESP32
    /// default) and AsyncWebSocket ws("/ws") on port 80 (Dongle_Binary.ino).
    public var hubURL = "ws://192.168.4.1:80/ws"
    public var rateHz = 30
    /// Must name the same tree as the page's suit drop-down.
    public var tree = PageTree.velvetFamily
    public var facing = CameraFacing.forward
    public var hipHeightCm = 100.0
    /// The manual lever arm: the hip centre is this far behind the screen.
    /// The screen faces the body, so "behind the screen" is the camera's +Z.
    public var leverDepthCm = 10.0
    /// A measured lever arm (camera frame, metres). It wins over the depth.
    public var measuredLever: Vec3?
    public var logging = true
    public var spokenCues = true
    public var countdownSeconds = 8

    public init() {}

    public var lever: Vec3 { measuredLever ?? Vec3(0, 0, leverDepthCm / 100) }

    public static let rates = [15, 30, 60]

    enum CodingKeys: String, CodingKey {
        case hubURL, rateHz, tree, facing, hipHeightCm, leverDepthCm, measuredLever,
             logging, spokenCues, countdownSeconds
    }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        let d = AppSettings()
        hubURL = try c.decodeIfPresent(String.self, forKey: .hubURL) ?? d.hubURL
        rateHz = try c.decodeIfPresent(Int.self, forKey: .rateHz) ?? d.rateHz
        tree = try c.decodeIfPresent(PageTree.self, forKey: .tree) ?? d.tree
        facing = try c.decodeIfPresent(CameraFacing.self, forKey: .facing) ?? d.facing
        hipHeightCm = try c.decodeIfPresent(Double.self, forKey: .hipHeightCm) ?? d.hipHeightCm
        leverDepthCm = try c.decodeIfPresent(Double.self, forKey: .leverDepthCm) ?? d.leverDepthCm
        measuredLever = try c.decodeIfPresent(Vec3.self, forKey: .measuredLever)
        logging = try c.decodeIfPresent(Bool.self, forKey: .logging) ?? d.logging
        spokenCues = try c.decodeIfPresent(Bool.self, forKey: .spokenCues) ?? d.spokenCues
        countdownSeconds = try c.decodeIfPresent(Int.self, forKey: .countdownSeconds) ?? d.countdownSeconds
    }

    /// The pipeline these settings describe.
    public func makePipeline() -> PosePipeline {
        PosePipeline(frame: HipsFrame(lever: lever, hipHeight: hipHeightCm / 100, facing: facing),
                     tree: tree, rateHz: Double(rateHz))
    }
}
