# 01 — Node Firmware

## Phase 1 (diagnosis)
[`REPORT.md`](REPORT.md) — the original A1 assessment. Unchanged.

## Phase 3 (implementation)
Start with **[`SOLVED.md`](SOLVED.md)** for the outcome at a glance, then:

| File | What it is |
|---|---|
| [`A1_00_IMPLEMENTATION_PLAN.md`](A1_00_IMPLEMENTATION_PLAN.md) | Execution mode, requirements, **and the explicit Phase 2 delta — what was not redone** |
| [`A1_01_SOLUTION_DECISIONS.md`](A1_01_SOLUTION_DECISIONS.md) | Why each change is the change it is, and what would justify revisiting it |
| [`A1_02_CHANGE_REPORT.md`](A1_02_CHANGE_REPORT.md) | What actually changed, with anchors and before/after |
| [`A1_03_VERIFICATION_REPORT.md`](A1_03_VERIFICATION_REPORT.md) | Commands, results, residual risks, and what may **not** be claimed |
| [`A1_04_RADIO_CHANNEL_STUDY.md`](A1_04_RADIO_CHANNEL_STUDY.md) | One channel or several — constraints, options, comparison protocol |
| [`A1_05_LEARNING_GUIDE.md`](A1_05_LEARNING_GUIDE.md) | The system explained: quaternions, time, two cores, the wire, parsers |
| **[`A1_05_RETYPE_THESE.md`](A1_05_RETYPE_THESE.md)** | **The five code sections to type out yourself**, with exercises and an answer key |
| [`A1_06_TRACEABILITY_MATRIX.md`](A1_06_TRACEABILITY_MATRIX.md) | Every finding → anchor → evidence → diagnosis → resolution |
| [`A1_07_OPEN_ITEMS_AND_HARDWARE_RUNBOOK.md`](A1_07_OPEN_ITEMS_AND_HARDWARE_RUNBOOK.md) | What is still open, with exact commands and pass/fail rules |
| [`A1_08_REPRODUCTION_COMMANDS.md`](A1_08_REPRODUCTION_COMMANDS.md) | How to rerun everything |
| [`A1_09_WALL_CHECKLIST.md`](A1_09_WALL_CHECKLIST.md) · [`.csv`](A1_09_WALL_CHECKLIST.csv) | The printable checklist, in dependency order |
| [`A1_10_REFERENCED_BACKLOG.md`](A1_10_REFERENCED_BACKLOG.md) | Real findings deliberately left out of A1 scope |

Hub findings: [`../02_hub/HUB_SOLVED.md`](../02_hub/HUB_SOLVED.md).

## The one thing to keep in mind

**No firmware was built for the target and nothing ran on hardware.** 189 host
assertions pass across five suites, and two of those suites found real defects —
but `CODE_FIXED` is not `VERIFIED`. The first open item is an `arduino-cli` build.
