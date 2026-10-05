# Documentation index

Reconciled on 2026-10-05 against source repair `443c769` and its verified
installation. Later documentation commits do not change the installed binaries.
The development Mac uses protocol 4; the mini and existing DMG remain on
`c563b00` / protocol 3. Refer to the owning documents below instead of copying
package hashes, test counts or acceptance claims into new guides.

## Current guides and ownership

| Document | Owns |
| --- | --- |
| [README](../README.md) | Product overview, current status, quick build and entry points |
| [Installation](../INSTALL.md) | Per-Mac/package identities, hashes, startup evidence and retained rollback paths |
| [Testing](../testing.md) | Automated coverage, commands, test isolation and validation limits |
| [Show readiness](../SHOW-READINESS.md) | Planned SD5/56-input rig and unperformed acceptance gates |
| [Field checklist](../FIELD-TEST.md) | Repeatable interaction/capture checks; unchecked means unperformed |
| [Hardware audit checklist](../FIELD-TEST-AUDIT.md) | Additional device, storage, console and recovery cases |
| [Tasks](../tasks.md) | Current work, completed steps and carried-forward open checks |
| [Changelog](../CHANGELOG.md) | User-visible changes and dated delivery history |

## Engineering references

| Document | Owns |
| --- | --- |
| [Architecture](../architecture.md) | Modules, processes, threads, data flow and persistence/capture contracts |
| [Decisions](../decisions.md) | Dated rationale and explicit amendments that supersede older choices |
| [Coding standards](../coding-standards.md) | Contributor conventions, safety invariants and verification expectations |
| [Operating notes](../CLAUDE.md) | Repository workflow and implementation constraints |
| [Design](../design.md) | Product interaction and visual-token rules; not field/accessibility acceptance |
| [Component index](components/README.md) | Scope and maintenance rules for component contracts |

## Component contracts

| Component | Reference |
| --- | --- |
| Automation controls | [AutomationToolbar](components/AutomationToolbar.md) |
| Clock and capture status | [BigClockPanel](components/BigClockPanel.md) |
| Mixer channel | [ChannelStrip](components/ChannelStrip.md) |
| Dialog helpers | [DialogChrome](components/DialogChrome.md) |
| EDIT row | [EditPageTrackRow](components/EditPageTrackRow.md) |
| Time and marker ruler | [EditTimeRuler](components/EditTimeRuler.md) |
| EDIT tool palette | [EditToolsBar](components/EditToolsBar.md) |
| Level fader | [FineFader](components/FineFader.md) |
| Meter | [LedMeter](components/LedMeter.md) |
| Master channel | [MasterStrip](components/MasterStrip.md) |
| Removed global clip bar | [PeakTally — historical only](components/PeakTally.md) |
| Orphan-session recovery | [SessionRecoveryDialog](components/SessionRecoveryDialog.md) |
| Non-modal feedback | [Toast](components/Toast.md) |
| Transport controls | [TransportBar](components/TransportBar.md) |
| New/open workflow | [WelcomeDialog](components/WelcomeDialog.md) |

## Audit and field evidence

These files retain the measurements, counts and outcomes from their named
checkpoints. A later installation does not turn an older test into evidence for
new code. Removed installer paths and old rollback names are historical; use
[INSTALL.md](../INSTALL.md) for current artifacts.

| Record | Scope |
| --- | --- |
| [October 5 follow-up](REVIEW-REPAIRS-2026-10-05.md) | All 21 findings, failing/passing regressions, 563-group suites and remaining limits |
| [October 5 S1–S6 audit](AUDIT-2026-10-05.md) | Earlier deep STOP/concurrency repair ledger and its 519-group checkpoint |
| [October 3 audit](../AUDIT_FIXES_2026-10-03.md) | Initial 28 and additional 14 findings |
| [October 1–2 long takes](../FIELD-TEST-2026-10-01.md) | Named Mac mini builds, primary-file checks and UI/host limits |
| [September 30 audit](../AUDIT_FIXES_2026-09-30.md) | Continuation, report and unattended STOP integrity |
| [September 20 audit](../AUDIT_FIXES_2026-09-20.md) | Recording reliability changes |
| [September 15 audit](../AUDIT_REPORT_2026-09-15.md) | Whole-project findings and evidence |
| [September 12 audit](../AUDIT_FIXES_2026-09-12.md) | Recording/session-integrity fixes |

## Theme sources and historical handoffs

| Document | Status |
| --- | --- |
| [App icon](../Source/Theme/README-icon.md) | Current source/build workflow |
| [Token promotion](../Source/Theme/TOKENS-heated-steel.md) | Still a proposed upstream brand-repository change |
| [Contrast audit](../Source/Theme/CONTRAST-AUDIT.md) | Historical colour-pair snapshot, not current WCAG or stage-lighting certification |
| [Pixel-parity specification](../Source/Theme/IMPLEMENTATION_SPEC.md) | Historical June handoff; use current layout source before applying it |

## Updating this set

Update the owner first, then reconcile callers and links. Record the exact source
build, machine, scope and result for new evidence. Preserve historical counts;
mark superseded operational instructions clearly. Do not check off physical
recording, normal quit, UI latency or accessibility based solely on unit tests or
startup rendering. Documentation-only edits validate links, source claims and
examples without relaunching or altering the installed app.
