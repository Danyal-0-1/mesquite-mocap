#!/usr/bin/env bash
# Rebuild the proposal slide deck and speaker notes as PDFs.
#
# Renders with headless Chrome, which handles the CSS paged-media rules and the
# inline SVG figures natively. Edit slides.html / speaker_notes.html and rerun.
set -euo pipefail
cd "$(dirname "$0")"

CHROME="$(command -v google-chrome || command -v chromium || command -v chromium-browser)"
[ -n "$CHROME" ] || { echo "No Chrome/Chromium found."; exit 1; }

render() {
  echo "  $2"
  "$CHROME" --headless=new --disable-gpu --no-sandbox --no-pdf-header-footer \
            --print-to-pdf="$PWD/$2" "file://$PWD/$1" 2>/dev/null
}

echo "Rendering:"
render slides.html        Mesquite_ML_Proposal_Slides.pdf
render speaker_notes.html Mesquite_ML_Proposal_SpeakerNotes.pdf

echo
for f in Mesquite_ML_Proposal_Slides.pdf Mesquite_ML_Proposal_SpeakerNotes.pdf; do
  printf '%-42s %s\n' "$f" "$(command -v pdfinfo >/dev/null && pdfinfo "$f" | awk '/^Pages/{print $2" pages"}' || echo built)"
done
