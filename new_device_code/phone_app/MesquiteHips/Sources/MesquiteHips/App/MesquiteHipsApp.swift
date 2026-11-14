// =========================================================================
//  APP ENTRY  --  App/MesquiteHipsApp.swift
//
//  xtool links the library into an executable and calls the @main type, so
//  the entry point lives in the library itself (as in xtool's template).
//  iOS only: on a desktop `swift build` and `swift test` compile the Core
//  and Transport folders and skip this one.
// =========================================================================
#if os(iOS)
import SwiftUI

@main
struct MesquiteHipsApp: App {
    @StateObject private var controller = PhoneController()
    @Environment(\.scenePhase) private var scenePhase

    var body: some Scene {
        WindowGroup {
            ContentView(controller: controller)
        }
        .onChange(of: scenePhase) { phase in
            controller.scenePhaseChanged(phase)
        }
    }
}
#endif
