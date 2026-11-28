# Audit backlog: remaining work

Source: audit sections 6.2 (layout) and 7 (feature completeness), explained
2026-11-28. Decisions from the user:

- Snapshots (7.3) are being implemented separately: leave the scaffolding alone.
- Routing page: remove the UI only (`RoutingMatrixComponent`, its tab, menu
  entry, tests). Keep the engine routing matrix and the Mixer routing chips.
- Keyboard shortcuts (7.19): skipped.
- 7.12: implement WASAPI loopback capture. VST2 stays documented as absent
  (no redistributable SDK).
- 7.13: keep the endpoint format rewrite, disclose it, restore the original on
  exit.
- Order: cheap + visible first, then high damage, then big rocks.

## Tier 1 - cheap, visible

| Item | What | Files | Status |
| --- | --- | --- | --- |
| A1 | Patches canvas height includes the outputs column | `app/src/PatchesView.cpp` | done |
| A3 | Mixer input edit controls wrap instead of leaving an 18 px chip | `app/src/ChannelStrip.cpp` | done |
| A4 | Rack outputs column scrolls | `app/src/RackView.cpp` | done |
| A5 | FX window minimums + cascade | `Theme.h`, effect windows | done |
| 7.8 | First scan refreshes an open plugin menu | `app/src/PluginMenu.cpp` | done |
| 7.15 | Patches selection stored by id + kind | `app/src/PatchesView.*` | done |
| 7.17 | EQ tooltip, Multiband/De-esser/Limiter preset labels | `app/src/*Window.cpp` | done |
| Routing | Remove the Routing page and its code | `MainComponent.*`, CMake, README | done |

## Tier 2 - high damage

| Item | What | Files | Status |
| --- | --- | --- | --- |
| 7.1 | Scene switch keeps matching streams open | `app/src/AudioEngine.cpp` | done |
| 7.2 | Capture opens off the message thread, one bulk open | `app/src/AudioEngine.*`, `ChannelStrip.cpp` | done |
| 7.5 | Save Scene As confirms overwrite | `MainComponent.cpp`, `SceneBridge.cpp` | done |
| 7.6 | Named-save dirty tracked apart from autosave | `MainComponent.cpp` | done |
| 7.10 | Refuse a bundle whose saved class id is missing | `app/src/PluginHost.cpp`, `RestrictedHost.cpp` | done |
| 7.13 | Disclose + restore the endpoint default format | `app/src/DeviceStream.*`, `SettingsView.cpp`, `Main.cpp`, README | done |
| 7.14 | Routing chips remember the last non-off gain | `app/src/ChannelStrip.cpp`, engine | done |
| 7.16 | Rename dialog per strip, validated, right gesture | `StripDialog.*`, `AudioEngine.cpp`, `MixerComponent.cpp`, `DeviceControl.h` | done |

## Tier 3 - big rocks

| Item | What | Files | Status |
| --- | --- | --- | --- |
| 7.4 | Saves page lists autosaves, empty state, selection drives buttons | `SavesView.*`, `MainComponent.cpp`, README | done |
| 7.11 | Output channel-pair picker | `app/src/DeviceStream.*`, `AudioEngine.cpp`, device UI | done in working tree (schema 12, built, 491/491) |
| 7.12 | WASAPI loopback capture device type | `app/src/DeviceStream.*`, `AudioEngine.cpp` | done in working tree (built, 488/488 tests; loopback endpoint not hardware-verified) |
| 7.20 | Restore the session from the newest autosave after a crash | `Main.cpp`, `MainComponent.*`, `SceneBridge.*` | done |
| 7.18 | DSP depth (EQ slope/shelf Q, gate hold+hysteresis+sidechain, compressor RMS+dry/wet, limiter true peak, LR crossovers, real trace curves, RMS/LUFS meters, denoiser FFT size) | `app/src/*Window.cpp`, `app/src/*Engine*` | done: EQ shelf-Q + 12/24/48 dB/oct slope (schema 13); gate hold/hysteresis/side-chain HPF, compressor RMS/mix, limiter true peak (schema 14); LR4 crossovers and traces from the real filter response; denoiser FFT size (schema 15); momentary LUFS (BS.1770 K-weighting) row on every channel meter. 523/523 |
| 7.21 | Docs drift (PRODUCT slots, README Saves/Racks, improvement.md) | `*.md` | done: PRODUCT slot count and VST2 wording, README Saves/Racks/routing/loopback, improvement.md test counts, SceneValidator chain cap 16 -> 80 |

## User test follow-ups (round 2)

