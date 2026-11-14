// =========================================================================
//  SETTINGS  --  App/SettingsView.swift
//
//  Every setting is stored at once (PhoneController.settingsChanged) and
//  reaches the pipeline on the next frame. Two take effect later, and say so:
//  the camera facing (at the next Zero) and the hub address (after "Use").
// =========================================================================
#if os(iOS)
import SwiftUI

struct SettingsView: View {
    @ObservedObject var controller: PhoneController
    @State private var hubDraft = ""

    var body: some View {
        Form {
            Section {
                TextField("ws://192.168.4.1:80/ws", text: $hubDraft)
                    .keyboardType(.URL)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                Button("Use this address") { controller.settings.hubURL = hubDraft }
                    .disabled(hubDraft == controller.settings.hubURL)
                Picker("Messages per second", selection: $controller.settings.rateHz) {
                    ForEach(AppSettings.rates, id: \.self) { Text("\($0)").tag($0) }
                }
            } header: {
                Text("Hub")
            } footer: {
                Text("""
                Join the hub's Wi-Fi (MM-…) in iOS Settings first. To test without a hub, run \
                tools/mock_hub.js on a computer and enter ws://<its address>:8765/ws.
                """)
            }

            Section {
                Picker("Suit tree", selection: $controller.settings.tree) {
                    ForEach(PageTree.allCases, id: \.self) { Text($0.title).tag($0) }
                }
            } header: {
                Text("Page")
            } footer: {
                Text("""
                Pick the tree chosen in the page's suit menu. The trees label the phone's axes \
                differently, and the app turns its data so walking and leaning both come out \
                the right way for that tree.
                """)
            }

            Section {
                Picker("Rear camera looks", selection: $controller.settings.facing) {
                    Text("Forward (phone on the front)").tag(CameraFacing.forward)
                    Text("Backward (phone on the back)").tag(CameraFacing.backward)
                    Text("To my left").tag(CameraFacing.left)
                    Text("To my right").tag(CameraFacing.right)
                }
                Stepper(value: $controller.settings.hipHeightCm, in: 50...130, step: 1) {
                    LabeledContent("Hip height", value: String(format: "%.0f cm", controller.settings.hipHeightCm))
                }
                Stepper(value: $controller.settings.leverDepthCm, in: 0...30, step: 1) {
                    LabeledContent("Hip centre behind the screen",
                                   value: String(format: "%.0f cm", controller.settings.leverDepthCm))
                }
                if let l = controller.settings.measuredLever {
                    LabeledContent("Measured lever arm", value: String(format: "%.1f cm", l.length * 100))
                    Button("Forget the measured lever arm", role: .destructive) {
                        controller.forgetMeasuredLever()
                    }
                }
            } header: {
                Text("How the phone is worn")
            } footer: {
                Text("""
                The camera direction is used at the next Zero. 100 cm hip height matches the \
                avatar. A measured lever arm replaces the manual depth until you forget it.
                """)
            }

            Section {
                Toggle("Record a session log", isOn: $controller.settings.logging)
                Toggle("Spoken cues", isOn: $controller.settings.spokenCues)
                Stepper(value: $controller.settings.countdownSeconds, in: 3...20) {
                    LabeledContent("Countdown", value: "\(controller.settings.countdownSeconds) s")
                }
            } header: {
                Text("Session")
            } footer: {
                Text("The log setting applies from the next Start.")
            }
        }
        .navigationTitle("Settings")
        .onAppear { hubDraft = controller.settings.hubURL }
    }
}
#endif
