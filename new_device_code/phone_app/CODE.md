# The code of Mesquite Hips

**Picture it.** Three layers, three threads.

- At the bottom, **Core**: plain Swift that knows the mathematics and the message format. It runs anywhere, including
  your Linux laptop, so all of it is tested there.
- Beside it, **Transport**: the WebSocket link. It uses Apple's Network.framework, so it runs on the iPhone and on a
  Mac, where it is tested against a stand-in hub.
- On top, **App**: the iPhone-only part. ARKit in, a SwiftUI screen out, and a controller that only decides *when*
  things run, never *what* is sent.

```
App (iOS only)            MesquiteHipsApp ── ContentView, SettingsView        Cues (voice, vibration)
                                    │                 ▲ @Published status, 10 Hz
                                    ▼                 │
                          PhoneController ── ARSession (frames on the frame queue)
                                    │ FrameInput
Core (any platform)       PosePipeline ── HipsFrame ── HubMessage          SessionLog   LeverArmFit
                                    │ "{"bone":"Hips",…}"                       Settings     Geometry
Transport (Apple)         HubLink (Network.framework WebSocket, its own queue) ──▶ hub
```

## Contents

1. [What it is built with, and why](#1-what-it-is-built-with-and-why)
2. [Where everything is](#2-where-everything-is)
3. [File by file](#3-file-by-file)
4. [Threads and state](#4-threads-and-state)
5. [The checks](#5-the-checks)
6. [Changing things safely](#6-changing-things-safely)
7. [How the code is written](#7-how-the-code-is-written)
8. [Learning the pieces](#8-learning-the-pieces)

---

## 1. What it is built with, and why

| Piece | Used for | Why this one |
|---|---|---|
| **Swift**: a Swift 6 compiler, the Swift 5 language mode (`swift-tools-version: 5.10`) | everything | the language of Apple's SDKs. Swift 5 mode keeps Swift 6's strict data-race checking from turning ARKit's and Network.framework's callback queues into compile errors. The app keeps each piece of state on one queue instead ([§4](#4-threads-and-state)). Moving to Swift 6 mode is a refactor, described in [§6](#6-changing-things-safely) |
| **SwiftUI** | the screens | declarative, and needs no storyboard or asset catalog, which cannot be compiled outside macOS. Uses iOS 16 features (`NavigationStack`, `ShareLink`, `LabeledContent`), so iOS 16 is the minimum |
| **ARKit** (`ARSession`, `ARWorldTrackingConfiguration`) | where the phone is | Apple's visual-inertial tracking, with position, heading, a tracking state and capture timestamps. No view is attached: the session runs headless and only the pose is used |
| **Network.framework** (`NWConnection`, `NWProtocolWebSocket`) | talking to the hub | it is not subject to App Transport Security, so plain `ws://` to `192.168.4.1` needs no exemptions. It can refuse the cellular interface, and it exposes TCP no-delay and keepalive. It ships with iOS and macOS, so the same file is tested on a Mac |
| **AVFoundation** (`AVSpeechSynthesizer`, `AVAudioSession`), UIKit haptics | spoken countdowns | the screen is against your body while you use the app |
| **Foundation** | files, JSON, dates | available on Linux too, so Core needs nothing else |
| **Swift Package Manager** | the project | what xtool builds; one manifest instead of an Xcode project |
| **xtool** | build, sign and install from Linux | builds the package with the iOS SDK it extracts from `Xcode.xip`, signs with your Apple ID, and installs over USB ([BUILD_ON_LINUX.md](BUILD_ON_LINUX.md)) |
| **Node.js**, no packages | the hub stand-in and its checks | runs the page's own parser module, `js/mesq_parser.js` |

There are **no third-party Swift packages**, so nothing has to be downloaded or resolved at build time, which keeps the
Linux build simple.

## 2. Where everything is

```
new_device_code/phone_app/
├── README.md  HOW_IT_WORKS.md  MATH.md  CODE.md  BUILD_ON_LINUX.md
├── MesquiteHips/                      ← run xtool here
│   ├── Package.swift                  one library product = the app
│   ├── xtool.yml                      bundle id, Info.plist and icon for xtool
│   ├── Info.plist                     only what the app adds (permissions, name, file sharing)
│   ├── Icon.png                       512×512 app icon
│   ├── Sources/MesquiteHips/
│   │   ├── Core/                      portable: Geometry, HipsFrame, HubMessage, LeverArmFit,
│   │   │                              PosePipeline, SessionLog, Settings
│   │   ├── Transport/HubLink.swift    Network.framework WebSocket (Apple platforms)
│   │   └── App/                       iOS only: MesquiteHipsApp, PhoneController, ContentView,
│   │                                  SettingsView, Cues
│   └── Checks/                        a plain executable: Harness, Simulator, CoreChecks,
│                                      SessionChecks, Scenarios, LinkCheck, main
└── tools/
    ├── run_checks.sh                  builds and runs every check that needs no iPhone
    └── mock_hub.js                    the hub's phone path + the page's parser, for testing
```

## 3. File by file

### The package

- **[Package.swift](MesquiteHips/Package.swift)** declares one library product, `MesquiteHips`. xtool requires exactly
  one: it generates a small executable that links this library and calls its `@main`. The platforms are iOS 16 and
  macOS 13; macOS is listed only so the sources build on a Mac.
- **[xtool.yml](MesquiteHips/xtool.yml)**: `bundleID` (`cc.mesquite.hips`), `infoPath` and `iconPath`.
- **[Info.plist](MesquiteHips/Info.plist)** is merged by xtool over the one it generates. It adds:
  - the display name;
  - the **camera** and **local network** permission texts. iOS refuses both without them;
  - landscape orientations;
  - `UIFileSharingEnabled` and `LSSupportsOpeningDocumentsInPlace`, so the logs appear in the Files app;
  - `NSAllowsLocalNetworking`, a harmless belt-and-braces for local cleartext.

  xtool itself sets the bundle id, the executable, `MinimumOSVersion` (from Package.swift), `UILaunchScreen` (so the
  app is full screen) and `UIRequiredDeviceCapabilities = [arm64]`.

### Core: plain Swift, tested everywhere

| File | Types | Job |
|---|---|---|
| [Geometry.swift](MesquiteHips/Sources/MesquiteHips/Core/Geometry.swift) | `Vec3`, `Quat` | vectors and quaternions in `Double`: the Hamilton product, rotating a vector, matrix columns ⇄ quaternion (Shepperd), a stable angle, heading. `simd` is Apple-only, so it is not used here |
| [HipsFrame.swift](MesquiteHips/Sources/MesquiteHips/Core/HipsFrame.swift) | `CameraFacing`, `ZeroState`, `HipsFrame`, `PageTree` | the lever arm, the Zero ([MATH.md §3](MATH.md#3-the-zero-frame)), the reported pose, and the per-tree turn (`PageTree.wire`) |
| [HubMessage.swift](MesquiteHips/Sources/MesquiteHips/Core/HubMessage.swift) | `HubMessage`, `Encoded` | the JSON line: the precision ladder, the 95-byte limit, the zero nudge, number formatting with integers ([MATH.md §5](MATH.md#5-the-message-95-bytes)) |
| [LeverArmFit.swift](MesquiteHips/Sources/MesquiteHips/Core/LeverArmFit.swift) | `LeverArmFit`, `Result`, `Failure` | pivot calibration from running sums, its quality gates, and the heading covered ([MATH.md §4](MATH.md#4-the-lever-arm)) |
| [PosePipeline.swift](MesquiteHips/Sources/MesquiteHips/Core/PosePipeline.swift) | `Tracking`, `FrameInput`, `FrameOutput`, `PosePipeline` | one call per ARKit frame: tracking policy, automatic and requested Zero, the reported pose with continuous sign, the tree turn, encoding, the rate gate |
| [SessionLog.swift](MesquiteHips/Sources/MesquiteHips/Core/SessionLog.swift) | `SessionLog` | the CSV log: header, event lines, one row per frame, buffered writes |
| [Settings.swift](MesquiteHips/Sources/MesquiteHips/Core/Settings.swift) | `AppSettings` | every user setting with its default. Decoding falls back to the default for any missing field, so an older stored copy still loads. `makePipeline()` |

### Transport

**[HubLink.swift](MesquiteHips/Sources/MesquiteHips/Transport/HubLink.swift)** wraps one `NWConnection`: TCP with
no-delay, a 3 s connect timeout and keepalive; WebSocket on top, auto-answering pings; cellular refused.

- `start(url:)` and `stop()` run on the link's queue.
- `send(_:)` can be called from any thread. It returns `false` unless the link is ready and fewer than two sends are in
  flight, and it never queues.
- On `.waiting` or `.failed` the link tears down and reconnects after 0.5 s, then 1, 2 and 4 s.
- A `generation` counter makes callbacks from an old connection harmless.
- The whole file sits inside `#if canImport(Network)`, so it compiles to nothing on Linux.

### App: iOS only (`#if os(iOS)`)

- **[MesquiteHipsApp.swift](MesquiteHips/Sources/MesquiteHips/App/MesquiteHipsApp.swift)** is `@main`. It owns the one
  `PhoneController` and tells it when the app enters the background or becomes active.
- **[PhoneController.swift](MesquiteHips/Sources/MesquiteHips/App/PhoneController.swift)**, the glue:
  - *Commands, on main:* `start`, `stop`, `zeroWithCountdown`, `measureLever`, `setPocket`, `scenePhaseChanged`.
  - *ARKit delegate, on the frame queue:* `session(_:didUpdate:)` converts each `ARFrame` with `input(from:)`, then
    calls `process`:
    - the pipeline;
    - `HubLink.send`, and `markSent` only when the link took the message;
    - the log;
    - the lever fit, if one is running;
    - a status snapshot for the screen, ten times a second.
  - *Settings:* stored in `UserDefaults` as JSON, and copied to the frame queue on every change.
  - *The configuration:* world tracking with gravity alignment and nothing else.
- **[ContentView.swift](MesquiteHips/Sources/MesquiteHips/App/ContentView.swift)**: the status rows, the countdown, the
  buttons, the live numbers, sharing the log, and the black "pocket" overlay.
- **[SettingsView.swift](MesquiteHips/Sources/MesquiteHips/App/SettingsView.swift)**: a `Form` bound straight to
  `controller.settings`. The hub address is edited as a draft and applied with a button, so typing does not reconnect
  on every key.
- **[Cues.swift](MesquiteHips/Sources/MesquiteHips/App/Cues.swift)**: speech plus vibration. The audio session is
  `.playback`, so the cues sound with the silent switch on.

### Checks and tools

| File | Job |
|---|---|
| [Checks/Harness.swift](MesquiteHips/Checks/Harness.swift) | named checks, a report, a seeded random source and Shoemake's uniform random rotations |
| [Checks/Simulator.swift](MesquiteHips/Checks/Simulator.swift) | the truth: a wearer in a room, the phone strapped on in four ways, and the world an ARKit session started earlier would define |
| [Checks/CoreChecks.swift](MesquiteHips/Checks/CoreChecks.swift) | groups `geometry`, `hub message`, `reported frame`, `page trees` |
| [Checks/SessionChecks.swift](MesquiteHips/Checks/SessionChecks.swift) | groups `lever arm` and `pipeline`, on simulated 60 Hz takes |
| [Checks/Scenarios.swift](MesquiteHips/Checks/Scenarios.swift) | scripted takes (walk-lean-turn per tree, half turn with and without the lever arm, sway at the Zero point), written as JSON lines for replay into the hub stand-in or a page harness |
| [Checks/LinkCheck.swift](MesquiteHips/Checks/LinkCheck.swift) | the real `HubLink` streaming a take to a hub (stand-in or real) |
| [tools/mock_hub.js](tools/mock_hub.js) | a minimal RFC 6455 server applying `Dongle_Binary.ino`'s acceptance rule, feeding `js/mesq_parser.js`; live, `--replay`, `--self-test` |
| [tools/run_checks.sh](tools/run_checks.sh) | all of the above in order; `--e2e` and `--catalyst` on a Mac |

## 4. Threads and state

| Queue | Owns | Who calls in |
|---|---|---|
| **main** | the `@Published` UI state, the countdown, `Cues`, the screen's brightness, `settings` | SwiftUI; `DispatchQueue.main.async` from the frame queue |
| **frame queue** (`mesquite.frames`, ARKit's `delegateQueue`) | the `PosePipeline`, the `SessionLog`, the `LeverArmFit`, rate and level counters | ARKit (every frame); `frameQueue.async` from main for settings, Zero requests and starting a fit |
| **link queue** (`mesquite.hublink`) | the `NWConnection` and its reconnect timer | Network.framework callbacks; `start` and `stop` |

The link's few shared fields (state, in-flight count, statistics) sit behind one lock, because `send` is called from the
frame queue. That is why `HubLink` is declared `@unchecked Sendable`.

Two rules keep this safe:

- An `ARFrame` is reduced to a `FrameInput`, a few doubles, before anything else happens.
- UIKit is touched only on main. For example, the device name and iOS version for the log header are read in
  `start()` and handed to the frame queue as plain strings.

## 5. The checks

```sh
cd new_device_code/phone_app
tools/run_checks.sh                    # Linux or macOS: needs swiftc and node
tools/run_checks.sh --e2e --catalyst   # macOS: adds the network test and the iOS compile
```

| Stage | What it proves | Where it runs |
|---|---|---|
| Core checks (45) | the conventions match three.js and ARKit; every message fits and parses; the Zero frame, the lever fit, the rate gate and the tracking policy behave as [MATH.md](MATH.md) says | Linux, macOS |
| `mock_hub.js --self-test` | the stand-in applies the hub's rule: 95 bytes in, 96 out, no fragments, no binary | Linux, macOS |
| scenario replays | every message of six takes passes the hub rule and the page's parser, with no position skipped by the page's zero test | Linux, macOS |
| `--e2e` | the app's own Network.framework link delivers a whole take to the stand-in, byte for byte | macOS |
| `--catalyst` | the whole app, screens included, compiles and links against the real ARKit, SwiftUI, UIKit and AVFoundation | macOS |

**Why no XCTest.** The checks must run on Linux and on a Mac that has only the Command Line Tools. Either can lack a
usable XCTest, so `Checks/` is a plain executable that `run_checks.sh` builds with `swiftc` from the app's own Core
sources.

**The Catalyst trick.** The macOS SDK carries the iOS frameworks used by Mac Catalyst, under
`System/iOSSupport`. Compiling with `-target arm64-apple-ios16.0-macabi` therefore type-checks and links every ARKit,
SwiftUI and UIKit call against Apple's real interfaces, without Xcode and without an iPhone. It does not *run* ARKit:
a Mac cannot track. Running it is the job of the phone.

**If SwiftPM fails on a Mac.** If `swift build` fails on a Mac with only the Command Line Tools, with undefined
`PackageDescription` symbols for any package, the tools carry stale files from an older version. Reinstalling the
Command Line Tools fixes it. The checks do not use SwiftPM, so they run regardless.

## 6. Changing things safely

**Adding a field to the hub message.** Count the bytes first ([MATH.md §5.1](MATH.md#51-the-budget)).

- `"batt":0.85,` costs 12 bytes. Level 0 would then rarely fit, and the pose would lose precision to carry it.
- Prefer one of these:
  - put the information in the session log;
  - change the hub and the page together: raise `MESQ_USB_REC_MAX` in `mesq_hub_core.h` (it sizes a 128-slot queue,
    so check the hub's memory) and keep the parser's 512-byte line limit in mind.
- Then:
  - update `HubMessage.levels` and the expected keys in `checkHubMessage`;
  - run the 180,000-pose check;
  - replay the scenarios through `mock_hub.js`.

**Changing a tree, or the frame.** If `trees/<id>/meta.json` changes the `Hips` maps, update `PageTree` so that each
tree's orientation and position both come out along the avatar's facing, and keep `checkPageTrees` in step. A tree
whose `posswap` equals its `axis` is `.matched`.

**Adding a setting.**

1. Add it to `AppSettings`, with a default, a `CodingKeys` case and a `decodeIfPresent` fallback.
2. Copy it to the pipeline in `PhoneController.settingsChanged`.
3. Show it in `SettingsView`.

**Changing the rate.** Remember that the page smooths per message ([MATH.md §6](MATH.md#6-rate-and-the-pages-smoothing)).

**Moving to the Swift 6 language mode.** A full Catalyst build with `-swift-version 6` reports eleven data-race
errors. All are of one kind, and all are in `PhoneController`: a non-`Sendable` value (`self`, or the log once) is sent
into a `DispatchQueue` closure. Core, Transport and the views build clean. The refactor:

1. Make `PhoneController` and `Cues` `@MainActor`.
2. Move the frame-queue state (the pipeline, the log, the fit) into its own object that the `nonisolated`
   `ARSessionDelegate` methods use.
3. Set `swift-tools-version: 6.0`.

## 7. How the code is written

The comments follow the rest of the repository, `js/mesq_parser.js` and `Dongle_Binary.ino`:

- each file opens with a block saying **why it exists**;
- tags mark the kind of reasoning:

  | Tag | Marks |
  |---|---|
  | PROTOCOL | a wire contract |
  | MATH | a derivation |
  | RECOVERY | failure handling |
  | CONCURRENCY | who may touch what |
  | POLICY | what is sent when |
  | COMPATIBILITY | old firmware or pages |

- the external fact relied on is cited with its address: a file and line in this repository, or the Apple
  documentation page.

## 8. Learning the pieces

| To learn | Start with |
|---|---|
| Swift | *The Swift Programming Language* ([docs.swift.org/swift-book](https://docs.swift.org/swift-book/)): the Basics, Structures and Classes, Closures, Protocols, and Concurrency chapters |
| SwiftUI and iOS apps | Stanford **CS193p**, *Developing Applications for iOS* ([cs193p.sites.stanford.edu](https://cs193p.sites.stanford.edu)), free lectures; Apple's SwiftUI tutorials ([developer.apple.com/tutorials/swiftui](https://developer.apple.com/tutorials/swiftui)) |
| ARKit tracking | Apple, [Understanding world tracking](https://developer.apple.com/documentation/arkit/understanding-world-tracking); WWDC 2018 *Understanding ARKit Tracking and Detection* (session 610); WWDC 2017 *Introducing ARKit* (session 602) |
| Network.framework | WWDC 2018 *Introducing Network.framework: A modern alternative to Sockets* (session 715); Apple's `NWConnection` and `NWProtocolWebSocket` reference |
| WebSocket frames | RFC 6455 §5 (framing, masking, fragmentation); then read `tools/mock_hub.js`, which implements the server side in about 60 lines |
| The frames and quaternions | [MATH.md §9](MATH.md#9-references) |
| xtool | [xtool.sh](https://xtool.sh) and its *Build your first iOS app on Linux* tutorial |
