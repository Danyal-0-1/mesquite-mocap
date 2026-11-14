#!/usr/bin/env bash
# =========================================================================
#  run_checks.sh -- every check that runs without an iPhone.
#
#    1. the app's Core checks (math, wire format, lever arm, pipeline),
#       built with swiftc from the app's own sources;
#    2. the hub mock's self-test;
#    3. the scenario takes replayed through the hub rule and the page's
#       parser (js/mesq_parser.js);
#    4. with --e2e, on macOS only: the app's real Network.framework link
#       streaming to the mock hub over a socket;
#    5. with --catalyst, on macOS only: the whole app, ARKit and SwiftUI
#       screens included, compiled and linked for Mac Catalyst. That is the
#       iOS API surface, so it catches what the desktop checks cannot.
#
#  Needs: swiftc (Swift 6 toolchain) and node (18 or later).
#  On a Mac with only the Command Line Tools, run it as
#      DEVELOPER_DIR=/Library/Developer/CommandLineTools tools/run_checks.sh --e2e --catalyst
# =========================================================================
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
PKG="$HERE/../MesquiteHips"
WORK="${MESQ_WORK:-${TMPDIR:-/tmp}/mesquite_hips_checks}"
E2E=0
CATALYST=0
for a in "$@"; do
  case "$a" in
    --e2e) E2E=1 ;;
    --catalyst) CATALYST=1 ;;
    *) echo "usage: $0 [--e2e] [--catalyst]" >&2; exit 2 ;;
  esac
done
mkdir -p "$WORK/scenarios"

echo "== building the checks (swiftc, $(swiftc --version 2>&1 | head -1))"
swiftc -O -swift-version 5 \
  "$PKG"/Sources/MesquiteHips/Core/*.swift \
  "$PKG"/Sources/MesquiteHips/Transport/*.swift \
  "$PKG"/Checks/*.swift \
  -o "$WORK/checks"

echo "== running the checks"
MESQ_SCENARIO_DIR="$WORK/scenarios" "$WORK/checks"

echo
echo "== hub mock self-test"
node "$HERE/mock_hub.js" --self-test

echo
echo "== scenario takes through the hub rule and js/mesq_parser.js"
for f in "$WORK"/scenarios/*.jsonl; do
  printf '  %-40s ' "$(basename "$f")"
  if node "$HERE/mock_hub.js" --replay "$f" > "$f.replay.json"; then
    node -e '
      const r = require(process.argv[1]);
      console.log(`ok   ${r.accepted} accepted, ${r.parser.json_lines} parsed, ` +
                  `max ${r.bytes_max} B, page drops ${r.page.position_guard_drops} ` +
                  `(naive sender: ${r.naive_without_zero_nudge.position_guard_drops})`);' "$f.replay.json"
  else
    echo "FAIL (see $f.replay.json)"; exit 1
  fi
done

if [ "$E2E" = 1 ]; then
  if [ "$(uname)" != Darwin ]; then
    echo "== --e2e needs macOS (Network.framework); skipped"
  else
    echo
    echo "== end to end: HubLink (Network.framework) -> mock hub -> js/mesq_parser.js"
    PORT=$(node -e 'const s=require("net").createServer().listen(0,()=>{console.log(s.address().port);s.close()})')
    rm -f "$WORK/sent.txt" "$WORK/e2e_report.json"
    node "$HERE/mock_hub.js" --port "$PORT" --host 127.0.0.1 --quiet --idle-exit 1500 \
         --expect "$WORK/sent.txt" --report "$WORK/e2e_report.json" > "$WORK/e2e_hub.log" 2>&1 &
    HUB=$!
    sleep 0.5
    MESQ_HUB_URL="ws://127.0.0.1:$PORT/ws" MESQ_SENT_OUT="$WORK/sent.txt" "$WORK/checks" | sed -n '/== link/,$p'
    if wait "$HUB"; then
      node -e '
        const r = require(process.argv[1]);
        console.log(`  ok    hub accepted ${r.accepted}/${r.expect.sent}, identical to what was sent: ` +
                    `${r.expect.identical}; parsed ${r.parser.json_lines}; refused ${JSON.stringify(r.refused)}; ` +
                    `page guard drops ${r.page.position_guard_drops}`);' "$WORK/e2e_report.json"
    else
      echo "  FAIL  the hub mock disagreed with what was sent:"; cat "$WORK/e2e_hub.log"; exit 1
    fi
  fi
fi

if [ "$CATALYST" = 1 ]; then
  if [ "$(uname)" != Darwin ]; then
    echo "== --catalyst needs macOS; skipped"
  else
    echo
    echo "== the whole app for Mac Catalyst (ARKit, SwiftUI, UIKit, AVFoundation, Network)"
    SDK="$(xcrun --sdk macosx --show-sdk-path)"
    swiftc -parse-as-library -module-name MesquiteHips -swift-version 5 \
      -target "$(uname -m)-apple-ios16.0-macabi" -sdk "$SDK" \
      -Fsystem "$SDK/System/iOSSupport/System/Library/Frameworks" \
      -I "$SDK/System/iOSSupport/usr/lib/swift" -L "$SDK/System/iOSSupport/usr/lib/swift" \
      "$PKG"/Sources/MesquiteHips/Core/*.swift "$PKG"/Sources/MesquiteHips/Transport/*.swift \
      "$PKG"/Sources/MesquiteHips/App/*.swift -o "$WORK/MesquiteHips-catalyst"
    echo "  ok    compiled and linked with no errors"
  fi
fi

echo
echo "all checks passed"
