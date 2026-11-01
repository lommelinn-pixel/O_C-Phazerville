# RandGate v2 — performance controls

**Branch:** randgate-v2, Phazerville v1.14 for standard Teensy 3.2.
**Status:** custom firmware for testing. Keep the working randgate-v1 .hex available as a rollback until tested on hardware.

## Inputs and outputs

- TR1 = external clock, set to either 1/16 or 1/32 notes in RandGate's ALL settings.
- TR2 = reset (rising edge). The next clock starts step 1.
- CV1 / CV2 / CV3 / CV4 = real-time gate length modulation for OUT A / B / C / D.
- OUT A–D = four individual generated 0/5V gate sequences.

Each CV input is **bipolar**: 0 V is neutral, −5 V subtracts 100 percentage points, and +5 V adds 100 percentage points from its channel's manual GATE x setting (result clamped 0–200%). With manual 100%, the full CV range produces 0–200% scaling. An unpatched/0V input leaves the manual setting unchanged. Unipolar 0–5 V increases gate length from the manual setting; use a negative CV to shorten it.

Gate scaling acts on the duration of the currently playing gate *as well as future gates*, but cannot re-open a gate that has already ended. Randomized positions and durations do not change when GATE x is adjusted. Frozen patterns remain frozen.

## User interface

- Left encoder: select CH A, B, C, D, ALL, **PR** (presets).
- Left encoder press on a channel: toggle freeze. On ALL: freeze or unfreeze all four.
- Right encoder: browse parameters; press right encoder to edit; rotate it to change, press again to finish.
- Channel parameters include **GATE x** (0–200%, 100% neutral), next to MIN % / MAX %.
- When LINK is enabled in ALL, adjusting GATE x on one channel copies that manual value to the other three channels. CV modulation is always per-channel.

### Eight performance presets

Select **PR** with the left encoder, then:

1. Select **SLOT** with the right encoder, press to edit, rotate to choose slot 1–8, press to exit edit mode.
2. Select **SAVE**, press the right encoder to store the current audible 4-channel setup in that slot.
3. Select **RECALL AT**, choose **BAR** (default) or **NOW**.
4. Select **LOAD**, press to load that slot. Empty slots do nothing.
5. In BAR mode, loading is queued for the next 4/4 bar boundary (16 incoming 1/16 pulses, or 32 incoming 1/32 pulses, measured from TR2 reset / app start), then all four sequences restart at step 1 together. In NOW mode, the patterns reset immediately, with their first events sounding on the next incoming clock.

Each snapshot preserves the **currently audible** pattern on each channel (including exact deterministic seed, division probabilities, min/max length, and generation density); channel length; the four manual GATE x values; frozen states; global density, clock mode, linked mode, and retrigger mode. Queued but not-yet-generated parameter edits are deliberately *not* included in a preset snapshot.

**To persist saved preset slots after power-off, also save O_C application data to EEPROM: long-press the right encoder to open the main app menu, then long-press it again to save settings.** The in-app SAVE button stores a snapshot in working memory; this does not immediately write EEPROM, to avoid interrupting tight gate timing.

## Compatibility

The v2 application storage layout is different from v1. Old RandGate v1 app data is not automatically migrated. Record existing v1 settings/patterns and keep the v1 firmware .hex before flashing.

**Teensy 3.2 EEPROM trade-off:** The v2 firmware keeps full-screen **Rout** and **Hemispheres**, but excludes the standalone **Scenery** and **Piqued** applications and reduces Hemispheres' on-device preset slots from four to two. Their source remains in the repository, and the separate v1 firmware is unchanged. Existing Hemisphere/app settings may not restore across this firmware change because application data chunk sizes differ. Back up presets, calibration data and working v1 firmware before flashing. This reduction is required to fit eight persistent RandGate snapshots in the 2 KB internal EEPROM; it is not an audio/CPU limitation.

## Testing

1. With 1/16 clock, confirm A–D produce gates and TR2 resets all phases.
2. Freeze a pattern, shorten its manual GATE x to 20%, then extend to 180%; ensure onsets stay fixed.
3. Apply −5 V and +5 V to CV1; check OUT A duration shortens/lengthens without modifying OUT B–D.
4. With an oscilloscope, adjust GATE x during a HIGH gate and verify its ending moves dynamically.
5. Save a frozen four-channel sound into slot 1, alter parameters, and LOAD NOW; verify exact repeat.
6. Queue slot 2 in BAR mode and confirm it changes only on the next bar.
7. After the system-wide EEPROM save, power-cycle and verify presets and their states.
8. Confirm Rout still appears in the app selection menu and functions normally.
