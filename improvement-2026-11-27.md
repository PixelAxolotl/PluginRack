# PluginRack audit and remediation plan (2026-11-27)

This file is a work order for an implementation agent. It lists every defect,
performance problem, layout problem and half-finished feature found in the
2026-11-27 audit, with the evidence, the trigger, the consequence and the
concrete fix. Work it top to bottom: section 1 is what the owner reported,
sections 2–4 make the app stable and correct, sections 5–6 are responsiveness
and layout, section 7 is feature completeness.

All line numbers refer to commit `ef10049` (`fix(patches): key taps and chain
rows by id and type`). They will drift as you edit: always re-locate by the
function or symbol name given next to the line.

---

## 0. Ground rules for the implementing agent

### 0.1 How this audit was done (read before trusting an item)

- Five independent static reviews (engine, devices/plugin hosting, core DSP,
  Rack view, other UI views), followed by spot checks of the highest-impact
  claims against the source. The app was **not** built or run: it is
  Windows/MSVC/JUCE-only and the audit ran on Linux.
- The core library and tests **were** built and run on Linux (g++ 13) with
  `-DPLUGINRACK_BUILD_APP=OFF`: 478 test cases, 477 pass, 1 fails (item 2.1).
- Core DSP findings marked "probe-confirmed" were reproduced numerically with
  throwaway programs linked against `libpluginrack_core.a`, plus an ASan/UBSan
  fuzz of every processor (4 sample rates, blocks up to 3x prepared size,
  NaN/Inf/denormal/silence, extreme parameters). The fuzz found no memory
  error; its only finding is item 4.9.
- Every item carries a **Confidence**:
  - *Verified* – the full path was traced and re-checked against the source.
  - *Probe-confirmed* – reproduced numerically (core only).
  - *Traced* – one reviewer traced the full path; not independently re-read.
  - *Suspected* – depends on plugin, driver or OS behaviour that could not be
    observed. Confirm on Windows before relying on it.

### 0.2 Repository rules that still apply

- Build after every source/config change, from the repo root:
  `powershell -ExecutionPolicy Bypass -File .\tools\build-and-copy.ps1`
  (copies `PluginRack.exe` to the root). Do not claim completion with a stale exe.
- Tests: `ctest --test-dir build/dev --output-on-failure` (Catch2,
  `pluginrack_tests`). CI (`.github/workflows/ci.yml`) builds core+tests only.
- MSVC `/W4 /WX`: keep new code warning-clean.
- Commits: Conventional Commits, **no `Co-authored-by` or AI-attribution
  trailers** (`.githooks/commit-msg` rejects them).
- `tests/test_underrun_paths.cpp` greps source text for invariants. When a fix
  changes a literal it pins, update the test in the same commit and say why.
  Items below name the affected checks.
- README "Development notes" invariants are binding: no allocation or blocking
  on the audio thread; the UI never waits on the audio thread; device opens go
  through the background opener guarded by `openingDevices_`; views never read
  worker-owned device strings directly; `makeScene()` never holds `stateLock`
  across plugin code; Reset to Defaults must stay complete.
- One logical fix per commit. Each commit message should name the item number
  from this file (for example `fix(rack): build docked editor without pluginLock (1.1)`).

### 0.3 Measuring section 1

The Rack swap already logs its phases to `%APPDATA%\PluginRack\logs\PluginRack.log`:
`plugin: editor <ms>` (plugin UI creation), `rack: swap freeze <ms>`,
`rack: swap create`, `rack: swap layout`, `rack: swap fitted at <ms>`,
`rack: swap done <ms> ticks=<n> captures=<n>`, `rack: editor docked <ms>`.
Before changing anything in section 1, record these for 10 switches between
two plugins (one light JUCE plugin, one heavy one such as Ozone, plus any GPU
plugin you have). Record them again after each item. Report the before/after
table in the PR. Target: the time from click to `rack: swap done`, minus
`plugin: editor`, below ~60 ms.

---

## 1. Owner-reported: delay between clicking an insert and its editor appearing in Racks

The owner's words: "there is a delay from pressing to it being on the main
editor page". Clicking a VST card (`InsertSlotComponent::mouseDown`,
`app/src/InsertHost.h:479`) calls `RackView::insertActivated`
(`RackView.cpp:3853`) → `RackView::refreshEditor()` (`RackView.cpp:1294`)
on mouse-down, so the press itself is not delayed. Everything below happens
after it. Items are ordered by expected impact.

### 1.1 Editor creation holds `pluginLock`, so the insert goes dry while the UI is built

- **Severity:** audio dropout on every editor switch, plus switch latency.
  **Confidence:** Verified.
- **Where:** `PluginHost::createEditorForDock` (`app/src/PluginHost.cpp:709-722`)
  and `PluginHost::showEditor` (`:685-697`) hold `pluginLock` across
  `plugin->createEditorAndMakeActive()`. The in-process render path
  `PluginHost::processBlock` does `ScopedTryLock lock(pluginLock); if (!lock.isLocked() ...) return;`
  (`:1250-1253`), i.e. it skips the plugin and passes the block through dry.
- **Trigger/consequence:** every card click (and every floating-editor open)
  bypasses that insert for the whole UI build, 50 ms to 2.5 s depending on the
  plugin (the code's own comments quote 1–2.5 s). A limiter or compressor
  audibly drops out of the chain each time the user switches editors.
- **Why it is safe to drop the lock:** every path that replaces or destroys
  the instance runs on the message thread (`adopt` at `AudioEngine.cpp:1924`,
  `:2641` inside `callAsync`; `loadNextPendingInsert` on the restore timer;
  `retireInsert`/`releaseResources` on the message thread). Editor creation is
  also message-thread only, so the instance cannot change underneath it.
  Rendering concurrently with editor creation is what every DAW does.
- **Fix:**
  ```cpp
  juce::AudioProcessorEditor* PluginHost::createEditorForDock() {
      JUCE_ASSERT_MESSAGE_THREAD
      closeFloatingEditor();
      // Instance replacement is message-thread only, so a reference taken
      // here stays valid. pluginLock is deliberately not held while the
      // plugin builds its UI: the in-process render only try-locks it and
      // leaves the insert dry for as long as the lock is held.
      std::shared_ptr<juce::AudioPluginInstance> instance;
      {
          const juce::ScopedLock lock(pluginLock);
          instance = plugin;
      }
      if (instance == nullptr || !instance->hasEditor()) return nullptr;
      const ScopedPluginLoadMark loadMark(juce::File(/* this insert's plugin path */));
      const auto start = juce::Time::getMillisecondCounterHiRes();
      auto* editor = instance->createEditorAndMakeActive();
      juce::Logger::writeToLog("plugin: editor " + ... );
      return editor;
  }
  ```
  Apply the same pattern to `showEditor` (both the `getActiveEditor()` check
  and the creation). `ScopedPluginLoadMark` (`PluginHost.cpp:95`) is added so a
  crash inside editor creation is attributed and block-listed like a crash
  during load (today an editor-creation crash is never blocked and can loop on
  restore; see 3.12). PluginHost needs the plugin path for that: store it at
  `adopt` time if it is not already available.
- **Verify:** route a sine through a limiter VST, switch editors repeatedly and
  confirm no level jumps on the output meter; `plugin: editor` timings unchanged.

### 1.2 The freeze frame does a whole-window screen copy and a slow pixel loop on the message thread

- **Severity:** switch latency (tens of ms per click, more at 4K).
  **Confidence:** Verified (cost magnitude unmeasured).
- **Where:** `RackView::refreshEditor`, freeze block `RackView.cpp:1424-1509`;
  `nativedisplay::captureScreenRegion` and `toImage` (`app/src/NativeThumbnail.cpp:37-66`, `:203-244`).
- **Mechanism:** on every swap the message thread: enumerates all process
  windows (`hideOffscreenJuceChildren`), BitBlts the **whole app window**
  (e.g. 2400x1350 physical px at 150 %), copies it with `GetDIBits`, then runs
  `toImage`, a per-pixel loop with a 3-byte `memcpy`, an alpha write and a
  branchy "uniform" check for every pixel (3–8 M iterations). It then creates a
  new always-on-top desktop window (`frozenOverlay->addToDesktop`) and paints
  the full-window image into it (`performAnyPendingRepaintsNow`), then runs two
  more window enumerations.
- **Fix (in order of value/risk):**
  1. Make `toImage` a branch-free 32-bit loop that the compiler vectorises, and
     build a software image (it is also used on the capture worker, where a
     native Direct2D image costs GPU work per lock):
     ```cpp
     juce::Image toImage(const void* dibPixels, int width, int height, bool* uniform) {
         juce::Image image(juce::Image::ARGB, width, height, false, juce::SoftwareImageType());
         juce::Image::BitmapData dest(image, juce::Image::BitmapData::writeOnly);
         jassert(dest.pixelStride == 4);
         const auto* source = static_cast<const std::uint32_t*>(dibPixels);
         // DIB rows are B,G,R,X bytes = 0xXXRRGGBB, the PixelARGB layout on
         // little-endian Windows; only alpha needs forcing.
         const std::uint32_t first = source[0] & 0x00ffffffu;
         std::uint32_t diff = 0;
         for (int y = 0; y < height; ++y) {
             const auto* src = source + static_cast<std::size_t>(y) * static_cast<std::size_t>(width);
             auto* dst = reinterpret_cast<std::uint32_t*>(dest.getLinePointer(y));
             for (int x = 0; x < width; ++x) {
                 const std::uint32_t p = src[x];
                 diff |= (p ^ first) & 0x00ffffffu;
                 dst[x] = p | 0xff000000u;
             }
         }
         if (uniform != nullptr) *uniform = (diff == 0);
         return image;
     }
     ```
     Update the three callers (`captureScreenRegion`, `captureWindowForOverlay`,
     `captureWindow`); pass `nullptr` where the uniform flag is unused.
  2. Keep one overlay component alive for the RackView's lifetime (created on
     first use, `setVisible(false)` between swaps) instead of creating and
     destroying a top-level window per click.
  3. Only if measurements still show the freeze above ~15 ms: capture the bay
     plus the strip above it rather than the whole window. The comment at
     `RackView.cpp:1470-1473` explains why the whole window is covered (plugin
     windows taller than the bay paint over the side-chain bar/top bar), so
     keep the whole-window *overlay bounds* but only re-capture the bay area
     and reuse the last full capture for the rest, or skip the overlay when no
     stray plugin window is detected. Test with the plugins named in the
     comments (Ozone, Ripeness) before keeping this.

