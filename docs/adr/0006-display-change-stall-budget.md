# ADR 0006: Stall target split into steady state and display changes

- **Status:** Accepted (2026-10-30, chosen by the project owner after the third acceptance run)

## Context

The brief (§4.5) asks that adding or removing a virtual display completes in under 3 s, "with no system-wide stall
> 250 ms and no hang". Three acceptance runs on the dev PC (`docs/perf.md`) and the reproduction with SudoVDA,
parsec-vdd and MTT (`docs/vdd-freeze-analysis.md`) show:

- Tessera's driver does its part quickly: a monitor is visible in the topology in about 0.1 s, a batch of 8 in about
  0.5 s, and its host process stays near 0 % CPU.
- **Windows freezes composition and Explorer while it reconfigures displays**, whatever the driver: about 0.2 s for
  one display (80–85 % of single changes under 250 ms; worst 0.34 s compose, 0.48 s Explorer) and about 1 s for 8
  displays in one request. The third-party drivers stall the same or worse.
- **Changes made back to back queue up**: Windows is still reconfiguring when the next change arrives, and the desktop
  freezes for seconds.
- **Once the displays are steady, nothing stalls over 250 ms** (from 1 s after the last change, 120 s with 4 virtual
  displays).

So 250 ms holds in steady state but cannot be met during a change of several displays by any driver.

## Decision

The stall target is split in two:

1. **Steady state: no system-wide stall over 250 ms** while the set of displays does not change, from 1 s after a
   change is complete. This is a hard target, as in the brief. Met (run 3, `hold-4`).
2. **During a display change: one freeze per change, no worse than the run-3 baseline.** The freeze is Windows' cost
   and is accepted. The baseline is the worst stall per burst in run 3 (`tools/vdd-stress/per_burst.py`; for 8
   displays, the worse of add and remove):

   | Change | Compose, median / p90 / max | Explorer, median / p90 / max |
   |---|---|---|
   | 1 display, add | 0.23 / 0.27 / 0.34 s | 0.16 / 0.23 / 0.48 s |
   | 1 display, remove | 0.20 / 0.32 / 0.34 s | 0.13 / 0.24 / 0.28 s |
   | 8 displays in one request, physical monitors on | 0.93 / 1.24 / 1.55 s | 0.56 / 0.77 / 1.76 s |
   | 8 displays in one request, headless | 1.05 / 1.42 / 1.63 s | 0.74 / 2.04 / 3.24 s |

   A later acceptance run on the same PC whose medians or p90s are more than 25 % above these is a regression to
   investigate.

The other §4.5 targets stay as they are: every add or remove under 3 s, and no hang.

## Consequences

Rules for the host (Phase 2 on), because they are what keeps changes inside the baseline:

- **Batch.** Apply all of a client's displays with one `ADD_MONITORS` (or `REMOVE_MONITORS`) request and one topology
  change, never one display at a time.
- **Never chain changes.** Start a display change only after the previous one is visible and about 1 s has passed;
  coalesce requests that arrive meanwhile into the next change.
- **Stable monitor IDs.** Give each client monitor the same 128-bit ID every time. A new ID makes Windows create a new
  monitor device and new saved settings (about 80 ms more per add, 1.4 s more per batch of 8), and leftovers pile up.
- **Headless:** after a change, re-apply the virtual-only layout once if Windows switched the physical displays back
  on, saving it to Windows' database so that the same monitor set gets it next time. Batched changes did not trigger
  this in run 3; one-at-a-time adds did, half the time.
- **Uninstall** deletes leftover monitor devices and settings first (`vdd-install forget-monitors`), as `remove`
  already does.
- The client should expect every stream to pause for up to about 1 s when the display set changes, and present that
  as "reconfiguring displays" rather than as a network problem (Phase 2/3 design).
- `docs/perf.md` reports both parts of the target, and acceptance runs compare against the baseline above.
