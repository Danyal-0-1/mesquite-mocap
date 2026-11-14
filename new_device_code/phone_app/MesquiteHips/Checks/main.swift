// =========================================================================
//  Mesquite Hips checks. Built and run by ../tools/run_checks.sh.
//
//  Environment:
//    MESQ_SCENARIO_DIR  write the scenario takes there (for mock_hub --replay)
//    MESQ_HUB_URL       also run the link check against a hub (mock or real)
//    MESQ_SENT_OUT      where the link check writes the lines it sent
// =========================================================================
import Foundation

let env = ProcessInfo.processInfo.environment

checkGeometry()
checkHubMessage()
checkHipsFrame()
checkPageTrees()
checkLeverArm()
checkPipeline()

if let dir = env["MESQ_SCENARIO_DIR"] {
    report.section("scenarios")
    do { try writeScenarios(to: URL(fileURLWithPath: dir)) } catch {
        report.check(false, "write scenarios", "\(error)")
    }
}
if let hub = env["MESQ_HUB_URL"], let url = URL(string: hub) {
    checkLink(url: url, sentOut: env["MESQ_SENT_OUT"].map { URL(fileURLWithPath: $0) })
}

exit(report.finish())