### 1.3 Fixed polling waits in the swap state machine

- **Severity:** switch latency (≥ ~100 ms of pure waiting per switch; up to
  ~400–700 ms in the slow paths). **Confidence:** Verified.
- **Where:** the `poll`/`finish` lambdas in `RackView::refreshEditor`
  (`RackView.cpp:1649-1862`).
- **Mechanism:**
  - The first poll is scheduled 16 ms out (`callAfterDelay(16)` at `:1862`).
  - Phase 1 waits for the editor to report a fitted size on **two consecutive
    polls** (`state->ticks >= 2`, `:1818-1819`), each 16 ms apart, or 25 polls
    (~400 ms) if it never fits.
  - Phase 2 with the freeze overlay active waits a further **4 polls**
    (`shownTicks >= 4`, `:1792`) before revealing.
  - Without an active freeze (capture failed), phase 2 waits up to **20 polls**
    (~320 ms) and issues up to 12 PrintWindow readiness probes from tick 6.
  - Minimum: ~6 polls ≈ 100 ms after the plugin's editor already exists.
  (JUCE sets `timeBeginPeriod(1)` in `juce_SystemStats_windows.cpp`, so the
  16 ms delays are accurate; the cost is the count, not timer granularity.)
- **Fix:**
  1. Run the first poll immediately with `juce::MessageManager::callAsync`
     instead of after 16 ms; the synchronous layout at `:1622-1637` has already
     applied the fit, so most plugins are fitted on that first check.
  2. Replace the magic numbers with named constants at the top of the file
     (`kSwapFitStablePolls`, `kSwapFrozenSettlePolls`,
     `kSwapUnfrozenMaxPolls`, `kSwapFitMaxPolls`) and lower them: fit stable
     2 → 1 when the fitted size equals the size computed by the synchronous
     layout (otherwise keep 2); frozen settle 4 → 2, and before counting call
     `nativedisplay::repaintWindowNow(incomingWindow)` once the incoming
     window is shown under the overlay so GDI/JUCE-software plugins have
     painted synchronously.
  3. Keep the existing safety caps; only the common path should get faster.
  4. Tune with the logs from 0.3 and watch for white/black flashes on reveal
     with: one JUCE software plugin, one OpenGL/GPU plugin, Ozone, Ripeness.
     If a plugin flashes, prefer a per-plugin fallback (keep 4 polls when the
     readiness probe or `repaintWindowNow` shows a uniform frame) over raising
     the global constant.

### 1.4 Every log line opens, appends to and closes the log file on the calling thread

- **Severity:** switch latency and general UI cost. **Confidence:** Verified
  (JUCE `FileLogger::logMessage` constructs a `FileOutputStream` per message).
- **Where:** `TimestampedLogger` (`app/src/Main.cpp:45-58`) forwards to
  `juce::FileLogger`. A single swap writes ~10 lines (`rack: swap freeze`,
  `rack: natural`, `rack: swap create`, `rack: fit ...`, `rack: editor placed`,
  `rack: swap layout`, `rack: swap fitted`, `rack: swap done`,
  `plugin: editor`, `rack: editor docked`), each a CreateFile/Write/Close on
  the message thread. With antivirus real-time scanning each can cost
  milliseconds. Layout paths also log on resizes (`RackView.cpp:2022`, `:2080`).
- **Fix:** make the logger asynchronous with an explicit flush:
  ```cpp
  class TimestampedLogger final : public juce::Logger, private juce::Thread {
  public:
      TimestampedLogger(const juce::File& file, const juce::String& startMessage)
          : juce::Thread("PluginRack log"), sink(file, startMessage),
            startMs(juce::Time::getMillisecondCounterHiRes()) {
          startThread(juce::Thread::Priority::low);
      }
      ~TimestampedLogger() override { stopThread(2000); flush(); }

      void logMessage(const juce::String& message) override {
          const auto elapsed = juce::Time::getMillisecondCounterHiRes() - startMs;
          {
              const juce::ScopedLock lock(queueLock);
              pending.add(juce::String(elapsed, 1).paddedLeft(' ', 9) + " ms  " + message);
          }
          wake.signal();
      }
      // Writes everything queued so far on the calling thread.
      void flush() {
          juce::StringArray batch;
          { const juce::ScopedLock lock(queueLock); batch.swapWith(pending); }
          if (!batch.isEmpty())
              sink.logMessage(batch.joinIntoString(juce::newLine.getDefault()));
      }
  private:
      void run() override {
          while (!threadShouldExit()) { wake.wait(200); flush(); }
      }
      juce::FileLogger sink;
      double startMs = 0.0;
      juce::CriticalSection queueLock;
      juce::StringArray pending;
      juce::WaitableEvent wake;
  };
  ```
  Then call `flush()` explicitly on every path that ends the process without
  running destructors: the shutdown `TerminateProcess` (`Main.cpp:~1024`), the
  headless switches that quit (`--test-plugin`, `--list-*`, `--scan-plugin`,
  `--test-child-*`, `--test-crash-report`) and anywhere else `std::exit`,
  `_exit` or `TerminateProcess` is used (grep for them). The crash handler
  writes its own report and must **not** call `flush()` (it may allocate).
  Accept that a hard crash can lose the last few ms of queued lines; the crash
  report carries the backtrace.
- Also drop the per-resize `rack: editor placed` log, or log it only during a
  swap.

### 1.5 The docked editor is screen-captured continuously while the user works in it

- **Severity:** sluggish editor interaction (up to ~25 % of the message thread).
  **Confidence:** Verified.
- **Where:** `RackView::timerCallback` thumbnail block (`RackView.cpp:1171-1210`)
  and `nativedisplay::WindowCaptureWorker` (`NativeThumbnail.cpp:528-608`).
- **Mechanism:** every tick (Settings "Rack thumbnail refresh", default
  15 fps, max 60) asks the worker to `PrintWindow(PW_RENDERFULLCONTENT)` the
  docked plugin's window. PrintWindow sends WM_PRINT to the window's owner,
  the message thread, so the plugin re-renders its whole UI there. The worker
  only sleeps 3x the capture cost between captures, so up to a quarter of
  message-thread time goes to repainting the plugin for a card thumbnail of
  the editor that is already on screen beside it. GPU plugins that return a
  flat frame are discarded and re-captured forever with no backoff.
- **Fix (draft implemented and reverted during the audit; use as reference):**
  ```cpp
  // RackView.h, next to lastSnapshotMs:
  juce::uint32 lastVstCaptureMs = 0;
  juce::uint32 lastBayInteractionMs = 0;
  bool editorBayInUse() const;

  // RackView::timerCallback, replacing the wantsThumbnail block:
  const auto nowMs = juce::Time::getMillisecondCounter();
  if (wantsThumbnail && editorBayInUse()) lastBayInteractionMs = nowMs;
  constexpr juce::uint32 kVstThumbnailIntervalMs = 500;
  constexpr juce::uint32 kVstThumbnailIdleMs = 600;
  const bool bayIdle = nowMs - lastBayInteractionMs >= kVstThumbnailIdleMs;
  const auto thumbnailIntervalMs = juce::jmax(
      kVstThumbnailIntervalMs, static_cast<juce::uint32>(1000 / juce::jmax(1, fps)));
  if (wantsThumbnail && bayIdle) {
      if (auto* pluginWindow = nativedisplay::pluginWindowOf(*dockedEditor)) {
          if (nowMs - lastVstCaptureMs >= thumbnailIntervalMs) {
              lastVstCaptureMs = nowMs;
              /* existing thumbnailWorker.request(...) */
          }
      } else {
          /* existing software-snapshot branch, with
             interval = jmax(thumbnailIntervalMs, 4 * lastSnapshotCostMs) */
      }
  } else if (!swappingEditor) {
      // Never during a swap: the swap's readiness probe uses the same worker.
      thumbnailWorker.clearRequest();
  }

  bool RackView::editorBayInUse() const {
      // Plugin UIs are native windows; JUCE never sees clicks inside them.
      const auto pointer = juce::Desktop::getInstance().getMainMouseSource().getScreenPosition();
      return editorHolder.getScreenBounds().toFloat().contains(pointer)
             || juce::ModifierKeys::getCurrentModifiersRealtime().isAnyMouseButtonDown();
  }
  ```
  Also reset `lastVstCaptureMs = 0` where a new editor is docked (next to
  `knobWatcher.clear(); thumbnailWorker.clearRequest();`, `RackView.cpp:1527-1528`).