| Item | What | Status |
| --- | --- | --- |
| 2 | Save Scene As never created a save | done: the dialog passed `deleteWhenDismissed = true` and also deleted itself in the callback (double delete). `MainComponent.cpp` now passes `false` |
| 10 | Meter readout RMS vs LUFS | done: `LevelMeter::Readout`, button beside Peak hold in Settings |
| 13 | Output pair must start on any channel | done: `firstChannel` accepts any 0..62, I/O picker lists every start channel ("From N"), engine no longer snaps odd offsets |
| 14 | Mono split made one strip | done: `splitIntoMonoChannels` adds every strip before any mapping edit (a mapping edit starts the background reopen and `addChannel()` refuses while it runs); names stop accumulating ("Input 1 1") |
| 14 | Input error stayed after narrowing the mapping | done: the reuse path recomputes `inputError` from the strip's own `inputChannels` against the capture's available inputs |
| 24 | Dirty marker semantics | kept as is by user choice: asterisk means "not in a named save"; autosave does not clear it |
| 25 | "Load Scene" not found | done: the menu now has a **Scene** submenu with Save Scene As..., Load Scene, Restore, Open Folder |
| 26 | EQ slope "goes back to 12" | done: the slope combo was disabled on bells/shelves and read 12; it is now hidden unless a high-pass or low-pass point is selected |
| - | Spectral denoiser could not learn again after Clear | done: `startLearning()` drops a stale ready profile and resets progress, so a new Learn starts at 0% instead of handing back the old profile |
| 30/31 | True peak, LR crossovers | covered by unit tests; offline chain tests added for the limiter ceiling and the EQ slope through the whole render path |

## Desktop automation pass (2026-11-28)

Drove the release build through Windows UI Automation plus synthetic mouse
input (`%LOCALAPPDATA%\Temp\opencode\pluginrack-auto\{uia,mouse}.ps1`,
screens `ui-01`..`ui-62`). The user's scene folder was backed up first and
restored afterwards; test saves were deleted.

| Item | Runtime result |
| --- | --- |
| 2 | PASS. Save Scene As created a new file without touching the old one; empty name shows "Please enter a save name." |
| 10 | PASS. Settings button toggled RMS -> LUFS; Mixer strips grew the LUFS row, output strips kept Peak/RMS |
| 13 | PASS. Picker lists one item per device channel (2 for both USB endpoints), "From 2" persisted as `firstChannel: 1` in a saved scene; any start channel is accepted |
| 14 | PASS. Split on a stereo CABLE strip produced both mono strips (A1, A2) with correct chips and names |
| 16 | PASS. Mapping to A3 showed "Missing input 3"; mapping back to A2 cleared it on both strips |
| 9 | PASS. Dynamics (670,380), Feedback Suppressor (932,416), EQ (742,452): each new window is offset, none stacked |
| 20 | PASS. SIGKILL then relaunch showed the restore prompt; "Restore" loaded the last autosave |
| 25 | PASS. Menu shows the Scene submenu with all four entries |
| 12 | Loopback device is present in the picker and already carries the scene's mono strip; audio audibility not measured |
| 11 | Chip toggling works and saves `-60` when off; the remembered-gain path could not be driven because no UI sets a non-zero send level any more (routing page removed) |
| 17 | Neutral. Both endpoint default formats are byte-identical before launch, while running and after exit; this run never rewrote them, so the restore had nothing to undo. Restore call and Settings disclosure are in place |
| 19 | Not run. Needs the first-scan state (plugin cache wiped) and the menu opened during the scan |
| 26 | Not driven. Raw clicks on the EQ curve did not change the selected band under automation, so the slope combo could not be exercised; the combo is hidden for non-filter bands by code |
| 30/31 | Covered by the offline tests only |

Found during the pass: `splitBaseName` only removed one trailing number, so a
second split produced "Input 1 2 1". It now strips every trailing `" N"`
segment. The I/O picker tooltips said "channel pair"; they now say "start
channel".

## Desktop automation pass, round 2 (2026-11-28)

Second pass over the follow-up items, plus the fixes from the user's retest.

| Item | Runtime result |
| --- | --- |
| Autosave | PASS. Edit -> log "autosave: change detected"; 30 s later "wrote last scene", `last.json` timestamp moved |
| Dirty marker | PASS. `Save*` right after an edit, still `Save*` after the autosave wrote, plain `Save` after Save Scene As |
| Overwrite confirm | PASS. Save Scene As onto an existing name shows "A save named <name> already exists. Overwrite it?"; Cancel leaves the file byte-identical |
| Saves page | PASS. Rows list named saves plus the read-only Autosave row; Return loads, F2 opens "Rename Save", Delete confirms and removes the file |
| Rename validation | PASS after fix. Empty and duplicate names are refused; a valid rename applies. A duplicate used to retitle the strip while the engine kept the old name (`ChannelStrip` titled from the request instead of the engine); it now titles from the engine. Empty names are refused silently, which reads as "nothing happened" |
| Rack outputs scroll | PASS after fix. 18 outputs fit; at ~40 the scrollbar appeared and the wheel scrolled to Output 50. Adding past the validator cap used to make every autosave fail ("scene has 40 outputs, expected at most 32"); `+ Out`/`+ Ch` now stop at the shared caps in `SceneLimits.h` |
| First-scan menu refresh | Not reproduced. A rescan finishes in 0.1-1 s, so the menu reopened after the scan had already ended. Code path and the disabled "Scanning plugins..." row are in place |
| EQ button state | Confirmed by screenshot: the EQ chain card shows B and × like the other cards |

Note: the window close button hides to the tray when `closeToTray` is on
(default). The process staying alive after Close is that feature, not a leak;
with the setting off, Close exits, writes the clean-exit marker and restores
endpoint formats.

## Verification

Every item: build with `powershell -ExecutionPolicy Bypass -File .\tools\build-and-copy.ps1`,
run the test suite, add or update the named test. Smoke results are recorded or
reported as not possible (audio/device items need real hardware).
