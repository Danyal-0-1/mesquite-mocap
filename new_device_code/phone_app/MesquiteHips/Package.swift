// swift-tools-version: 5.10
// =========================================================================
//  MESQUITE HIPS  --  the iPhone app that is the suit's "Hips" device.
//
//  WHY THIS IS A PLAIN SWIFT PACKAGE
//  xtool (https://github.com/xtool-org/xtool) builds an iOS app from a
//  SwiftPM package on Linux, so there is no .xcodeproj, no storyboard and
//  no asset catalog: none of those can be built outside macOS. xtool wraps
//  the ONE library product below into the app bundle; xtool.yml names the
//  bundle and Info.plist adds the permission strings.
//
//  The same sources build on a desktop: everything under
//  Sources/MesquiteHips/Core is plain Swift + Foundation, and the iOS
//  screens compile only for iOS (#if os(iOS)). The checks in Checks/ are a
//  plain executable built by ../tools/run_checks.sh with swiftc alone, so
//  they run on Linux and macOS without XCTest or a working SwiftPM test
//  setup. See ../CODE.md.
//
//  Tools version 5.10 on purpose: it keeps the Swift 5 language mode with
//  any Swift 6 compiler. ARKit and Network.framework call back on their own
//  queues, and the app keeps each piece of state on one queue
//  (App/PhoneController.swift) rather than using Swift 6's actor checking.
// =========================================================================
import PackageDescription

let package = Package(
    name: "MesquiteHips",
    platforms: [
        .iOS(.v16),
        .macOS(.v13),
    ],
    products: [
        // An xtool project contains exactly one library product: the app.
        .library(
            name: "MesquiteHips",
            targets: ["MesquiteHips"]
        ),
    ],
    targets: [
        .target(name: "MesquiteHips"),
    ]
)
