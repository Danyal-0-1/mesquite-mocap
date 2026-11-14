// =========================================================================
//  SPOKEN CUES  --  App/Cues.swift
//
//  WHY: the phone is worn with its screen against the body and its camera
//  looking out, so nothing on the screen can be read during a Zero or a
//  lever-arm turn. Every timed step is therefore announced by voice and by
//  a vibration. The audio session uses .playback, so the cues still play
//  with the ring/silent switch on silent, and mixes with other audio rather
//  than ducking it for the whole session.
// =========================================================================
#if os(iOS)
import AVFoundation
import UIKit

final class Cues {
    private let synth = AVSpeechSynthesizer()
    private var enabled = true
    private var prepared = false

    func prepare(spoken: Bool) {
        enabled = spoken
        guard !prepared else { return }
        prepared = true
        let audio = AVAudioSession.sharedInstance()
        try? audio.setCategory(.playback, mode: .voicePrompt, options: [.mixWithOthers])
        try? audio.setActive(true)
    }

    func setSpoken(_ on: Bool) { enabled = on }

    /// Say something now, cutting off anything still being said.
    func say(_ text: String) {
        UIImpactFeedbackGenerator(style: .medium).impactOccurred()
        guard enabled else { return }
        if synth.isSpeaking { synth.stopSpeaking(at: .immediate) }
        let u = AVSpeechUtterance(string: text)
        u.rate = AVSpeechUtteranceDefaultSpeechRate
        synth.speak(u)
    }

    func success(_ text: String) {
        UINotificationFeedbackGenerator().notificationOccurred(.success)
        say(text)
    }

    func failure(_ text: String) {
        UINotificationFeedbackGenerator().notificationOccurred(.error)
        say(text)
    }
}
#endif
