// =========================================================================
//  THE MAIN SCREEN  --  App/ContentView.swift
//
//  One screen, read top to bottom in the order of a session: is everything
//  green, what to press next, what is going out. No camera preview: the
//  app renders nothing it does not need, which keeps the phone cool on a
//  long take. The status rows say whether the camera can track.
// =========================================================================
#if os(iOS)
import SwiftUI

struct ContentView: View {
    @ObservedObject var controller: PhoneController

    var body: some View {
        NavigationStack {
            List {
                statusSection
                if let n = controller.countdown {
                    Section {
                        Text("\(controller.activity ?? "Start") in \(n)")
                            .font(.system(size: 44, weight: .bold, design: .rounded))
                            .frame(maxWidth: .infinity)
                        Button("Cancel", role: .cancel) { controller.cancelCountdown() }
                    }
                }
                if let notice = controller.notice {
                    Section { Text(notice).font(.callout) }
                }
                actionSection
                liveSection
                Section {
                    NavigationLink("Settings") { SettingsView(controller: controller) }
                    if let url = controller.logURL {
                        ShareLink(item: url) {
                            Label("Share this session's log", systemImage: "square.and.arrow.up")
                        }
                    }
                } footer: {
                    Text("Logs are also in the Files app: On My iPhone, Mesquite Hips.")
                }
            }
            .navigationTitle("Mesquite Hips")
        }
        .overlay {
            if controller.pocketMode { PocketView(controller: controller) }
        }
    }

    private var live: LiveStatus { controller.live }

    private var statusSection: some View {
        Section {
            StatusRow(title: "Tracking", value: controller.running ? live.tracking : "not started",
                      ok: live.trackingOK)
            StatusRow(title: "Hub", value: controller.running ? live.link : "stopped", ok: live.linkOK)
            StatusRow(title: "Sending", value: live.linkOK ? "\(live.sendHz) per second" : "nothing",
                      ok: live.linkOK && live.sendHz > 0)
            StatusRow(title: "Zero", value: zeroText, ok: live.epoch > 1)
            StatusRow(title: "Lever arm", value: leverText, ok: controller.settings.measuredLever != nil)
        }
    }

    private var actionSection: some View {
        Section {
            if controller.running {
                Button("Stop", role: .destructive) { controller.stop() }
            } else {
                Button("Start streaming") { controller.start() }
                    .font(.headline)
            }
            Button("Zero in T-pose") { controller.zeroWithCountdown() }
                .disabled(!controller.running || controller.countdown != nil)
            Button("Measure lever arm (turn on the spot)") { controller.measureLever() }
                .disabled(!controller.running || controller.countdown != nil)
            Button("Pocket screen (triple-tap to wake)") { controller.setPocket(true) }
                .disabled(!controller.running)
        } footer: {
            Text("""
            Order: Start. Strap the phone on, screen to your body. Measure the lever arm once \
            per strap-on. Zero in T-pose, then press the page's Set T-pose while still in it.
            """)
        }
    }

    private var liveSection: some View {
        Section("Live") {
            LabeledContent("Hips (cm)", value: hipsText)
            LabeledContent("Heading", value: live.headingDeg.map { String(format: "%.1f°", $0) } ?? "—")
            LabeledContent("Last message", value: live.lastLevel >= 0
                ? "\(live.lastBytes) bytes, level \(live.lastLevel)" : "—")
            LabeledContent("Sent / skipped busy", value: "\(live.sent) / \(live.skippedBusy)")
            LabeledContent("Log", value: controller.logURL == nil ? "off"
                : ByteCountFormatter.string(fromByteCount: Int64(live.logBytes), countStyle: .file))
        }
    }

    private var zeroText: String {
        switch live.epoch {
        case 0: return "waiting for tracking"
        case 1: return "automatic (not yet in T-pose)"
        default: return "done (\(live.epoch - 1)x in T-pose)"
        }
    }

    private var leverText: String {
        if let l = controller.settings.measuredLever {
            return String(format: "measured %.1f cm", l.length * 100)
        }
        return String(format: "manual %.0f cm behind the screen", controller.settings.leverDepthCm)
    }

    private var hipsText: String {
        guard let p = live.hipsCm else { return "—" }
        return String(format: "x %.1f  y %.1f  z %.1f", p.x, p.y, p.z)
    }
}

struct StatusRow: View {
    let title: String
    let value: String
    let ok: Bool

    var body: some View {
        HStack {
            Image(systemName: ok ? "checkmark.circle.fill" : "exclamationmark.circle")
                .foregroundStyle(ok ? Color.green : Color.orange)
            Text(title)
            Spacer()
            Text(value)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.trailing)
        }
    }
}

/// A black screen for wearing: an OLED pixel that is black draws no power.
struct PocketView: View {
    @ObservedObject var controller: PhoneController

    var body: some View {
        ZStack {
            Color.black.ignoresSafeArea()
            VStack(spacing: 8) {
                Text(controller.live.linkOK ? "Sending \(controller.live.sendHz)/s" : controller.live.link)
                Text(controller.live.tracking)
                Text("Triple-tap to wake").font(.footnote)
            }
            .foregroundStyle(Color(white: 0.35))
        }
        .contentShape(Rectangle())
        .onTapGesture(count: 3) { controller.setPocket(false) }
        .statusBarHidden(true)
    }
}
#endif