- **Bug fixed by the same change:** today the `else` branch calls
  `thumbnailWorker.clearRequest()` on every tick while `swappingEditor` is
  true, which can cancel the swap's own readiness capture
  (`RackView.cpp:1785`); its callback then never fires, `capturePending` stays
  true, and the swap falls through to the 20-poll cap (part of 1.3).
- **Further improvements:**
  - After 3 consecutive null/flat captures of the same window, stop capturing
    it until the editor changes (GPU plugins never answer PrintWindow).
  - When a swap starts, crop the outgoing editor's area out of the freeze
    image and use it as that insert's thumbnail (`applyVstThumbnail`), so the
    card is current without another PrintWindow.
  - Downscale inside GDI (`StretchBlt` with `HALFTONE` into a target-size DIB)
    in `captureWindow` instead of copying the full-size bitmap and calling
    `Image::rescaled`.
  - Update the Settings tooltip (`SettingsView.cpp:372-374`): the rate drives
    the built-in cards; VST cards refresh at most twice a second and pause
    while the pointer is over the editor.

### 1.6 Plugin parameter changes flood the message thread (and allocate on the audio thread)

- **Severity:** sluggish UI with plugins that move parameters constantly;
  real-time rule violation. **Confidence:** Verified.
- **Where:** `PluginKnobWatcher::parameterValueChanged` (`app/src/PluginKnobWatcher.cpp:72-82`),
  `post()` (`:34-41`), `bind()` (`:84-133`).
- **Mechanism:** JUCE reports a hosted VST3's output parameter changes from
  the audio thread. Each change calls `MessageManager::callAsync` (a heap
  allocation and PostMessage on the audio thread). On the message thread each
  message runs `bind()`: `getName()`, four `std::function` allocations and
  `slider_touch::recordDial`, which repaints the touch strip. Plugins that
  expose meters or LFO-modulated parameters generate this per block.
