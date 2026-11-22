# Clock auto-reclaim on reconnect

Date: 2026-11-22
Status: proposed

## Intent

When the clock device disappears, the engine now promotes another output or
falls back to the free-running internal clock. Once the original clock device
is plugged back in, the clock role should return to it automatically, without
the user reloading the strip by hand.

The user chose automatic reclaim over a manual "Use as clock" action.

## Goals

- Give the clock role back to the original clock output (the first output,
  the one the app treats as the clock) when its saved endpoint reappears.
- Reclaim within a few seconds of the device becoming available, using the
  existing 4 Hz UI health tick and the 5 s device-enumeration cache.
- Never change the session sample rate to make the reclaim work. Reclaim only
  when the device opens at the engine's current rate; otherwise keep the
  fallback playing and show the real open error on the strip.
- Demote the stand-in cleanly: stop its render hook, restore FIFO headroom,
  and leave it playing as an ordinary output.
- Keep audio running while the reclaim happens. A short handover gap is
  acceptable; a stuck double-render is not.
- Clear the "No such device" / "Clock device stopped" error when the reclaim
  succeeds, and refresh the strips and clock line.

## Non-goals

- No sample-rate conversion or mid-session rate change.
- No reclaim attempts when the clock is already on the original output.
- No infinite reopen loop: a present-but-refusing device gets a bounded number
  of attempts per plug-in episode.
- No change to input strips or the duplex-input rules beyond what the existing
  clock open already does.
- No new user-facing setting; reclaim is automatic.

## Approaches considered

1. **Automatic reclaim (chosen).** Watch for the saved clock endpoint while
   the engine is on a fallback. Reopen it as an output at the current rate,
   then hand it the clock render role. Unattended and matches the user's
   "plug it back in and carry on" expectation. Cost: a brief audio handover.
2. **Manual switch-back.** Show a "Use as clock" action when the device
   returns. No surprise handover, but the user must notice and click, and the
   app still needs all the same open/reassign code. Rejected because the user
   chose automatic.
3. **Full session restart on reconnect.** Reopen every device and re-prime as
   at startup. Simpler state, but a much longer glitch and it throws away
   levels/streams that are still healthy. Rejected.

## Design

- **Preferred clock output.** The first output (`outputs.front()`, lowest id)
  remains the designated clock. A fallback is active whenever
  `clockOutputId` is not that output (including `0` for the internal clock).
- **Reconnect watch.** On the 4 Hz tick, when not opening devices and the
  preferred output is not the clock, check the cached output enumeration for
  the preferred output's saved device name. Ignore an empty/silent name.
  - For a named endpoint: attempt a reopen when it is present.
  - For `System Default`: skip automatic reclaim; the OS default device is a
    moving target and the default stream is handled by the existing open
    paths.
- **Reopen path.** Reuse `reloadDevicesAsync({preferred output})`, which opens
  the strip asynchronously as an ordinary output at the current rate. If the
  engine is on the internal clock (`clockOutputId == 0`), the existing open
  logic already makes that output the clock, so no second step is needed.
- **Handover for a promoted clock.** After the reopen completes and its first
  callback has arrived, the next tick sees the preferred output live. If its
  rate equals the current engine rate, stop the internal clock (if running),
  then do one locked hand-off: set the preferred FIFO target to zero, activate
  its already-installed render hook, set `clockOutputId`, deactivate the old
  clock and restore its non-clock FIFO headroom. A larger block is prepared
  before the hand-off; a smaller block after it, so the old driver's blocks
  never exceed the render ceiling. If the rate differs, leave the fallback in
  place and show the rate error.
- **Retry policy.** At most one reopen attempt every 5 s, at most 3 attempts
  per plug-in episode. Reset the counter when the endpoint goes absent again
  or when the reclaim succeeds. A failed attempt stores the driver error on
  the strip but is quiet (no modal dialog) and does not clear the fallback.
- **State ownership.** All watch/attempt state is touched only on the message
  thread (the existing tick) or inside the device-open job while
  `openingDevices_` excludes the tick, matching the existing
  `clockFailoverActive_` discipline.
- **UI.** A successful reclaim clears the preferred strip's error and returns
  `true` so the tick refreshes strips; the toolbar clock line already follows
  `getClockDescription()` each tick. Log `Clock reclaimed by <device>`.

## Build notes

- New engine API `bool maybeReclaimClock();` next to `checkClockHealth()`.
- Add a small liveness/rate guard helper if it keeps the tick readable; the
  source-contract tests will check the reclaim path, the same-rate condition,
  and the demotion calls.
- Update the README clock paragraph to describe the reconnect behavior.
- Build with `tools/build-and-copy.ps1` and run the clock/failover tests plus
  the full suite.

## Left to the builder

- Final wording of the reclaim log and the rate-mismatch message.
- Whether the retry counter should restart after a long idle period in
  addition to the absence transition.
- Exact non-clock `setTargetFillBlocks` value when demoting (match the normal
  open path's headroom).
