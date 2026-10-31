# RandGate — four-output seeded generative gate sequencer

**Hardware:** Ornament & Crime (Teensy 3.2), Phazerville PSv1.14 fork.
**Status:** first implementation; confirm T32 CI succeeds and test on hardware before performance use.

RandGate is a **full-screen app**, not a Hemisphere applet. It uses all four physical DAC outputs, so Rout and RandGate coexist in the firmware menu but cannot execute simultaneously.

## Connections

| Port | Purpose |
| --- | --- |
| TR1 | Clock (default: 1/16 notes; switchable to 1/32 notes) |
| TR2 | Reset on rising gate |
| A–D | Independent 0/5 V gate streams |
| CV1–CV4 | Unused in v1 |

No internal free-running clock is provided. An external clock is required. With 1/16 input, 1/32 events are placed at an internally calculated midpoint; a 1/32 input clock is preferable for varying tempo.

## Controls

- **Left encoder:** choose output A, B, C, D, or ALL.
- **Left button:** toggle freeze for the selected output; when ALL is selected, freeze all or unfreeze all.
- **Right encoder (browse):** choose a parameter.
- **Right button:** edit/confirm selected parameter.
- **Right encoder (edit):** change its value.
- **Right button on FREEZE/REROLL:** perform the action immediately.

Channel parameters: LENGTH (1–32 x 1/16 notes), SEED (1–65535), P 1/32, P 1/16, P 1/8, P 1/4 (0–100%), MIN % and MAX % (0–200%), FREEZE, REROLL.

Global parameters: DENSITY (0–200%), LINK, CLOCK IN (1/16 or 1/32), RETRIG, FREEZE ALL, REROLL ALL.

## Rhythmic model

There are two 1/32 slots per 1/16 step and up to 64 slots per pattern. Each slot independently tests all matching rhythmic onset grids (1/32, 1/16, 1/8, 1/4). When multiple grids succeed, the **coarsest** interval wins. Global density scales probabilities, capped at 100%.

Each generated onset receives a randomized gate length, uniformly distributed between MIN % and MAX % of its *selected rhythmic interval*. Example at 120 BPM: a 1/16 event at 80% lasts 100 ms, while a 1/4 event at 80% lasts 400 ms. Gate end time is timed in internal firmware ticks, not quantized to clock edges.

A new onset while an existing gate is high is ignored by default. With RETRIG enabled, a 1 ms gap is inserted and the later event replaces the earlier one.

## Freeze and seeds

- Deterministic generator per channel, given the same seed and generation settings.
- Unfrozen channels evolve at their own pattern boundaries, using new seeds.
- Frozen channels repeat the identical onset/length table on every loop.
- REROLL replaces the current table with a new seeded one, including while frozen.
- LINK copies probability and MIN/MAX edits to other channels; seeds and pattern lengths remain independent.
- Saved settings include active generation settings and their **density snapshot**, so frozen patterns can be reconstructed after power cycle when app data is saved from the normal O_C settings mechanism.

Editing a frozen channel's parameters does not alter its sound until reroll or unfreeze. Editing an unfrozen channel takes effect on the next cycle, or on immediate reroll.

## Build

From the software directory, run:

    pio run -e T32

Then check Teensy 3.2 flash/RAM usage. In the app menu, Rout and RandGate should both be present.

## Hardware validation checklist

1. Confirm TR1 clock and TR2 reset; verify the first clock after reset plays step 0.
2. Play 1-step and 32-step channels together. Check 1/32 subdivisions.
3. Compare multiple cycles of frozen patterns; changing probability while frozen must not change any event.
4. Verify gate duration on a scope at 60, 120, 180 BPM.
5. Test MIN=0, MAX=200, overlapping gates and retrigger behavior.
6. Save settings, restart, confirm exact frozen pattern recall.
7. Change between Rout and RandGate and confirm no stuck output gates.

## v1 limitations

- No swing, CV control or internal free-running clock.
- First clock uses a default tempo until a second clock is observed.
- Reroll during playback replaces the remaining events immediately.
- External clock tempo changes do not retroactively stretch gates already on.