- **Fix (draft implemented and reverted during the audit):**
  - `PluginKnobWatcher` also derives privately from `juce::Timer`.
  - `parameterValueChanged` only does
    `pendingValueIndex.store(parameterIndex, std::memory_order_relaxed);`
    (`std::atomic<int> pendingValueIndex{-1}`).
  - `setPlugin` starts a 20 Hz timer; `clear()` stops it, resets
    `pendingValueIndex` to -1 (after `removeListener`, which waits for an
    in-flight callback because JUCE's listener list is locked).
  - `timerCallback`: `const int index = pendingValueIndex.exchange(-1);` then the
    same guards as before (`gestureIndex >= 0`, `slider_touch::isDriving()`,
    `suppressUntilMs`) and `bind(index)`. Exchange *before* the guards so a
    change made during a gesture is dropped.
  - Add `std::uint32_t slider_touch::generation()` (incremented in `record`,
    `recordDial`, `clear`) and remember `boundIndex`/`boundGeneration` in the
    watcher; `bind()` returns early when the same parameter is already the
    current dial, so an unchanged target is not rebuilt and repainted.
  - Keep gestures on `post()` (rare, UI thread).
  - Also skip non-automatable / read-only parameters in `bind()`
    (`!parameter->isAutomatable()` or a meter category), so a meter output
    cannot hijack the touch strip.
- **Test:** none pins this; a Catch2 test is not practical (JUCE). Verify by
  docking a plugin with animated parameters and checking that the touch strip
  follows the last knob touched and the UI stays smooth.

### 1.7 The docked editor is re-fitted on every timer tick for some plugins

- **Severity:** sluggish editor, log I/O at tick rate. **Confidence:** Traced
  (plugin-dependent).
- **Where:** `RackView::timerCallback` calls `layoutDockedEditor()` every tick
  (`RackView.cpp:920`) → `applyDockedEditorFit` (`:2046-2101`),
  `componentMovedOrResized` (`:2113-2148`).
- **Mechanism:**
  - A plugin without content-scale support comes back at its old size, so
    `dockedEditorScale = rescaled ? fit : 1.0f` (`:2079`) stores 1.0; the
    early-out `abs(fit - dockedEditorScale) < 0.01` (`:2060`) then never hits,
    and **every tick** calls `setScaleFactor(fit)` (a COM call into the plugin
    plus a resize) and writes a `rack: fit` log line.
  - A plugin that applies the scale asynchronously reports its new size later;
    `componentMovedOrResized` then reads it as "the plugin chose a new size"
    and divides by `dockedEditorScale == 1.0`, corrupting
    `dockedEditorNaturalSize`; the next fit is wrong and the two fight.
  - A plugin that settles 1 px larger than the clamped placement fails
    `getBounds() != placement` (`:2017`) every tick: `setBounds` → plugin
    reverts → repeat, with a log line each time.
- **Fix:**
  1. Add `float dockedEditorRequestedFit = 0.0f;` set after `setScaleFactor`,
     reset wherever `dockedEditorScale = 1.0f` is reset. Early-out in
     `applyDockedEditorFit` when `abs(fit - dockedEditorRequestedFit) < 0.01f`.
  2. In `componentMovedOrResized`, if `dockedEditorRequestedFit` differs from
     `dockedEditorScale` and the new visual size is within 2 % of
     `natural * dockedEditorRequestedFit`, set
     `dockedEditorScale = dockedEditorRequestedFit` (the scale arrived late)
     instead of moving the baseline.
  3. In `layoutDockedEditor`, skip `setBounds` when the position matches and
     each size axis differs from `placement` by ≤ 2 px.
  4. Keep the per-tick call but it is now a no-op in the steady state.

### 1.8 The post-swap guard hides the app's own popups, dialogs and floating editors

- **Severity:** wrong behaviour right after a switch ("menus don't open",
  floating editors vanish for good). **Confidence:** Traced.
- **Where:** `RackView::timerCallback` swap guard (`RackView.cpp:926-941`),
  `maskIncomingWindow` (`:1911-1967`), the freeze block (`:1499-1505`) and
  `finish` (`:1690-1704`) call `nativedisplay::hideJuceWindowsInRect` and
  `hideOtherProcessWindows` (`NativeThumbnail.cpp:328-426`).
- **Mechanism:** for 3 s after every switch (extended by 1 s each time
  something is hidden) every visible window of this process that intersects
  the app is hidden with `SW_HIDE`, except the docked editor. During the swap
  every top-level window of the process is hidden. That includes JUCE
  PopupMenus and ComboBox lists, tooltips, AlertWindows and **floating plugin
  editor windows** opened from the Mixer (`PluginHost::showEditor`). Nothing
  re-shows them: `PluginHost::editorWindow` still exists, so clicking the
  plugin again only calls `toFront` on a hidden window and the editor cannot be
  brought back until it is closed by another path.
- **Fix:**
  1. Never hide a window that belongs to the app's own JUCE desktop. In
     `NativeThumbnail.cpp` add (message thread only):
     ```cpp
     bool isOwnDesktopWindow(HWND window) {
         auto& desktop = juce::Desktop::getInstance();
         for (int i = 0; i < desktop.getNumComponents(); ++i)
             if (auto* c = desktop.getComponent(i))
                 if (auto* peer = c->getPeer())
                     if (peer->getNativeHandle() == window) return true;
         return false;
     }
     ```
     and return early from `hideIfInRect`, `hideOtherTopCallback` and
     `hideIfOffscreen` for such windows. In-process JUCE plugins link their
     own JUCE copy with its own `Desktop`, so plugin-created stray windows are
     still caught.
  2. End the post-swap guard as soon as the user interacts
     (`if (editorBayInUse()) swapGuardUntilMs = 0.0;` before hiding), and stop
     extending it by 1 s per hide.
  3. `hideOffscreenJuceChildren` hides any `JUCE_*` window whose top edge is
     above the app; apply the same own-desktop exclusion so a floating editor
     the user placed above the app is left alone.

### 1.9 The side-chain bar blocks on the audio lock 20 times a second

- **Severity:** UI stalls while a VST editor is docked (the bar is visible
  whenever one is). **Confidence:** Verified.
- **Where:** `SideChainBar::timerCallback` (`app/src/SideChainBar.cpp:137-150`,
  timer started at `:134`, never stopped) → `AudioEngine::getSideChainKeyPeak`
  (`AudioEngine.cpp:3529-3551`) takes `ScopedLock(stateLock)`, which
  `render()` holds for the whole block including plugin DSP
  (`AudioEngine.cpp:771`).
- **Fix:** gate the timer on `isShowing()` (start/stop it in
  `visibilityChanged`), and make the getter non-blocking: either
  `ScopedTryLock` and return the last value on a miss, or (better) have the
  bar cache the `InsertSlot*` it was given in `setTarget` together with the
  engine's structure revision and read `insert->sideChainKeyPeak.get()`
  directly while the revision is unchanged (the message thread is the only
  structural mutator, so the pointer is stable between revisions).
- Same pattern in `RackView::refreshSourceList` (4 Hz when an output/bus is
  selected) which calls `getChannelDeviceState` per routed channel, each a
  blocking `stateLock`.

### 1.10 Smaller Rack swap defects

- **Frozen frame painted at the wrong place** (Verified):
  `RackView::paintOverChildren` (`RackView.cpp:3346-3352`) and
  `self->repaint(self->editorHolder.getBounds())` (`:1731`) use
  `editorHolder.getBounds()`, which is relative to the editor viewport's
  content component, not to RackView. Use
  `getLocalArea(&editorHolder, editorHolder.getLocalBounds())`.
- **Mixed-DPI capture math** (Suspected): the freeze converts logical to
  physical with one `peer->getPlatformScaleFactor()`. Use
  `peer->getAreaCoveredBy(editorHolder)` scaled to physical pixels relative to
  the window rect.
- **Retired editors leak** (Traced): `destroyEditorWhenIdle`
  (`RackView.cpp:1267-1292`) does nothing if the RackView is gone within
  80 ms; a scene restore during a swap also skips the outgoing editor. Destroy
  outstanding retired editors in `releaseEditorsNow()` (keep a small list of
  pending retirements).

### 1.11 Optional phase 2: keep recently used editors alive

Only after 1.1–1.10 are in and measured. The biggest remaining cost is the
plugin's own `createEditorAndMakeActive` (50 ms–2.5 s). Keeping the last N
(2–3) docked editors alive but hidden (native window hidden and parked, JUCE
component invisible) would make switching back and forth between two plugins
instant. Risks that must be handled: JUCE allows one active editor per
processor (a floating `showEditor` must evict the cached one first); every
path that frees an insert (`releaseEditorsNow`, erase, reload, restore, bus
removal) must destroy its cached editor first; hidden JUCE plugin editors
keep running their timers (cap N, evict LRU); the window-hiding heuristics in
1.8 must treat cached editors as "keep". Prototype behind a Settings toggle
and test with Ozone, Ripeness, a GPU plugin and a plain JUCE plugin.

---

## 2. Build and test hygiene

### 2.1 CI is red: stale source-text test

- **Confidence:** Verified (Linux run: 477/478 pass).
- `tests/test_underrun_paths.cpp:626` expects `"Pending changes - press Apply"`
  in `SettingsView.cpp`; commit `4277bd9` changed the label to
  `"Settings pending - Apply to activate (Save stores the scene)"`
  (`SettingsView.cpp:681`). Update the check to
  `CHECK(contains(settings, "Settings pending - Apply to activate"));`.

### 2.2 Make the core build portable

- `core/CMakeLists.txt` passes `/W4 /WX` unconditionally, so core+tests only
  build with MSVC although core is documented as platform-independent. Wrap it:
  ```cmake
  if(MSVC)
      target_compile_options(pluginrack_core PRIVATE /W4 /WX)
  else()
      target_compile_options(pluginrack_core PRIVATE -Wall -Wextra)
  endif()
  ```
  With that change the full suite builds and runs with g++ 13 on Linux.
- g++ reports one `-Warray-bounds` warning in
  `FeedbackSuppressorProcessor::analyzeSpectrum` (`core/src/FeedbackSuppressorEffect.cpp`)
  from `std::sort` over a `std::array<float, 8>`; check the sort range and
  silence or fix it.
- Update `improvement.md`'s stale test counts when you finish.

---

## 3. Stability: crashes, hangs and data loss (fix before any feature work)

### 3.1 Device pickers run device opens on untracked threads (use-after-free, double render)

- **Severity:** crash / heap corruption; audio dropout. **Confidence:** Verified
  (thread launch) / Traced (consequences).
- **Where:** `ChannelStrip.cpp:64-72` and `MixerComponent.cpp:~56` call
  `engine->setChannelInputDevice` / `setOutputDevice` inside a bare
  `juce::Thread::launch`. Those calls never set `openingDevices_`, so every
  `isOpeningDevices()` guard passes while they run. They hold raw `Channel*` /
  `Output*` across opens that take 3–12 s (`setChannelInputDeviceImpl`
  `AudioEngine.cpp:4476-4696`, `setOutputDeviceImpl` `:4820-5172`).
- **Triggers/consequences:**
  - Remove the strip (or load a scene) while the open runs → the worker writes
    `channel->inputError` (`:4656`), `channel->input` (`:4565`) or
    `out->output` (`:5141`) into freed memory.
  - Two picks on the same endpoint race and double-open it.
  - The clock path runs `prepareToPlayImpl` (plugin `prepareToPlay`) on this
    ad-hoc thread (`:5112`), although the comment at `:5099` requires the
    message thread.
  - Changing the clock output's device: `out->output` is moved out (`:4907`),
    the 4 Hz `checkClockHealth` (`:5350`, driven from `MainComponent.cpp:439`)
    sees a null clock and promotes another output with `setRenderActive(true)`
    (`:5299`); the hand-off (`:5128-5164`) only deactivates `prevOutput`,
    which is null. Two outputs now drive `render()`: inputs are read at double
    speed and output FIFOs overflow until Reload All.
- **Fix:** route menu picks through the existing background opener: set the
  strip's device name under `stateLock` on the message thread, then call
  `reloadDevicesAsync({ the strip })` (it already uses `beginDeviceOpen` and
  the `openingDevices_` guard). Additionally, in the clock hand-off call
  `setRenderActive(false)` on every output except the new clock, and make
  `checkClockHealth` ignore a clock whose stream is mid-reopen. Remove the
  `Thread::launch` blocks. Add a source-text test that the pickers no longer
  call `Thread::launch`.

### 3.2 Capture devices are closed while holding `stateLock` (dropout, possible deadlock)

- **Severity:** audio dropout on every input change; possible permanent hang.
  **Confidence:** Verified (path) / Traced (deadlock interleaving).
- **Where:** `AudioEngine::pruneUnusedCaptures()` (`AudioEngine.cpp:4716-4725`)
  erases map entries, so `~InputCapture` runs `InputCapture::close()`
  (`DeviceStream.cpp:578-583`), which calls
  `DeviceThreading::runOnMessageThreadAndWait` (`DeviceStream.cpp:326-365`).
  It is called inside `stateLock` scopes at `AudioEngine.cpp:1632, 4491, 4516,
  4569, 4679` (verify each scope when editing). The duplex branch at
  `:4987-4991` closes under the lock too.
- **Consequences:**
  - On the message thread (e.g. `removeChannel`) the device close runs inside
    the audio lock: every output goes silent for the close (tens to hundreds
    of ms).
  - On a worker (picker thread, opener helper), the close posts to the message
    thread and **waits while holding `stateLock`**. If the message thread
    blocks on `stateLock` before dispatching that message (4 Hz
    `checkClockHealth`, `getClockDescription`, any locked strip getter), both
    threads wait forever and render misses every block.
- **Fix:** make `pruneUnusedCaptures` move retired captures into a
  `std::vector<std::shared_ptr<InputCapture>>` returned to (or owned by) the
  caller, and destroy that vector after the lock scope ends, exactly as
  `commitSceneRestore` and `releaseDeviceInputsForClock` already do with
  `retiredCaptures`. Also take the lock around the unlocked call in
  `retargetInputsToClockDevice` (`:4813-4816`; the header says "Requires
  stateLock").

### 3.3 Removing a strip with an EQ/Dynamics window open writes into freed memory

- **Severity:** crash / heap corruption. **Confidence:** Traced.
- **Where:** `MixerComponent.cpp:1381-1392`, `:1426-1437`, `:1476-1481` call
  `removeChannel` / `removeOutput` / `removeBus` (freeing the engine object,
  `AudioEngine.cpp:1607-1640`, `1785-1862`, `2310-2332`) and then
  `rebuild()`. The strip destructors (`ChannelStrip.cpp:206`, `:533-570`;
  `MixerComponent.cpp:182-223`) call `channel.dynamicsTap.setMode(...)` on
  the freed object and delete the EQ window, whose destructor
  (`EQWindow.cpp:308-311`) runs a lambda doing `safe->channel.eqTap.setMode()`
  (`ChannelStrip.cpp:485`).
- **Fix:** close the strip's effect windows (`closeAllEqWindows()` / the
  strip's close-all) **before** calling the engine remove, as scene load and
  Reset already do (`MainComponent.cpp:669`, `:927`). Same for the I/O view
  and RackView if they host effect windows for that strip.

### 3.4 The staged scene restore leaves the UI live, then frees strips under it

- **Severity:** crash (with an FX window open) and silent loss of edits.
  **Confidence:** Traced.
- **Where:** `MainComponent::applySceneAsync` (`MainComponent.cpp:909-943`)
  only refuses saves. Mixer, I/O, Routing and Rack stay editable while
  plugins load one per message turn; `AudioEngine::commitSceneRestore`
  (`AudioEngine.cpp:4241`, `:4429-4452`) then swaps the strips, discarding
  those edits, and `finished()` runs `refreshViews()` → `mixer.rebuild()`
  after the old strips are freed (same use-after-free as 3.3 if a window is open).
- **Fix:** while `isSceneRestoreActive()`, put a modal overlay (or disable
  input) over the view router with the existing "Loading scene… n/total"
  text; add a pre-commit hook (`AudioEngine::onBeforeRestoreCommit`) so
  `MainComponent` closes all effect windows and releases docked editors while
  the old strips are still alive.

### 3.5 Reset to Defaults can hang, is incomplete and cannot be undone

- **Severity:** UI hang; README rule violated. **Confidence:** Traced.
- **Where:** Reset callback `MainComponent.cpp:645-761`.
  - `while (getOutputs().size() < 3) addOutput();` (`:679`): `addOutput()`
    returns without adding while `isOpeningDevices()` (`AudioEngine.cpp:1740`).
    The opening check runs before the confirmation dialog (`:649`), not on
    confirm, so an auto-reload that starts while the dialog is open makes this
    loop forever on the message thread.
  - Not restored: EQ phase flip (`setChannelEqPhaseFlip`,
    `setOutputEqPhaseFlip`, `AudioEngine.h:741`, `:754`); buses, their inserts
    and bus→output routes survive.
  - `previousScene_` is never captured, although `MainComponent.h:154-157`
    says undo covers reset.
  - The trailing `saveScene()` writes `default.json`, which startup never
    loads (`SceneBridge.cpp:316` uses `loadDefaultOrLast`).
- **Fix:** move Reset into `AudioEngine::resetToDefaults()` that takes the
  opening guard, refuses (with a message) when devices are opening, and builds
  the default state in one step; capture `previousScene_` first; reset phase
  flip and remove buses; drop the `saveScene()` call. Add a core-level test
  that the reset state equals the documented default scene.

### 3.6 The engine lets the user build scenes the validator refuses to save

- **Severity:** data loss (autosave and exit save fail silently).
  **Confidence:** Verified (limits) / Traced (UI paths).
- **Where:** validator caps in `core/src/SceneValidator.cpp:39-42`, `:314`,
  `:499-509`: ≤ 16 chain rows per strip, ≤ 64 channels, ≤ 32 outputs,
  ≤ 1024 routes. The engine allows 64 VST inserts per strip
  (`AudioEngine.cpp:1873`), unlimited built-in chain rows (`:2082-2096`), and
  `addChannel` (`:1680`) / `addOutput` (`:1739`) have no cap. Past a cap,
  `SceneBridge` refuses save and load; exit autosave fails and the failure is
  only logged (`MainComponent.cpp:1146-1151`).
- **Fix:** move the caps into `core/include/pluginrack/SceneLimits.h` (as was
  done for buses), use them in the validator and in `addPluginAsync`,
  `addOutputPluginAsync`, the `ensure*ChainItem` functions, `addChannel` and
  `addOutput`, and surface a refusal message in the UI (disable + tooltip on
  the add buttons at the cap). Update validator tests to use the constants.

### 3.7 Large plugin states save fine but can never be loaded

- **Severity:** data loss. **Confidence:** Traced.
- **Where:** load refuses plugin state > 1 MiB base64 (`PluginHost.cpp:799`,
  `:1049-1060`) and scene files > 8 MiB (`SceneBridge.cpp:41`,
  `SceneLimits.h:10`). `save`/`saveLast` and the validator have no size check.
  An IR reverb, cab sim or sampler state saves, rotates into all five autosave
  files, and then restores as "too large" (plugin not loaded).
- **Fix:** validate sizes at save time with the same constants and keep the
  previous good file when refusing; raise the in-process limit (the 1 MiB cap
  was copied from the child IPC payload limit and should only apply to the
  child path); longer term, store large states as sidecar files next to the
  scene.

### 3.8 A hung plugin scan freezes startup forever

- **Severity:** hang on every launch (last scene auto-restores the same
  plugin). **Confidence:** Traced (depends on JUCE Windows
  `ChildProcess::readProcessOutput` blocking until the buffer fills or the
  child exits — confirm in the pinned JUCE source).
- **Where:** `PluginHost::scanFile` loop (`PluginHost.cpp:851-866`) calls
  `child.readProcessOutput(chunk, 4096)` and only then checks
  `kScanTimeoutMs`. A silent child (licence prompt, hang) never produces 4096
  bytes, so the timeout is unreachable. Restore calls this on the message
  thread via `loadNextPendingInsert` → `loadPlugin` → `loadFromFile` →
  `scanFile` (`AudioEngine.cpp:4217`). `PluginLibrary`'s scan thread wedges the
  same way.
- **Fix:** read stdout on a helper thread into a buffer, wait with
  `child.waitForProcessToFinish(timeout)` on the caller, kill on timeout.
  During restore, skip the scan when the plugin library already has a cached
  description for the same path + mtime, and run any remaining scan off the
  message thread (as `addPluginAsync` does).

### 3.9 Plugin state capture skips the plugin's audio (periodic dropouts)

- **Severity:** audio dropout every 30 s. **Confidence:** Verified.
- **Where:** `PluginHost::getStateAsBase64` holds `pluginLock` across
  `getStateInformation` (`PluginHost.cpp:332-336`);
  `MainComponent::checkFullSceneSafetyNet` (`MainComponent.cpp:1086-1113`,
  called at `:511`) runs `makeScene()` for every plugin every 30 s, even with
  no edits. Each capture leaves that insert dry for its duration (see 1.1 for
  why the render path goes dry).
- **Fix:** on the message thread, copy the `shared_ptr` under the lock and call
  `getStateInformation` outside it (same reasoning as 1.1). Keep the lock in
  `editorStateAsBase64`, which runs on a worker. See also 6.6 for reducing the
  safety-net cost.

### 3.10 Failed opens leave silent strips despite the "replace only after success" rule

- **Output** (Traced): `prevOutput->close()` (`AudioEngine.cpp:5001`) runs
  before `newPlayback->open()` for every device type; on failure
  (`:5050-5074`) the closed playback is put back, so the strip keeps its name
  but is silent and a clock loss follows. Pre-close only when the old and new
  endpoint are the same ASIO driver or either is exclusive/single-client.
  `tests/test_underrun_paths.cpp:698-701` only checks that the literal
  `prevOutput->close()` precedes `newPlayback->open(`; keep the literal inside
  the conditional.
- **Input** (Traced): `AudioEngine.cpp:4581-4598` closes before reopening, and
  `:4653-4658` records `channel->inputError` only `if (!forceReopen)`. Apply,
  Reload and auto-reload use `forceReopen = true`, so a failed reopen shows
  the device name, no error, and `getChannelHealth` reports green for a null
  input (`:6345`). Record the error on every channel sharing the device
  regardless of `forceReopen`, and pre-close only for single-client backends.

### 3.11 Other concurrency defects (fix together)

- `clockFailoverActive_` (`AudioEngine.h:1268`) is a plain `bool` written from
  the opener, the message thread and picker threads → `std::atomic<bool>`.
- `requestedOutputBufferSize_` plain `int` read by workers (`AudioEngine.cpp:5006`)
  → atomic like `requestedInputBufferSize_`.
- `currentSampleRate` read unlocked at `AudioEngine.cpp:1884`, `:2601` and in
  the `reloadDevicesAsync` lambda → read the atomic mirror.
- `tapScratch` is unsized until the first `prepareToPlayImpl` (`:585`), which
  is deferred with `callAsync` (`:5107`); size it in the constructor or guard
  with `tapScratch.getNumSamples() >= frames` (`:1071-1078`, `:1280`, `:1310`).
- `OutputStrip` device-menu callback (`MixerComponent.cpp:44-75`) captures raw
  `this` and reads `output.id` when an item is picked; use a SafePointer and a
  captured id like `ChannelStrip` does.
- `SettingsView.cpp:728` reads `channel->input` (written by the opener) with no
  lock; use the locked snapshot API.
- `StreamHealthTracker::configure` races `onBlock` on `OutputPlayback`.

### 3.12 Crash attribution and the plugin block list

- **Where:** crash handler `Main.cpp:155-178`; `isBlocked` `PluginHost.cpp:154`.
- The handler blocks whichever plugin was *loading*, even when the faulting
  thread was rendering another insert: use `crash_context::currentRenderLabel()`
  first when the fault is on the audio thread.
- Editor creation has no `ScopedPluginLoadMark` (fixed by 1.1).
- `isBlocked` compares paths case-sensitively on Windows: compare with
  `equalsIgnoreCase` or normalise.
- Add an in-app "Blocked plugins" list with Unblock in Settings (today only
  `--clear-blocked-plugins`).

### 3.13 Opt-in child-process DSP (`--child-dsp`) defects

Not default, but fix before it becomes one (see `docs/superpowers/HANDOFF.md`).
- `RestrictedHost.cpp:377` gives the plugin a 2-channel buffer although
  `instantiate()` enables the aux side-chain bus (`PluginHost.cpp:1009-1042`);
  the in-process path guards this exact crash (`:1269-1275`). Reuse the
  composite buffer or disable the aux bus in the child. (Suspected crash.)
- `PluginProcessHost::controlCall` resets `requestSeq = responseSeq` after a
  timeout (`PluginProcessHost.cpp:209-215`, `:239`), so a late reply to the old
  request satisfies the next one and the shared payload buffer is clobbered.
  Use a monotonic app-side sequence and separate request/response buffers.
- `loadChildHost(..., 20000)` runs on the message thread
  (`AudioEngine.cpp:4229`, `:1932`, `:2649`, `:2566`) despite
  `PluginHost.h:128` saying "worker thread only".

---

## 4. Audio correctness (built-in DSP and engine)

### 4.1 EQ low shelf and high shelf are swapped

- **Severity:** wrong behaviour on every shelf band. **Confidence:** Verified
  (coefficients re-derived: the code's `lowShelf` has unity gain at DC) and
  probe-confirmed (LowShelf 200 Hz +12 dB → +0.4 dB at 50 Hz, +12 dB at 12 kHz).
- **Where:** `core/include/pluginrack/DspMath.h:156-197`. The `lowShelf` body
  is the RBJ high-shelf set and vice versa. The curve UI
  (`EQCurveComponent.cpp:50`) uses the same functions, so the picture matches
  the audio but not the label, and the "Warm"/"Presence" presets
  (`EQWindow.cpp:470-480`) do the opposite of their names.
- **Fix:** swap the two `normalise(...)` argument sets. Correct RBJ low shelf:
  `b0 = A((A+1) - (A-1)c + 2√A α)`, `b1 = 2A((A-1) - (A+1)c)`,
  `b2 = A((A+1) - (A-1)c - 2√A α)`, `a0 = (A+1) + (A-1)c + 2√A α`,
  `a1 = -2((A-1) + (A+1)c)`, `a2 = (A+1) + (A-1)c - 2√A α`
  (high shelf is the mirror: swap the signs of the `(A-1)c` terms and of `b1`/`a1`).
- **Compatibility:** saved scenes store band *type*, so after the fix existing
  scenes will sound different (correct now). Mention it in the README/changelog.
  Re-check the EQ presets' gains after the swap.
- **Test:** add to `tests/test_eq_engine.cpp`: a +12 dB low shelf at 200 Hz
  boosts 50 Hz by > 10 dB and leaves 12 kHz within 1 dB; mirror for high shelf.

### 4.2 Gate attack/release are inverted and every dynamics time is smoothed twice

- **Confidence:** Verified (code) / probe-confirmed (timings).
- **Where:** `core/src/DynamicsEffect.cpp:69` smooths the detector envelope
  with attack/release, then `:93` smooths the gain again with
  `targetGain < gain_ ? attack : release`.
- **Consequences:** for the gate, *opening* (gain rising) uses the release
  time: with defaults (attack 1 ms, release 100 ms) the gate takes ~227 ms to
  reach 90 % after a word onset and ~438 ms to close. The compressor's 1 ms
  attack takes ~5 ms to get within 3 dB because of the double smoothing.
- **Fix:** for the gate, `gainRate = targetGain > gain_ ? attack : release`.
  Use a single timing stage: a fast peak detector (fixed ~0.1 ms attack, short
  release) feeding the knob-timed gain smoother. Add timing tests (gate 90 %
  open within ~2x attack, compressor within ~3x attack).

### 4.3 Every multiband knob move resets its filters and envelopes

- **Severity:** audible bursts while adjusting. **Confidence:** probe-confirmed
  (bursts to 0.735 against a steady 0.074 peak).
- **Where:** `MultibandEffect::setEffect` → `rebuild()`
  (`core/src/MultibandEffect.cpp:45-80`) re-assigns `hpStates_` and
  `envelopes_`; the app calls `setEffect` on every knob event
  (`AudioEngine.cpp:3283-3286`, setters near `:2038`).
- **Fix:** only resize state when the band/crossover count changes:
  ```cpp
  if (hpStates_.size() != xovers) hpStates_.assign(xovers, {});
  if (envelopes_.size() != bands) envelopes_.assign(bands, {});
  ```
  and otherwise only recompute coefficients.

### 4.4 Multiband allocates on the audio thread

- **Confidence:** probe-confirmed (6 allocations on the first block and on any
  larger block).
- **Where:** `MultibandEffect::prepare(double, int maxBlockFrames = 0)`
  reserves no scratch when called as `prepare(rate)`, which is how the app
  calls it (`AudioEngine.cpp:253`, `:260`, `:1717`, `:1766`, `:2277`, `:3893`);
  `reserveScratch`/`rebuild` then run inside `process()`
  (`MultibandEffect.cpp:109-114`).
- **Fix:** remove the default argument and pass the engine's max block size at
  every call site, or reserve worst-case scratch in `prepare`. Add an
  allocation-counting test (the suite has `tests/support/AllocCounter.h`).

### 4.5 The limiter does not hold its ceiling

- **Confidence:** probe-confirmed (4.0 burst, ceiling −6 dB → 0.553, 0.86 dB
  over at 100 ms release; 16 dB over at 1 ms release; 9.7 % THD on a sine).
- **Where:** `core/src/LimiterEffect.cpp:84-91`: gain snaps down when a peak
  enters the look-ahead line and starts releasing immediately, so the delayed
  peak leaves through a partly released gain.
- **Fix:** track the minimum required gain over the look-ahead window (sliding
  minimum), ramp the attack across the look-ahead, hold until the peak has left
  the delay line, then release. Add a test that no output sample exceeds the
  ceiling for bursts, sines and impulses at every release setting the UI allows.

### 4.6 EQ passes NaN/Inf through its delay line

- **Confidence:** probe-confirmed (fuzz).
- **Where:** `core/src/EqEngine.cpp:116` writes the raw sample into the delay
  ring; the live path only scrubs when an opt-in flag is on
  (`AudioEngine.cpp:915-924`).
- **Fix:** `lineL[write] = std::isfinite(L[i]) ? L[i] : 0.0f;` (and R), or
  sanitize at the top of `process`.

### 4.7 Mono inputs are not upmixed in same-device duplex mode

- **Confidence:** Traced.
- **Where:** `AudioEngine::pullDuplexInput` (`AudioEngine.cpp:1471-1482`) fills
  only channel 0 for a mono mapping, while `InputCapture::pull` upmixes
  (`DeviceStream.cpp:786-791`) and PRODUCT.md promises upmix. A mono mic on an
  ASIO duplex strip has a silent right leg; pan behaves like a level control.
- **Fix:** when `numDest == 1`, copy to channel 1 too. Fix the wrong comment at
  `AudioEngine.h:1154`. Keep the `duplexInputBuffers_[source]` literal pinned
  by `tests/test_underrun_paths.cpp:1229`.

### 4.8 The internal fallback clock runs at the wrong rate

- **Confidence:** Traced (arithmetic).
- **Where:** `InternalClock::configure` (`AudioEngine.cpp:511-516`) rounds the
  block period to whole milliseconds: 256 frames at 48 kHz (5.33 ms) runs at
  5 ms (+6.7 %), 512 frames at 11 ms (−3 %), 128 frames −11 %. Secondary
  outputs only correct ±0.5 % drift (`DeviceStream.cpp:~1245`), so they
  periodically overflow/underrun while the fallback clock runs. The thread is
  also not registered with MMCSS.
- **Fix:** tick every 1 ms against `steady_clock` with a frame accumulator and
  render whole blocks as the accumulated debt allows; call
  `realtime::registerProAudioThread()` on that thread.

### 4.9 Latency compensation errors

- Bypassed inserts are still billed their latency: `chainItemLatencySamples`
  (`AudioEngine.h:1067-1072`) and `getLatencyReport` loops
  (`AudioEngine.cpp:5912-5920`, `:5946-5954`) ignore `insert->bypassed`,
  although `processBlock` returns immediately when bypassed. Return 0 for a
  bypassed insert (as the Limiter branch does). (Traced)
- A compensation delay above 16384 samples collapses to ~0: `DelayRing::read`
  clamps to `capacity_ - 1` (`core/include/pluginrack/TapDelay.h:48`) and the
  engine does not clamp the delay (`AudioEngine.cpp:1110`, `:1305-1309`).
  Clamp to `kMaxTapDelaySamples` in the engine. (Traced)
- Plugin latency is cached once at adopt/bypass/reload; `PluginHost` never
  listens for `audioProcessorChanged` with `latencyChanged`. Subscribe and
  refresh the cache + render plan on the message thread.
- `maxBusLatency` is global (`core/src/MixerEngine.cpp:364-385`): every direct
  route is delayed by the largest bus latency even to outputs without a bus;
  channels with different chain latencies are not aligned with each other.
  Document as a limitation or compute per output.

### 4.10 Secondary-output resampler restarts its phase every callback

- **Confidence:** Traced.
- **Where:** drift loop `DeviceStream.cpp:1253-1337` restarts the fractional
  read position at 0 each callback and consumes `ceil((n-1)*r)+1` frames
  instead of `n*r`, a jump of up to ~1 sample at each block boundary; the
  "exact" path needs `|r-1| < 1e-12`. The correction is also quantised so a
  deficit up to ~40 % of target is never corrected.
- **Fix:** carry a `double readPhase` across callbacks, consume
  `floor(readPhase + n*r)` frames and keep the fractional remainder. Add a unit
  test with a pure tone across 1000 callbacks at r = 1.0003 checking for
  discontinuities.

### 4.11 Compressor and multiband makeup behaviour

- Compressor makeup ramps at the release rate and `reset()` restarts gain at
  unity (`core/src/DynamicsEffect.cpp:93`, `:34`): +12 dB makeup at 300 ms
  release takes 603 ms to reach 90 %, so output fades in after every device
  change. Smooth makeup separately and initialise `gain_` to the makeup target.
  (probe-confirmed)
- Multiband auto-makeup applies a per-sample coefficient once per block
  (`MultibandEffect.cpp:237-238`), so speed depends on block size and it never
  converges; expander bands count ~160 dB of "reduction" on silence. Use
  `rate = 1 - exp(-frames / (fs * 0.2))`, ramp across the block, exclude
  expander bands. (probe-confirmed)
- `inputGainDb` is applied unsmoothed in dynamics, multiband and limiter:
  add a one-pole smoother to avoid zipper noise on knob drags.

### 4.12 Offline `MixerEngine` diverges from the live engine

Affects `tools/render_cli` and any offline reference used in tests.
(probe-confirmed)
- Runs all built-ins before all VST inserts, ignoring chain order
  (`core/src/MixerEngine.cpp:542`, `:599`); live interleaves
  (`AudioEngine.cpp:143-206`). Give `InsertChain` a per-slot
  `processSlot(id, block)` and run it at the row position.
- Ignores EQ phase flip (`buildStrip`, `:425-427` never calls
  `eq.setPhaseFlip`) and output L/R flip (`Output::flipped`).
- A block larger than prepared leaves the tail of every output stale
  (`:697`, `:714`, `:1042`): process in `maxFrames` chunks or zero the rest.

### 4.13 Validator and codec gaps

- Accepts only 44.1/48/88.2/96 kHz and 32–8192 frames
  (`core/src/SceneValidator.cpp:68-72`, `:854`), but the app saves
  `scene.sampleRate = currentSampleRate` (`AudioEngine.cpp:3639`): a 32, 176.4
  or 192 kHz device or an ASIO buffer below 32 makes every save fail. Accept
  8000–384000 Hz and 16–16384 frames. (Traced; probe-confirmed with a
  hand-built scene)
- Multiband, de-esser, limiter and feedback-suppressor effect ids are not
  checked for zero/duplicates (`SceneValidator.cpp:116-159`) while dynamics and
  denoiser are; the codec defaults missing ids to 1. Apply the dynamics checks
  to all families.
- `toJson` uses the strict UTF-8 error handler and writes NaN/Inf as `null`
  that `fromJson` then rejects (`core/src/SceneCodec.cpp:827`); use
  `error_handler_t::replace` and reject non-finite values before writing.
- Unchecked: `AudioSettings` numerics, EQ type enum values, `colourHue`,
  very long names. Add range checks matching the UI.

### 4.14 Smaller DSP items

- Feedback suppressor: 8192+2048-point FFT and peak picking land in a single
  callback every 512 frames (p99 204–270 µs vs median 3–50 µs at -O2), and
  every instance analyses on the same hop. Stagger `framesSinceAnalysis_`
  per instance. (probe-confirmed)
- Spectral denoiser noise estimate is biased ~7 dB low (12 dB setting gives
  5.1 dB on stationary noise): bias-compensate or over-subtract. (probe-confirmed)
- `bestAsioDeviceMatch` breaks ties by first candidate
  (`core/include/pluginrack/AsioDeviceMatch.h:64`): return empty on a tie.
- Mute/solo do not silence pre-fader (Input-tap) sends
  (`core/src/MixerEngine.cpp:737-790`, mirrored live). For streamers "mute my
  mic" still reaches the stream through a pre-fader send. Decide the intended
  behaviour with the owner; at minimum show it in the UI.
- `LevelHistory::copyOrderedTo` returns the oldest samples when the
  destination is smaller than the history (`core/src/TraceData.cpp`).

---

## 5. Responsiveness: the UI must never wait on the audio thread

### 5.1 `getLatencyReport()` fails whenever the audio thread is rendering

- **Confidence:** Verified.
- **Where:** `AudioEngine::getLatencyReport` (`AudioEngine.cpp:5866-5872`)
  try-locks `stateLock` and returns a default report (`valid = false`,
  `sampleRate = 48000`) on contention, which is frequent because `render()`
  holds the lock for the whole block.
- **Consequences:** the toolbar `!`/LAT readout flickers and re-lays out
  (`MainComponent.cpp:459-498`), I/O shows "No clock output"
  (`IoView.cpp:77-84`), Settings shows "n/a" (`SettingsView.cpp:474`), and ~20
  call sites that use `.sampleRate` as the live rate get 48 kHz at 44.1/96 kHz,
  so EQ/RTA curves and thumbnails are drawn wrong (`ChannelStrip.cpp:218, 253,
  319, 456, 763`; `MixerComponent.cpp:237, 280, 355, 486`; `RackView.cpp:576,
  2269, 2326, 2445, 2531, 2679`).
- **Fix:** in the failed branch set `report.sampleRate` from the atomic rate;
  switch every "what is the rate" call site to `getCurrentSampleRate()`;
  in the UI keep the last valid report instead of blanking.

### 5.2 Message-thread setters block on `stateLock`

- **Where:** every UI setter (`setChannelGain`, `setChannelMuted`, …) starts
  with `findChannel`/`findOutput`, which take `stateLock`
  (`AudioEngine.cpp:5773-5810`); the 4 Hz `getClockDescription()` and strip
  getters do too. Each fader/knob event can wait up to one render.
- **Fix:** the message thread is the only structural mutator, so give it
  unlocked `find*` variants (assert message thread) for lookups; keep the
  locked versions for workers. Values written to the strip must already be
  atomics or be published to the render plan; check each setter.

### 5.3 Device enumeration on the UI thread every 10 s

- **Where:** `MainComponent.cpp:454-457` → `refreshDeviceAvailability`
  (`AudioEngine.cpp:5732`) → `DeviceEnumeration::enumerate`
  (`DeviceStream.cpp:376-415`), which builds fresh `AudioDeviceManager`s and
  scans every type (cache TTL 5 s < 10 s interval, so it always rescans).
- **Fix:** enumerate on a background thread and post the result; scan only
  the types the opener uses (WASAPI, ASIO). Also set `JUCE_DIRECTSOUND=0` in
  `app/CMakeLists.txt`: the menu lists DirectSound names the opener can never
  open ("No such device").

### 5.4 Every structural edit rebuilds every view, twice

- **Where:** `MainComponent.cpp:298-313`, `:1159-1165`
  (`router.refreshViews()` rebuilds Rack, Routing (channels × outputs × 4
  widgets), Patches, all I/O boards and Saves, which lists and stats the scene
  directory, `SavesView.cpp:127-135`); `mixer.rebuild()` runs in the button
  handler and again via `onStructureChanged` (`MixerComponent.cpp:1059-1071`,
  `:1391-1392`, `MainComponent.cpp:383`); `setView` rebuilds the target view
  on every switch (`MainComponent.cpp:274-282`), destroying the Mixer's
  floating FX windows when you leave and return.
- **Fix:** per-view dirty flag set by structure revision; rebuild on show only
  if dirty; remove the duplicate rebuild; Saves refreshes only on save/rename/
  delete or when shown.

### 5.5 Other per-frame costs

- `LevelMeter`: own 30 Hz timer even when hidden (`LevelMeter.cpp:100-118`),
  not opaque (each repaint also repaints the strip behind), and a new
  `ColourGradient` per bar per frame (`:211-216`). Stop timers when not
  showing, `setOpaque(true)` with a background fill, cache the gradient.
- `IoView` polls the latency report at 4 Hz while hidden (`IoView.cpp:46`, `:110`).
- `PatchesView::paintInputCard` takes `stateLock` several times per socket per
  paint (`PatchesView.cpp:258`, `:279`): snapshot once per refresh.
- Rack: multiband and de-esser cards repaint every tick even when nothing
  changed; `refreshOutputButtons` repaints the whole Rack view (`RackView.cpp:3121`).
- Mouse wheel over faders/knobs/routing sends changes values instead of
  scrolling (`ValueEditSlider.h:51-54`, `RoutingMatrixComponent.cpp:156`).
  Call `setScrollWheelEnabled(false)` in the `ValueEditSlider` constructor (or
  require Ctrl), like Settings already does for its combos.

### 5.6 Autosave and the 30 s safety net

- `checkFullSceneSafetyNet` (`MainComponent.cpp:1086-1113`) captures every
  plugin's state, serialises the whole scene and hashes it on the message
  thread every 30 s; `saveLast` serialises five times and writes five files
  (`SceneBridge.cpp:177-197`). Autosave is a 30 s debounce with no maximum age,
  so continuous editing never autosaves.
- **Fix:** capture state per insert only when its `parameterRevision` changed
  (plus a slow full pass, e.g. every 5 min, staggered one insert per tick);
  rotate autosaves by renaming instead of re-serialising; add a maximum age
  (e.g. save at least every 2 min while dirty).

---

## 6. Responsive layout

Window limits: `Main.cpp:260-261` (`setResizeLimits(1180, 700, …)`,
`centreWithSize(1600, 900)`), not clamped to the display and not persisted.

### 6.1 Racks: fixed side columns starve the editor bay

- **Where:** `RackView::resized` (`RackView.cpp:3168-3343`) and
  `RackView::paint` (`:3355-3404`) each hard-code left 160, inserts 220,
  outputs 110, touch strip 64 (+ gaps ≈ 586 px). At the 1180 px minimum the bay
  is ~575×540 px: a 1000×600 plugin is scaled to ~0.56, Ozone's EQ to ~0.45.
- **Fix:**
  1. One `Columns RackView::columns() const` helper used by both `resized()`
     and `paint()` so they cannot diverge.
  2. Breakpoints on available width W: inserts column
     `jlimit(200, 280, W * 0.16)`; below W ≈ 1400 narrow the outputs column to
     96 and the touch strip to 52.
  3. An **Expand editor** toggle in the side-chain row (outside the bay, so a
     native plugin window can never cover it) that collapses the input strip,
     touch strip and outputs column to zero width (set empty bounds; do not
     fight the existing visibility logic) so the bay gains ~330 px. Persist it
     in the UI settings.
  4. Optionally cap the fit scale (e.g. 2.0) so a 4K bay does not make a
     plugin repaint 10× the pixels; keep "fill the bay" as the default if the
     owner prefers.

### 6.2 Racks: the side-chain bar overflows narrow bays

- **Where:** `SideChainBar::resized` (`SideChainBar.cpp:330-358`) lays out
  fixed widths totalling ~670 px plus the source combo, but the bay is ~575 px
  at minimum window width, so controls overlap or are cut off whenever the
  window is below ~1350 px.
- **Fix:** shrink in priority order when space is short: gain slider 150 → 90,
  key readout 96 → 64, tap combo 96 → 76, caption 64 → 0 (hidden), source combo
  88 → 72; never let `sourceIdCombo` go below ~80 px; below that, move the
  gain/key controls to a second row (raise `kHeight` only in that mode).

### 6.3 Racks: non-scalable plugins larger than the bay draw over other columns

- **Where:** `layoutDockedEditor` clamps the editor component to the bay
  (`RackView.cpp:1999-2021`), but a plugin without content-scale support and
  `canResize() == false` keeps its native HWND at natural size; native child
  windows are not clipped by the JUCE viewport, so it paints over the inserts
  and outputs columns. (Traced against JUCE `VST3PluginWindow`.)
- **Fix:** clip the plugin's HWND to the bay with `SetWindowRgn`
  (`nativedisplay` already has the helpers) whenever the visual size exceeds
  the bay, and show a small "Plugin larger than the bay – Expand editor" hint
  linked to 6.1's toggle.

### 6.4 Minimum window size vs laptop screens

- 1180×700 is larger than common laptop work areas (1366×768 at 125 % →
  ~1093×614; 1920×1080 at 150 % → 1280×720 minus taskbar).
- **Fix:** lower the limit to ~1024×640 once 6.1/6.2/6.5 make the views fit;
  clamp the initial size to the display's user area; persist window bounds
  (`getWindowStateAsString`/`restoreWindowStateFromString`).

### 6.5 Mixer, Patches, Routing, I/O and FX windows

- Mixer reserves 3 strips (486 px) for outputs and another 486 px for buses
  regardless of count (`MixerComponent.cpp:1149-1163`); at 1180 px with a bus
  the inputs lane fits one strip. Size each panel
  `min(count * step + 8, share)` and give the remainder to inputs.
- Patches canvas height ignores the output column (`PatchesView.cpp:605-621`):
  with ~8+ outputs the lower ones are unreachable. Use
  `contentH = max(contentH, 8 + outputs.size() * kRowH + 16)`.
- Routing matrix headers scroll away with the grid (`RoutingMatrixComponent.cpp:60-130`):
  pin row/column headers outside the viewport and sync their scroll offset.
- Mixer strip input chip is 18 px wide in stereo edit mode
  (`ChannelStrip.cpp:611-620`): wrap the edit controls onto two rows.
- Rack outputs column has no scrolling (`RackView.cpp:3361-3367`): ~18+
  outputs overflow at minimum height; put the buttons in a viewport.
- FX windows: Feedback Suppressor allows 400×400 where knobs shrink to ~16 px
  (`FeedbackSuppressorWindow.cpp:385`); set sensible minimums per window and
  cascade new windows instead of stacking them on top of each other.

---

## 7. Feature completeness (make it "not MVP")

Ordered by user impact. Each is either implement or remove/relabel; do not
leave placeholders.

1. **Scene switching interrupts audio for seconds** (Traced). `commitSceneRestore`
   closes every playback and capture (`AudioEngine.cpp:4363-4389`), resets the
   clock (`:4402`) and reopens everything (`:4420`), 3–12 s of silence on slow
   USB endpoints even when the device set is identical, contrary to PRODUCT.md
   principle 3. Carry over `OutputPlayback`/`InputCapture` objects whose device
   and format match and open only the differences.
2. **Opening captures from UI actions blocks the UI** (Traced):
   `setChannelMapping`/`setChannelLayout`/`setChannelInputChannels` end in
   `reassignChannelInputDevice` → `setChannelInputDeviceImpl` inline on the
   message thread (`AudioEngine.cpp:2817-2874`); "Split into mono channels"
   (`ChannelStrip.cpp:885-912`) reopens once per physical input and can rename
   the wrong strip when `addChannel()` silently no-ops (`AudioEngine.cpp:1681`).
   Open captures at the device's full input count on first assignment, batch
   remaps on the background opener, and check the channel count after
   `addChannel()`.
3. **Snapshots placeholder**: the toolbar snapshot button shows "not
   implemented" (`MainComponent.cpp:380`, `:823-826`, plus dead members in
   `MainComponent.h:42`, `:57`, `:184`, unused `fileChooser` `:189`). Either
   implement A/B scene snapshots (in-memory `Scene` slots with recall through
   `applySceneAsync`) or remove the scaffolding.
4. **Saves page**: README says it lists autosave/last-session entries; it does
   not (`SavesView.cpp`, `SceneBridge.cpp:117-134`). Add them (read-only Load),
   an empty-state message, and make the selected row drive Load/Edit/Delete.
   "Load Scene" in the menu (`MainComponent.cpp:636`) loads default/last, not
   a chooser: rename it or add a file chooser.
5. **Save Scene As… silently overwrites** (`MainComponent.cpp:841-863`,
   `SceneBridge.cpp:161-175`): confirm on existing names like toolbar Save does.
6. **"Save*" dirty marker is cleared by autosave** (`MainComponent.cpp:1137`,
   `:1155`): track named-save-dirty separately from autosave-pending.
7. **"Add plugin folder" is not persistent** (`PluginLibrary.cpp:214`,
   `:222-228`): persist extra folders in settings and keep their cached
   plugins.
8. **First plugin scan never refreshes the open menu** (`PluginMenu.cpp:290-298`).
9. **Plugin catalogue invalidation**: rescan check uses the bundle folder's
   mtime (`PluginLibrary.cpp:300`), which in-place updates do not change; a
   failed rescan deletes the previous entry (`:310`); the walker indexes the
   inner binary of `.vst3` bundles (`:27`).
10. **Multi-class bundles**: a missing saved `uniqueId` falls back to the
    first class and applies a foreign state (`PluginHost.cpp:1086-1091`):
    refuse with an error instead.
11. **Outputs are fixed to device channels 1/2** (`DeviceStream.cpp:912`,
    `:1051`) while inputs can pick channels: add an output channel-pair picker.
12. **PRODUCT.md promises "system loopback" and "VST2 best-effort"**; neither
    exists (`JUCE_PLUGINHOST_VST=0`, no loopback capture). Implement WASAPI
    loopback capture as a device type or correct the docs.
13. **Undisclosed system change**: `DeviceStream.cpp:271-299` rewrites the
    Windows endpoint default format via PolicyConfig and never restores it.
    Disclose in the UI/README, make it opt-in, or restore on exit.
14. **Routing**: chips in the Mixer set 0 dB on / −60 dB off and lose a level
    set in Routing (`ChannelStrip.cpp:1209-1211`): remember the last non-off
    gain per cell. Linked L/R sliders are paired by display name
    (`RoutingMatrixComponent.cpp:156`, `:176`); two strips with the same name
    cross-link: pair via the stored cell sliders.
15. **Patches keeps selection by index** across rebuilds
    (`PatchesView.cpp:521-640`): store node id + kind, clear when gone.
16. **Rename dialog**: `StripDialog` is a singleton (`StripDialog.cpp:34-47`),
    names are not validated (empty/duplicate/very long,
    `AudioEngine.cpp:2795`, `:2963`), and bus strips advertise "Double-click
    to rename" while only right-click works (`MixerComponent.cpp:21`,
    `DeviceControl.h:13-22`).
17. **Effect windows**: EQ tooltip still says "visual only in v1"
    (`EQWindow.cpp:91`); Multiband/De-esser/Limiter preset combos always show
    "Default" whatever the state (`MultibandWindow.cpp:113`,
    `DeEsserWindow.cpp:68`, `LimiterWindow.cpp:24`); show "Custom" like EQ and
    Dynamics.
18. **Built-in DSP depth** (after section 4): EQ HP/LP slope choice and a real
    shelf-Q (today Q > 1 is clamped, `DspMath.h:165`, while the UI offers up to
    20); gate hold + hysteresis + side-chain filter; compressor RMS detector
    option and dry/wet; limiter true-peak (oversampled) detection; Linkwitz-
    Riley multiband crossovers (today Butterworth high-pass differences with a
    +1.8 dB bump at the crossover); multiband/de-esser trace curves derived
    from the real transfer function instead of the heuristics in
    `core/src/TraceData.cpp`; meters with RMS/LUFS; denoiser FFT size option.
19. **Keyboard shortcuts**: none exist. Add an `ApplicationCommandManager`
    with view switching (Ctrl+1…7), save (Ctrl+S), mute/solo on the selected
    strip, and Expand editor (6.1).
20. **Recovery**: an in-process plugin crash takes the session down and
    autosave may be up to 30 s+ stale (see 5.6). After 5.6, add "restore the
    session from before the crash" on next launch using the newest autosave.
21. **Docs**: PRODUCT.md says "up to six slots" (engine allows 64, validator 16
    chain rows: reconcile with 3.6); `improvement.md` cites old test counts;
    README "Saves" and "Racks" paragraphs must match whatever 6.1, 7.3 and 7.4
    end up doing.

---

## 8. Suggested order of work and acceptance

1. 2.1, 2.2 (green CI first).
2. Section 1 in order 1.1 → 1.10, measuring per 0.3 after each item.
3. Section 3 (stability), 3.1–3.5 first.
4. Section 4.1–4.6 (audible DSP bugs, each with a regression test), then the rest.
5. Section 5, then 6, then 7.

Per change: build with `tools\build-and-copy.ps1`, run the full test suite,
add or update the tests named in the item, and record manual smoke results
(or say they were not possible). Do not mark an item done without saying how
it was verified. Keep a status line under each item heading
(`**Status:** done in <commit>` / `deferred: <reason>`), the way
`improvement.md` does.
