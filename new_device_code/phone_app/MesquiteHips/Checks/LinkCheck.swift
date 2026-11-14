// =========================================================================
//  LINK CHECK  --  Checks/LinkCheck.swift
//
//  The real Transport/HubLink.swift (Network.framework, as on the phone)
//  against a hub stand-in: ../tools/mock_hub.js, which applies the hub's
//  acceptance rule and feeds what it accepts to the page's own parser,
//  js/mesq_parser.js. Runs only where Network.framework exists (macOS), and
//  only when MESQ_HUB_URL is set; ../tools/run_checks.sh --e2e does both.
// =========================================================================
import Foundation

func checkLink(url: URL, sentOut: URL?) {
    #if canImport(Network)
    report.section("link: Network.framework WebSocket -> hub mock -> js/mesq_parser.js")
    let link = HubLink()
    link.start(url: url)
    let deadline = Date().addingTimeInterval(5)
    while link.snapshot.state != .ready && Date() < deadline { Thread.sleep(forTimeInterval: 0.02) }
    report.check(link.snapshot.state == .ready, "connects (WebSocket handshake accepted)", "\(link.snapshot.state)")
    guard link.snapshot.state == .ready else { return }

    // The velvet take at its real 30 Hz pace, compressed 2x.
    let take = runScenario(tree: .velvetFamily, lever: nil, mount: .frontPortrait(), seconds: 12, motion: walkLeanTurn)
    var accepted: [String] = []
    var refused = 0
    for line in take {
        guard let text = line.wire else { continue }
        // Retry a busy link for up to 20 ms, as the next AR frame would.
        var ok = link.send(text)
        var tries = 0
        while !ok && tries < 20 { Thread.sleep(forTimeInterval: 0.001); ok = link.send(text); tries += 1 }
        if ok { accepted.append(text) } else { refused += 1 }
        Thread.sleep(forTimeInterval: 1.0 / 60)
    }
    Thread.sleep(forTimeInterval: 0.5)
    let stats = link.snapshot.stats
    link.stop()
    Thread.sleep(forTimeInterval: 0.2)
    report.check(refused == 0 && stats.sendErrors == 0, "every message was taken and completed",
                 "sent \(stats.sent), completed \(stats.completed), busy \(stats.skippedBusy), errors \(stats.sendErrors)")
    report.check(stats.completed == accepted.count, "completions match sends")
    if let out = sentOut {
        try? Data((accepted.joined(separator: "\n") + "\n").utf8).write(to: out)
        report.note("wrote \(accepted.count) sent lines to \(out.path) for the mock to compare")
    }
    #else
    report.section("link")
    report.note("Network.framework is not available here (Linux): the link check runs on macOS.")
    #endif
}
