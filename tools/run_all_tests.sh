#!/usr/bin/env bash
# =========================================================================
#  tools/run_all_tests.sh -- every check that runs without hardware.
#
#  This is the "minimum code-validation gate" from the master prompt's §11.
#  It does NOT include the single-node or full-fleet hardware gates; those
#  need a pod on a bench and are specified in
#  system_assessment/01_node_firmware/A1_07_OPEN_ITEMS_AND_HARDWARE_RUNBOOK.md
# =========================================================================
set -uo pipefail
cd "$(dirname "$0")/.."
rc=0
hr() { printf '\n%s\n' "======================================================================"; }

hr; echo "1/5  Sketch type-check (g++ + stubs; NOT an arduino-cli target build)"
./tools/check_sketches.sh || rc=1

hr; echo "2/5  Firmware core tests (host build, real threads)"
g++ -std=c++17 -O2 -pthread -DMESQ_HOST_TEST \
    -I"Device code/Pod_Watch_Binary" -I"Device code/Dongle_Binary" \
    tools/firmware_tests.cpp -o /tmp/mesq_fw_tests || rc=1
/tmp/mesq_fw_tests || rc=1

hr; echo "3/5  Parser acceptance suite (js/mesq_parser.js, the shipped parser)"
node tools/parser_tests.js || rc=1

hr; echo "4/5  BVH export tests"
node tools/export_tests.js || rc=1

hr; echo "5/5  Phase 2 baseline harness (reproduces the defects + Phase 3 comparison)"
node tools/replay_harness.js || rc=1

hr
if [ $rc -eq 0 ]; then echo "ALL HOST SUITES PASSED"; else echo "FAILURES ABOVE"; fi
echo "NOT covered here: any hardware. No pod, hub or radio was exercised."
exit $rc
