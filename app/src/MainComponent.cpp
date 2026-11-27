#include "MainComponent.h"

#include <cmath>
#include <vector>

#include "AppLogos.h"
#include "PluginLibrary.h"
#include "SceneBridge.h"

#include <pluginrack/SceneCodec.h>

namespace {
// How long the scene must stay unchanged before the autosave writes it.
// Debounced so a busy edit session produces one write, not one every 30 s.
constexpr double kAutosaveIntervalMs = 30000.0;

// Cheap change detection: serializes the scene structure without any plugin
// state blobs. Once a second is enough to notice an edit and does not enter
// plugin code, so it cannot cause an audio dropout.
constexpr double kScenePollIntervalMs = 1000.0;

// FNV-1a over the serialized scene. The JSON embeds every plugin's state blob
// and is only built for the comparison, so keep the digest cheap.
juce::uint64 sceneFingerprint(const std::string& text) {
    juce::uint64 hash = 1469598103934665603ull;
    for (const unsigned char byte : text) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

// The toolbar logo: the mint-on-dark app icon, decoded once and shared by
// every ToolbarComponent. Binary data keeps it inside the exe, so the header
// never depends on a loose file next to it.
juce::Image appLogoImage() {
    static juce::Image cached;
    if (cached.isNull()) {
        int size = 0;
        if (const auto* data = AppLogos::logo_dark_png; data != nullptr) {
            size = AppLogos::logo_dark_pngSize;
            cached = juce::ImageFileFormat::loadFrom(data, static_cast<size_t>(size));
        }
    }
    return cached;
}
} // namespace

ToolbarComponent::ToolbarComponent() {
    logoImage = appLogoImage();
    for (auto* b : { &racksButton, &mixerButton, &patchesButton, &routingButton, &ioButton,
                     &savesButton, &settingsButton }) {
        b->setClickingTogglesState(true);
        theme::ui::styleButton(*b);
    }

    racksButton.onClick = [this] { selectView(AppView::Rack); };
    mixerButton.onClick = [this] { selectView(AppView::Mixer); };
    patchesButton.onClick = [this] { selectView(AppView::Patches); };
    routingButton.onClick = [this] { selectView(AppView::Routing); };
    ioButton.onClick = [this] { selectView(AppView::IO); };
    savesButton.onClick = [this] { selectView(AppView::Saves); };
    settingsButton.onClick = [this] { selectView(AppView::Settings); };

    racksButton.setTooltip("Rack view: plugin chains");
    mixerButton.setTooltip("Mixer view: faders, meters, inline routing");
    patchesButton.setTooltip("Patches view: inputs to outputs map");
    routingButton.setTooltip("Routing view: full channel/output matrix");
    ioButton.setTooltip("I/O view: input and output devices");
    savesButton.setTooltip("Saves view: saved scenes");
    settingsButton.setTooltip("Settings view: buffer, latency tuning, diagnostics");

    snapshotButton.setTooltip("Snapshots arrive in a later task");
    snapshotButton.onClick = [this] { if (onSnapshot) onSnapshot(); };

    latencyLabel.setJustificationType(juce::Justification::centredRight);
    latencyLabel.setFont(theme::ui::captionFont());
    latencyLabel.setColour(juce::Label::textColourId, theme::dim());

    errorButton.setTooltip("Audio errors - click for details");
    errorButton.setVisible(false);
    errorButton.onClick = [this] { if (onErrorClicked) onErrorClicked(); };

    for (auto* b : { &saveButton, &snapshotButton, &menuButton })
        theme::ui::styleButton(*b);
    theme::ui::styleButton(errorButton);
    errorButton.setColour(juce::TextButton::textColourOffId, theme::red());

    saveButton.onClick = [this] { if (onSave) onSave(); };
    menuButton.onClick = [this] { if (onMenu) onMenu(); };

    addAndMakeVisible(racksButton);
    addAndMakeVisible(mixerButton);
    addAndMakeVisible(patchesButton);
    addAndMakeVisible(ioButton);
    addAndMakeVisible(routingButton);
    addAndMakeVisible(savesButton);
    addAndMakeVisible(settingsButton);
    addAndMakeVisible(snapshotButton);
    addAndMakeVisible(latencyLabel);
    addAndMakeVisible(errorButton);
    addAndMakeVisible(saveButton);
    addAndMakeVisible(menuButton);

    updateNavToggles();
}

void ToolbarComponent::selectView(AppView view) {
    setView(view);
    if (onViewSelected) onViewSelected(view);
}

void ToolbarComponent::setView(AppView view) {
    currentView = view;
    updateNavToggles();
}

void ToolbarComponent::updateNavToggles() {
    racksButton.setToggleState(currentView == AppView::Rack, juce::dontSendNotification);
    mixerButton.setToggleState(currentView == AppView::Mixer, juce::dontSendNotification);
    patchesButton.setToggleState(currentView == AppView::Patches, juce::dontSendNotification);
    routingButton.setToggleState(currentView == AppView::Routing, juce::dontSendNotification);
    ioButton.setToggleState(currentView == AppView::IO, juce::dontSendNotification);
    savesButton.setToggleState(currentView == AppView::Saves, juce::dontSendNotification);
    settingsButton.setToggleState(currentView == AppView::Settings, juce::dontSendNotification);
}

void ToolbarComponent::setClockText(const juce::String& text, bool isWarning) {
    clockText = text;
    clockColour = isWarning ? theme::amber() : theme::dim();
    repaint();
}

void ToolbarComponent::setLatencyText(const juce::String& text, juce::Colour colour, const juce::String& tooltip) {
    latencyLabel.setText(text, juce::dontSendNotification);
    latencyLabel.setColour(juce::Label::textColourId, colour);
    latencyLabel.setTooltip(tooltip);
}

void ToolbarComponent::setHasError(bool hasError) {
    if (errorButton.isVisible() == hasError) return;
    errorButton.setVisible(hasError);
    resized();
}

void ToolbarComponent::paint(juce::Graphics& g) {
    g.fillAll(theme::panel());
    g.setColour(theme::border());
    g.drawHorizontalLine(getHeight() - 1, 0.0f, (float)getWidth());

    // App logo left of the wordmark: the mint-on-dark icon at 32 px, centred
    // in the 190 px wordmark lane the layout reserves below.
    if (!logoImage.isNull()) {
        constexpr int kLogoSize = 32;
        const int logoY = (getHeight() - kLogoSize) / 2;
        g.drawImageWithin(logoImage, 8, logoY, kLogoSize, kLogoSize,
                          juce::RectanglePlacement::centred | juce::RectanglePlacement::onlyReduceInSize,
                          false);
    }

    g.setColour(theme::green());
    g.setFont(juce::Font(juce::FontOptions(22.0f, juce::Font::bold)));
    g.drawText("PluginRack", 46, 0, 152, getHeight(), juce::Justification::centredLeft);

    // clock info (single line, clipped - same as the old header paint)
    g.setColour(clockColour);
    g.setFont(theme::ui::captionFont());
    g.drawFittedText(clockText, clockBounds, juce::Justification::centredRight, 1);
}

void ToolbarComponent::resized() {
    auto area = getLocalBounds().reduced(8, 12);
    area.removeFromLeft(190); // painted wordmark space

    racksButton.setBounds(area.removeFromLeft(64));
    area.removeFromLeft(4);
    mixerButton.setBounds(area.removeFromLeft(64));
    area.removeFromLeft(4);
    patchesButton.setBounds(area.removeFromLeft(76));
    area.removeFromLeft(4);
    ioButton.setBounds(area.removeFromLeft(48));
    area.removeFromLeft(4);
    routingButton.setBounds(area.removeFromLeft(64));
    area.removeFromLeft(4);
    savesButton.setBounds(area.removeFromLeft(64));
    area.removeFromLeft(4);
    settingsButton.setBounds(area.removeFromLeft(64));

    menuButton.setBounds(area.removeFromRight(60));
    area.removeFromRight(8);
    saveButton.setBounds(area.removeFromRight(50));
    area.removeFromRight(8);
    // Snapshot stays hidden until the A/B feature lands: a dead button that
    // pops a modal steals 84 px from the clock readout.
    snapshotButton.setVisible(false);
    if (errorButton.isVisible()) {
        errorButton.setBounds(area.removeFromRight(28));
        area.removeFromRight(8);
    }
    latencyLabel.setBounds(area.removeFromRight(128));
    clockBounds = area;
}

ViewRouter::ViewRouter(AudioEngine& engine, MixerComponent& mixerViewIn)
    : audioEngine(engine)
    , mixerView(mixerViewIn)
    , rackView(audioEngine)
    , routingView(audioEngine)
    , patchesView(audioEngine)
    , ioView(audioEngine)
    , savesView([this](const pluginrack::Scene& scene,
                       std::function<void(const juce::String&)> finished) {
          return onRestoreRequested
                     ? onRestoreRequested(scene, std::move(finished))
                     : juce::String("Scene restore is unavailable.");
      },
                [this](const juce::String& name) {
                    if (onSaveLoaded) onSaveLoaded(name);
                },
                [this](const juce::String& oldName, const juce::String& newName) {
                    if (onSaveIdentityChanged) onSaveIdentityChanged(oldName, newName);
                })
    , settingsView(audioEngine) {
    rackView.onJumpToMixer = [this] { setView(AppView::Mixer); };
    rackView.onStructureChanged = [this] {
        if (onStructureChanged) onStructureChanged();
    };
    // Output/Sub strips erase inserts the rack may have docked, and they
    // cannot reach the rack directly. The InsertHost remove button fires
    // this release-only hook synchronously before the engine frees the
    // processor; a full refreshViews here would re-dock the doomed insert
    // before the erase, so this releases without rebuilding.
    pluginRemovingHook() = [this] { releaseDockedEditor(); };
    ioView.onStructureChanged = [this] {
        // Strips added, removed or renamed on the I/O page change every view.
        if (onStructureChanged) onStructureChanged();
    };
    ioView.onSelectionChanged = [this](int id, bool isOutput) {
        rackView.setSelectedChannel(id, isOutput);
    };
    ioView.onDoubleClicked = [this](int id, bool isOutput) {
        rackView.setSelectedChannel(id, isOutput);
        setView(AppView::Rack);
    };
    mixerView.onSelectionChanged = [this](int id, bool isOutput) {
        rackView.setSelectedChannel(id, isOutput);
    };
    mixerView.onDoubleClicked = [this](int id, bool isOutput) {
        // Channels and outputs both open on their rack page.
        rackView.setSelectedChannel(id, isOutput);
        setView(AppView::Rack);
    };
    addAndMakeVisible(mixerView);
    addAndMakeVisible(rackView);
    addAndMakeVisible(routingView);
    addAndMakeVisible(patchesView);
    addAndMakeVisible(ioView);
    addAndMakeVisible(savesView);
    addAndMakeVisible(settingsView);
    refreshViews();
    setView(AppView::Rack);
}

void ViewRouter::setView(AppView view) {
    currentView = view;
    if (view == AppView::Rack) rackView.rebuild();
    if (view == AppView::Mixer) mixerView.rebuild();
    if (view == AppView::Routing) routingView.rebuild();
    if (view == AppView::Patches) patchesView.rebuild();
    if (view == AppView::IO) ioView.rebuild();
    if (view == AppView::Saves) savesView.rebuild();
    if (view == AppView::Settings) settingsView.refresh();
    mixerView.setVisible(view == AppView::Mixer);
    rackView.setVisible(view == AppView::Rack);
    routingView.setVisible(view == AppView::Routing);
    patchesView.setVisible(view == AppView::Patches);
    ioView.setVisible(view == AppView::IO);
    savesView.setVisible(view == AppView::Saves);
    settingsView.setVisible(view == AppView::Settings);

    // Every path that changes view must reach the toolbar, not just toolbar
    // clicks: double-clicking a strip jumps to the rack from inside a view,
    // and a stale toggle leaves the old tab lit. Guarded because the router
    // sets its initial view before the host wires this up.
    if (onViewChanged) onViewChanged(view);
}

void ViewRouter::refreshViews() {
    // Only orphaned editors are released here: with the insert still in the
    // engine, rackView.rebuild() reuses the live editor instead of destroying
    // and re-creating a plugin UI (some plugins hang on that teardown).
    releaseOrphanedEditor();
    rackView.rebuild();
    // Strips re-derive their identity colour and device state without being
    // rebuilt, so a rename made anywhere (rack, mixer, scene load) reaches the
    // fader instead of leaving it in its construction-time colour.
    mixerView.refreshStrips();
    routingView.rebuild();
    patchesView.rebuild();
    ioView.rebuild();
    savesView.rebuild();
    settingsView.refresh();
}

void ViewRouter::refreshDeviceStrips() {
    mixerView.refreshStrips();
    ioView.refreshDeviceStrips();
}

void ViewRouter::resized() {
    mixerView.setBounds(getLocalBounds());
    rackView.setBounds(getLocalBounds());
    routingView.setBounds(getLocalBounds());
    patchesView.setBounds(getLocalBounds());
    ioView.setBounds(getLocalBounds());
    savesView.setBounds(getLocalBounds());
    settingsView.setBounds(getLocalBounds());
}

MainComponent::MainComponent()
    : mixer(audioEngine), router(audioEngine, mixer) {
    const auto args = juce::JUCEApplication::getCommandLineParameterArray();
    if (args.contains("--child-dsp")) {
        audioEngine.setChildProcessDsp(true);
        juce::Logger::writeToLog("plugin: child DSP enabled via --child-dsp (beta)");
    }
    addAndMakeVisible(toolbar);
    addAndMakeVisible(router);

    toolbar.setView(AppView::Rack);
    toolbar.onViewSelected = [this](AppView view) { router.setView(view); };
    router.onViewChanged = [this](AppView view) { toolbar.setView(view); };
    router.onStructureChanged = [this] { refreshViews(); };
    router.onRestoreRequested = [this](const pluginrack::Scene& scene,
                                       std::function<void(const juce::String&)> finished) {
        return applySceneAsync(scene, true, std::move(finished));
    };
    router.onSaveLoaded = [this](const juce::String& name) { activeSaveName_ = name; };
    router.onSaveIdentityChanged = [this](const juce::String& oldName,
                                          const juce::String& newName) {
        if (activeSaveName_.equalsIgnoreCase(oldName)) activeSaveName_ = newName;
    };
    toolbar.onSave = [this] {
        // Stay where the user is: overwrite the active save, or ask for a
        // name when nothing is active yet. No teleport to the Saves tab.
        if (refuseSaveWhileRestoring()) return;
        if (activeSaveName_.isNotEmpty()) {
            const auto error = SceneBridge::saveNamed(audioEngine.makeScene(), activeSaveName_);
            if (error.isNotEmpty()) showError(error);
            else router.refreshViews();
        } else {
            saveSceneAs();
        }
    };
    toolbar.onMenu = [this] { showMenu(); };
    toolbar.onSnapshot = [this] { showSnapshotPlaceholder(); };
    toolbar.onErrorClicked = [this] { showAudioErrors(); };

    mixer.onStructureChanged = [this] { refreshViews(); };
    // A strip-local chain edit or rename: the strip has already refreshed
    // itself, so refresh the views that mirror engine state without rebuilding
    // the mixer board. Rebuilding it would close every strip's floating effect
    // window and drop scroll/focus for the strips the user did not touch.
    mixer.onInsertsChanged = [this] { router.refreshViews(); };
    // The rack applies chain/insert edits to its own rows, so a card add,
    // remove or drag must not rebuild the mixer board and close the other
    // strips' floating windows. Route those through the narrow notification.
    router.getRackView().onInsertsChanged = [this] { router.refreshViews(); };

    // All device/plugin work is deferred to the first paint (see paint()):
    // starting it from the constructor ran it before the window had drawn,
    // which is what made startup look like a hang.
    startTimerHz(4);
    const auto nowMs = juce::Time::getMillisecondCounterHiRes();
    nextScenePollMs_ = nowMs + kScenePollIntervalMs;
    nextFullSceneCheckMs_ = nowMs + kAutosaveIntervalMs;
    timerCallback(); // populate the toolbar readouts before the first tick
    setSize(1600, 900);
}

void MainComponent::timerCallback() {
    audioEngine.pollLatencyMeasurement();
    if (audioEngine.isSceneRestoreActive()) {
        // A staged restore instantiates one plugin per message-loop turn; the
        // clock line reports it so a slow plugin reads as progress, not a hang.
        toolbar.setClockText("Loading scene... "
                                 + juce::String(audioEngine.getSceneRestoreDone()) + "/"
                                 + juce::String(audioEngine.getSceneRestoreTotal()),
                             true);
        return;
    }
    if (audioEngine.isOpeningDevices()) {
        const auto total = audioEngine.getDeviceOpenTotal();
        const auto done = audioEngine.getDeviceOpenProgress();
        const auto status = audioEngine.getDeviceOpenStatus();
        juce::String text = "Starting audio... " + juce::String(done) + "/"
                          + juce::String(total);
        if (status.isNotEmpty()) text += "  " + status;
        toolbar.setClockText(text, true);
    } else {
        if (audioEngine.consumeDeviceOpenFinished()) {
            const auto deviceErrors = audioEngine.takeDeviceOpenErrors();
            // The strips were built before the background open finished, so
            // their device readouts still show the pre-open state. Refresh them
            // now: a device that no longer exists has to turn red, not stay
            // green.
            router.refreshDeviceStrips();
            if (deviceErrors.isNotEmpty())
                showError("Some audio devices could not be opened:\n" + deviceErrors);
        }
        // The clock stream can stop between the slower name scans (unplugged,
        // disabled, or closed by the OS). Check its heartbeat every tick so
        // the other outputs hand over to a promoted output or the internal
        // clock instead of going silent for up to ten seconds.
        if (audioEngine.checkClockHealth()) router.refreshDeviceStrips();
        // The preferred first output takes the clock back after its endpoint
        // returns (promoted output) or is reopened (internal-clock fallback).
        if (audioEngine.maybeReclaimClock()) router.refreshDeviceStrips();
        // With no clock output the strips already say so on their own device
        // rows; repeating it as a warning in the header only adds noise over
        // the output strips. The Menu keeps the full clock line.
        const auto clockDesc = audioEngine.getClockDescription();
        const bool noClock = clockDesc.startsWith("No output");
        toolbar.setClockText(noClock ? juce::String() : clockDesc, false);
    }

    // A device can vanish while its stream is open (unplugged, disabled in
    // Windows) and the strips would keep showing it as healthy. Check a few
    // times a minute and flip the affected strips to red.
    if (!audioEngine.isOpeningDevices() && ++availabilityTicks_ >= 40) {
        availabilityTicks_ = 0;
        if (audioEngine.refreshDeviceAvailability()) router.refreshDeviceStrips();
    }

    const auto report = audioEngine.getLatencyReport();
    if (!report.valid) {
        smoothedLatencyMs = -1.0;
        toolbar.setLatencyText({}, theme::dim(), {});
    } else {
        // A live glitch counter overrides smoothing: snap to the raw total so
        // the EMA never hides an active underrun/overrun.
        const bool glitch = (report.underruns + report.overruns) > 0;
        if (glitch || smoothedLatencyMs < 0.0) smoothedLatencyMs = report.totalMs;
        else smoothedLatencyMs += 0.25 * (report.totalMs - smoothedLatencyMs);
        const juce::String estimateTip = "Estimated (approx.) input to output: input " +
                            juce::String((int)std::round(report.inputDeviceMs)) + " ms (buffer " +
                            juce::String(report.inputBufferSize) + " smp) + plugins " +
                            juce::String((int)std::round(report.pluginMs)) + " ms + output " +
                            juce::String((int)std::round(report.outputDeviceMs)) + " ms (buffer " +
                            juce::String(report.outputBufferSize) + " smp) + output FIFO " +
                            juce::String((int)std::round(report.fifoMs)) + " ms" +
                            (glitch ? " - dropouts detected" : "");
        juce::String latText;
        juce::String tip;
        if (report.measuredValid) {
            latText = "LAT " + juce::String((int)std::round(report.measuredMs)) + " ms";
            tip = "Measured round-trip: " + juce::String((int)std::round(report.measuredMs))
                + " ms (" + estimateTip + ")";
        } else {
            latText = "LAT ~" + juce::String((int)std::round(smoothedLatencyMs)) + " ms (est.)";
            tip = estimateTip;
        }
        if (glitch) latText += " !";
        juce::Colour colour = theme::dim();
        if (glitch) colour = theme::red();
        else if (smoothedLatencyMs > 60.0) colour = theme::red();
        else if (smoothedLatencyMs > 30.0) colour = theme::amber();
        toolbar.setLatencyText(latText, colour, tip);
    }

    bool hasError = !report.valid;
    if (!hasError && (report.underruns + report.overruns) > 0) hasError = true;
    if (!hasError) hasError = !audioEngine.getDeviceErrors().empty();
    toolbar.setHasError(hasError);

    // Autosave is a debounce, not a repeating snapshot. The cheap poll runs at
    // 1 Hz and only notices edits; the scene is captured and written once,
    // kAutosaveIntervalMs after the last one. The slow full-scene check is a
    // safety net for plugins that change state without a parameter callback.
    // Time-based rather than tick-based so the early returns above cannot make
    // the interval drift.
    pollSceneChanges();
    const auto nowMs = juce::Time::getMillisecondCounterHiRes();
    if (nowMs >= nextFullSceneCheckMs_) {
        nextFullSceneCheckMs_ = nowMs + kAutosaveIntervalMs;
        checkFullSceneSafetyNet();
    }
    if (sceneChangedSinceSave_ && nowMs - lastSceneChangeMs_ >= kAutosaveIntervalMs)
        autosaveIfChanged();
}

void MainComponent::selectView(AppView view) {
    toolbar.setView(view);
    router.setView(view);
}

const std::vector<std::unique_ptr<AudioEngine::Channel>>& MainComponent::getChannelsForDiagnostics() {
    return audioEngine.getChannels();
}

juce::String MainComponent::loadSaveForDiagnostics(const juce::String& name) {
    juce::String error;
    auto saved = SceneBridge::loadNamed(name, error);
    if (error.isNotEmpty()) return error;
    if (!saved.has_value()) return "Save not found: " + name;
    return applyScene(*saved);
}

void MainComponent::feedDiagnosticsSignal(int /*numBins*/, double sampleRateHz) {
    // Headless screenshot: no audio engine runs, so the analyzer taps stay
    // silent and one timer frame of zeros floors every RTA to -60 dB.
    // Synthesize a broadband-ish spectrum: pink-noise magnitude with a
    // gentle sweep, written straight into spectrumAvg-equivalent level
    // (the post ring the Multiband/De-esser windows snapshot). One frame
    // of zeros floored it before; this gives a real trace for the capture.
    const auto& channels = audioEngine.getChannels();
    if (channels.empty()) return;
    auto* channel = channels.front().get();
    // Write the legacy strip tap AND every per-plugin tap, so the editor
    // and the mini previews — which now read their own taps — all show the
    // same synthetic spectrum in the capture.
    constexpr int n = 8192;
    auto fillRing = [&](juce::AudioBuffer<float>& ring) {
        for (int i = 0; i < n; ++i) {
            float s = 0.0f;
            for (int b = 0; b < 16; ++b) {
                const double f = 80.0 * std::pow(150.0, b / 15.0); // 80Hz..12kHz log
                const double level = 0.18 / std::sqrt(1.0 + b * 0.8)
                                   * (0.8 + 0.2 * std::sin(i * 0.0007 + b * 1.3));
                s += static_cast<float>(level * std::sin(
                    2.0 * juce::MathConstants<double>::pi * f * i / sampleRateHz + b));
            }
            ring.setSample(0, i, s * 0.35f);
            ring.setSample(1, i, s * 0.35f);
        }
    };
    for (auto* tap : {&channel->analyzerTap, &channel->eqTap, &channel->dynamicsTap,
                      &channel->multibandTap, &channel->deEsserTap, &channel->limiterTap}) {
        fillRing(tap->pre);
        fillRing(tap->post);
        tap->preWrite.set(0);
        tap->postWrite.set(0);
        tap->setMode(AudioEngine::AnalyzerTap::kPost);
    }
}

juce::Component* MainComponent::focusDiagnosticsEffect(pluginrack::ChainItemType type) {
    router.getRackView().setSelectedChannel(1, false);
    if (type == pluginrack::ChainItemType::SpectralDenoiser
        || type == pluginrack::ChainItemType::FeedbackSuppressor) {
        router.getRackView().focusDiagnosticsWindow(type);
        feedDiagnosticsSignal(4097, 48000.0);
        return router.getRackView().getFocusedDiagnosticsEffect();
    }
    const auto& channels = audioEngine.getChannels();
    std::vector<pluginrack::ChainItem> chain;
    if (!channels.empty()) {
        auto* channel = channels.front().get();
        if (type == pluginrack::ChainItemType::Multiband) {
            for (auto& e : channel->multibandEffects)
                if (!e.bypassed) chain.push_back({type, e.id});
        } else if (type == pluginrack::ChainItemType::DeEsser) {
            for (auto& e : channel->deEsserEffects)
                if (!e.bypassed) chain.push_back({type, e.id});
        } else if (type == pluginrack::ChainItemType::Limiter) {
            for (auto& e : channel->limiterEffects)
                if (!e.bypassed) chain.push_back({type, e.id});
        }
        if (!chain.empty()) {
            audioEngine.setChannelChain(channel->id, chain);
        }
    }
    router.getRackView().activateBuiltIn({type, !chain.empty() ? chain.front().id : 1});
    // focusMultiband/focusDeEsser already call rebuild(), so the mini cards
    // exist — but feed them the synthetic signal so their previews also
    // show a trace instead of an empty pane. Then repaint both surfaces.
    feedDiagnosticsSignal(4097, 48000.0);
    return router.getRackView().getFocusedDiagnosticsEffect();
}

void MainComponent::paint(juce::Graphics& g) {
    g.fillAll(theme::bg());
    // Queue the audio startup only once the first frame has been drawn: a
    // restore running from the constructor paints nothing until it finishes.
    if (!audioInitQueued_) {
        audioInitQueued_ = true;
        juce::Component::SafePointer<MainComponent> safe(this);
        juce::MessageManager::callAsync([safe] {
            if (safe != nullptr) safe->initialiseAudio();
        });
    }
    // The plugin catalogue warms the Add-plugin menu: cached results are shown
    // at once and only changed bundles are scanned, so this stays in the
    // background even on a machine with a large collection.
    if (!pluginScanQueued_) {
        pluginScanQueued_ = true;
        juce::MessageManager::callAsync(
            [] { PluginLibrary::getInstance().beginBackgroundScan(); });
    }
}

void MainComponent::resized() {
    auto area = getLocalBounds();
    toolbar.setBounds(area.removeFromTop(56));
    router.setBounds(area);
}

void MainComponent::showMenu() {
    juce::PopupMenu menu;
    menu.addItem("Save Scene As...", [this] { saveSceneAs(); });
    menu.addItem("Load Scene", [this] { loadScene(true); });
    menu.addItem("Open Scenes Folder", [this] {
        const auto dir = SceneBridge::getSceneFile().getParentDirectory();
        dir.createDirectory();
        dir.startAsProcess();
    });
    menu.addSeparator();
    menu.addItem("Reset to Defaults", [this] {
        // A staged restore owns the live strips until its commit; mutating
        // them now would be thrown away, and saveScene would refuse anyway.
        if (refuseSaveWhileRestoring()) return;
        if (audioEngine.isOpeningDevices()) {
            showError("Audio is still starting up. Please try again in a moment.");
            return;
        }
        juce::AlertWindow::showOkCancelBox(juce::MessageBoxIconType::QuestionIcon,
                                            "PluginRack",
                                            "Reset engine to defaults (1 channel, 3 outputs)?\n"
                                            "Strip names, device assignments and watched streams are kept.",
                                            "Reset", "Cancel", nullptr,
                                            juce::ModalCallbackFunction::create([this](int result) {
                                                if (result != 1) return;
                                                // The rack may have docked an editor owned by a strip
                                                // removed below: release it while the processors are
                                                // still alive. Release-only on purpose (see InsertHost.h).
                                                if (auto release = pluginRemovingHook()) release();
                                                // Mixer strips hold Channel&/Output& refs and their open
                                                // EQ windows call back into those refs on destruction
                                                // (analyzerTap): close them while the engine strips are
                                                // still alive, before any removeChannel/removeOutput frees
                                                // the refs the pending rebuild would otherwise tear down.
                                                mixer.closeAllEqWindows();
                                                std::vector<std::uint32_t> chIds;
                                                for (const auto& c : audioEngine.getChannels()) chIds.push_back(c->id);
                                                for (std::size_t i = 1; i < chIds.size(); ++i)
                                                    (void)audioEngine.removeChannel(chIds[i]);
                                                std::vector<std::uint32_t> outIds;
                                                for (const auto& o : audioEngine.getOutputs()) outIds.push_back(o->id);
                                                for (std::size_t i = 3; i < outIds.size(); ++i)
                                                    audioEngine.removeOutput(outIds[i]);
                                                while (audioEngine.getChannels().size() < 1) audioEngine.addChannel();
                                                while (audioEngine.getOutputs().size() < 3) audioEngine.addOutput();
                                                // Truncate keeps the first channel + first 3 outputs, so
                                                // their inserts survive the reset: clear inserts and
                                                // restore default bypassed EQ, then reset the survivors
                                                // themselves to defaults (gains, trim, pan, mutes, solos,
                                                // layouts, routes). Names and devices are kept.
                                                for (const auto& c : audioEngine.getChannels()) {
                                                    std::vector<std::uint32_t> insertIds;
                                                    for (const auto& insert : c->inserts)
                                                        insertIds.push_back(insert->id);
                                                    for (const auto insertId : insertIds)
                                                        audioEngine.removePlugin(c->id, insertId);
                                                    audioEngine.setChannelEq(c->id, pluginrack::defaultEqBands());
                                                    audioEngine.setChannelEqBypassed(c->id, true);
                                                    audioEngine.setChannelEqDelay(c->id, 0.0f);
                                                    audioEngine.setChannelEqRtaOffset(c->id, 0.0f);
                                                    audioEngine.setChannelEqRtaWeighting(c->id, 0);
                                                    audioEngine.setChannelMultibandRtaOffset(c->id, 0.0f);
                                                    audioEngine.setChannelMultibandRtaWeighting(c->id, 0);
                                                    audioEngine.setChannelDeEsserRtaOffset(c->id, 0.0f);
                                                    audioEngine.setChannelDeEsserRtaWeighting(c->id, 0);
                                                    audioEngine.setChannelFeedbackSuppressorRtaOffset(c->id, 0.0f);
                                                    audioEngine.setChannelFeedbackSuppressorRtaWeighting(c->id, 0);
                                                    audioEngine.setChannelEffects(c->id, pluginrack::defaultDynamicsEffects());
                                                    audioEngine.setChannelMultiband(c->id, pluginrack::defaultMultibandEffects());
                                                    audioEngine.setChannelDeEssers(c->id, pluginrack::defaultDeEsserEffects());
                                                    audioEngine.setChannelLimiters(c->id, pluginrack::defaultLimiterEffects());
audioEngine.setChannelFeedbackSuppressors(c->id, pluginrack::defaultFeedbackSuppressorEffects());
                                                    audioEngine.setChannelSpectralDenoisers(c->id, {});
                                                    audioEngine.setChannelChain(c->id, AudioEngine::defaultChain());
                                                    audioEngine.setChannelGain(c->id, 0.0f);
                                                    audioEngine.setChannelTrim(c->id, 0.0f);
                                                    audioEngine.setChannelPan(c->id, 0.0f);
                                                    audioEngine.setChannelMuted(c->id, false);
                                                    audioEngine.setChannelSoloed(c->id, false);
                                                    audioEngine.setChannelMapping(c->id, pluginrack::ChannelLayout::Stereo, {0, 1});
                                                }
                                                for (std::size_t i = 0; i < audioEngine.getOutputs().size(); ++i) {
                                                    const auto outputId = audioEngine.getOutputs()[i]->id;
                                                    std::vector<std::uint32_t> insertIds;
                                                    for (const auto& insert : audioEngine.getOutputs()[i]->inserts)
                                                        insertIds.push_back(insert->id);
                                                    for (const auto insertId : insertIds)
                                                        audioEngine.removeOutputPlugin(outputId, insertId);
                                                    audioEngine.setOutputEq(outputId, pluginrack::defaultEqBands());
                                                    audioEngine.setOutputEqBypassed(outputId, true);
                                                    audioEngine.setOutputEqDelay(outputId, 0.0f);
                                                    audioEngine.setOutputEqRtaOffset(outputId, 0.0f);
                                                    audioEngine.setOutputEqRtaWeighting(outputId, 0);
                                                    audioEngine.setOutputMultibandRtaOffset(outputId, 0.0f);
                                                    audioEngine.setOutputMultibandRtaWeighting(outputId, 0);
                                                    audioEngine.setOutputDeEsserRtaOffset(outputId, 0.0f);
                                                    audioEngine.setOutputDeEsserRtaWeighting(outputId, 0);
                                                    audioEngine.setOutputFeedbackSuppressorRtaOffset(outputId, 0.0f);
                                                    audioEngine.setOutputFeedbackSuppressorRtaWeighting(outputId, 0);
                                                    audioEngine.setOutputEffects(outputId, pluginrack::defaultDynamicsEffects());
                                                    audioEngine.setOutputMultiband(outputId, pluginrack::defaultMultibandEffects());
                                                    audioEngine.setOutputDeEssers(outputId, pluginrack::defaultDeEsserEffects());
                                                    audioEngine.setOutputLimiters(outputId, pluginrack::defaultLimiterEffects());
audioEngine.setOutputFeedbackSuppressors(outputId, pluginrack::defaultFeedbackSuppressorEffects());
                                                    audioEngine.setOutputSpectralDenoisers(outputId, {});
                                                    audioEngine.setOutputChain(outputId, AudioEngine::defaultChain());
                                                    audioEngine.setOutputGain(outputId, 0.0f);
                                                    audioEngine.setOutputMuted(outputId, false);
                                                    audioEngine.setOutputLayout(outputId, pluginrack::ChannelLayout::Stereo);
                                                    audioEngine.setOutputFlipped(outputId, false);
                                                }
                                                // Default routing: the channel is hot (0 dB) to the first
                                                // output only, silent (-60 dB) elsewhere, so no stale Sub
                                                // send survives the reset.
                                                if (!audioEngine.getChannels().empty() && !audioEngine.getOutputs().empty()) {
                                                    const auto firstOutId = audioEngine.getOutputs().front()->id;
                                                    for (const auto& c : audioEngine.getChannels()) {
                                                        for (const auto& o : audioEngine.getOutputs()) {
                                                            audioEngine.setSendGain(c->id, o->id,
                                                                o->id == firstOutId ? 0.0f : -60.0f);
                                                        }
                                                    }
                                                }
                                                refreshViews();
                                                saveScene();
                                            }));
    });
    menu.addSeparator();
    menu.addSectionHeader("Audio");
    menu.addItem(audioEngine.getClockDescription(), false, false, [] {});
    menu.addSectionHeader("Latency (est.)");
    const auto report = audioEngine.getLatencyReport();
    if (!report.valid) {
        menu.addItem("No output device", false, false, [] {});
    } else {
        const auto ms = [](double v) { return juce::String((int)std::round(v)) + " ms"; };
        menu.addItem("Total ~" + ms(report.totalMs), false, false, [] {});
        menu.addItem("Input device " + ms(report.inputDeviceMs) + " (buffer "
                         + juce::String(report.inputBufferSize) + " smp)",
                     false, false, [] {});
        juce::String pluginText = "Plugins " + ms(report.pluginMs);
        if (report.maxInsertId != 0) {
            pluginText += " (max " + report.maxInsertName + " " + ms(report.maxInsertMs) + ")";
        }
        menu.addItem(pluginText, false, false, [] {});
        menu.addItem("Output device " + ms(report.outputDeviceMs) + " (buffer "
                         + juce::String(report.outputBufferSize) + " smp)",
                     false, false, [] {});
        menu.addItem("Output FIFO " + ms(report.fifoMs), false, false, [] {});
        if (report.measuredValid) {
            menu.addItem("Measured round-trip " + ms(report.measuredMs), false, false, [] {});
        } else if (report.measureFailed) {
            menu.addItem("Measurement failed", false, false, [] {});
        }
        if (report.underruns > 0 || report.overruns > 0) {
            menu.addItem("Glitches: " + juce::String((juce::int64)report.overruns)
                             + " overruns / " + juce::String((juce::int64)report.underruns)
                             + " underruns",
                         false, false, [] {});
        }
    }

    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(toolbar.getMenuTarget()));
}

void MainComponent::showAudioErrors() {
    const auto report = audioEngine.getLatencyReport();
    juce::StringArray lines;
    if (!report.valid) {
        lines.add("No output device open.");
    } else if ((report.underruns + report.overruns) > 0) {
        lines.add("Glitches: " + juce::String((juce::int64)report.overruns) + " overruns / "
                  + juce::String((juce::int64)report.underruns) + " underruns.");
    }
    for (const auto& [stripName, stripError] : audioEngine.getDeviceErrors())
        lines.add(stripName + ": " + stripError);
    if (lines.isEmpty()) lines.add("No audio errors.");
    showError(lines.joinIntoString("\n"));
}

void MainComponent::showSnapshotPlaceholder() {
    juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "PluginRack",
                                           "Snapshots are not implemented yet - scene save/load stays in the toolbar.");
}

bool MainComponent::refuseSaveWhileRestoring() {
    if (!audioEngine.isSceneRestoreActive()) return false;
    showError("The scene is still loading. Please try saving again in a moment.");
    return true;
}

void MainComponent::saveScene() {
    if (refuseSaveWhileRestoring()) return;
    const auto error = SceneBridge::save(audioEngine.makeScene());
    if (error.isNotEmpty()) showError(error);
}

void MainComponent::saveSceneAs() {
    if (refuseSaveWhileRestoring()) return;
    auto* w = new juce::AlertWindow("Save Scene", "Enter a name for this save.", juce::MessageBoxIconType::QuestionIcon);
    w->addTextEditor("name", "", "Save name");
    w->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
    w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    juce::Component::SafePointer<MainComponent> safe(this);
    w->enterModalState(true, juce::ModalCallbackFunction::create([safe, w](int result) {
        if (result == 1 && safe != nullptr) {
            const auto name = w->getTextEditorContents("name");
            const auto error = SceneBridge::saveNamed(safe->audioEngine.makeScene(), name);
            if (error.isNotEmpty()) safe->showError(error);
            else {
                // The Save button now overwrites this name until another
                // scene is loaded or saved.
                safe->activeSaveName_ = name;
                safe->refreshViews();
            }
        }
        delete w;
    }), true);
}

void MainComponent::loadScene(bool showMissingMessage) {
    juce::String error;
    auto saved = SceneBridge::loadDefaultOrLast(error);
    if (error.isNotEmpty()) { showError(error); return; }
    if (!saved.has_value()) {
        if (showMissingMessage) showError("No scene has been saved yet.");
        return;
    }
    juce::Component::SafePointer<MainComponent> safe(this);
    const auto startError = applySceneAsync(*saved, true, [safe](const juce::String& pluginErrors) {
        if (safe == nullptr) return;
        if (pluginErrors.isNotEmpty()) {
            safe->showError("The scene was loaded, but some plugins could not be restored:\n"
                            + pluginErrors);
        } else {
            // The startup scene becomes the active save, so Save overwrites it.
            const auto defaultName = SceneBridge::getDefaultSaveName();
            if (defaultName.isNotEmpty()) safe->activeSaveName_ = defaultName;
        }
    });
    if (startError.isNotEmpty()) showError(startError);
}

juce::String MainComponent::applySceneAsync(const pluginrack::Scene& scene, bool openDevicesNow,
                                            std::function<void(const juce::String&)> onFinished) {
    // The engine instantiates one insert per message-loop turn; this call only
    // validates, releases editors, and starts the staged build. sceneLoading_
    // keeps menu/Saves loads and saves from overlapping it.
    if (sceneLoading_.exchange(true))
        return "A scene is already loading. Please try again in a moment.";
    if (audioEngine.isOpeningDevices()) {
        sceneLoading_.store(false);
        return "Audio is still starting up. Please try again in a moment.";
    }
    // An editor must not outlive the processor that backs it; close them while
    // the old strips are still alive. refreshViews re-docks after the commit.
    router.releaseDockedEditor();
    mixer.closeAllEqWindows();
    juce::Component::SafePointer<MainComponent> safe(this);
    const auto error = audioEngine.beginSceneRestore(
        scene, openDevicesNow,
        [safe, onFinished](juce::String pluginErrors) {
            if (safe != nullptr) {
                safe->sceneLoading_.store(false);
                safe->refreshViews();
            }
            if (onFinished) onFinished(pluginErrors);
        });
    if (error.isNotEmpty()) {
        sceneLoading_.store(false);
        return error;
    }
    return {};
}

juce::String MainComponent::applyScene(const pluginrack::Scene& scene, bool openDevicesNow) {
    // All of restoreScene runs here, on the message thread: it
    // instantiates plugins, swaps the strip vectors, and destroys the old
    // strips whose PluginHosts own JUCE editor windows - none of which may
    // happen on a worker thread. The device streams themselves are opened by
    // the engine's startup worker when openDevicesNow is false. The flag
    // serialises overlapping menu/Saves loads and resets on every path;
    // dialogs stay on the message thread too.
    if (sceneLoading_.exchange(true))
        return "A scene is already loading. Please try again in a moment.";
    if (audioEngine.isOpeningDevices()) {
        sceneLoading_.store(false);
        const juce::String message =
            "Audio is still starting up. Please try again in a moment.";
        showError(message);
        return message;
    }
    // The rack may dock an editor from a strip whose plugin dies inside
    // restoreScene; release it first (an editor outliving its processor
    // is unsafe to delete). refreshViews below re-docks against the new
    // strips. Mixer strips hold Channel&/Output& refs and their open EQ
    // windows call back into those refs on destruction (analyzerTap): close
    // them while the engine strips are still alive, before restoreScene
    // frees the refs the pending rebuild would otherwise tear down.
    router.releaseDockedEditor();
    mixer.closeAllEqWindows();
    const juce::String pluginErrors = audioEngine.restoreScene(scene, openDevicesNow);
    sceneLoading_.store(false);
    refreshViews();
    return pluginErrors;
}

void MainComponent::initialiseAudio() {
    juce::String error;
    auto saved = SceneBridge::loadDefaultOrLast(error);
    if (error.isNotEmpty()) {
        showError(error);
        audioEngine.openDefaultDevicesAsync();
    } else if (saved.has_value()) {
        // Build the strips and load the plugins in the staged restore (one
        // plugin per message-loop turn), then let the engine open the streams
        // on its worker: a slow plugin or WASAPI probe must not take the
        // message thread with it.
        juce::Component::SafePointer<MainComponent> safe(this);
        const auto startError = applySceneAsync(*saved, /*openDevicesNow=*/false,
                                                [safe](const juce::String& pluginErrors) {
            if (safe == nullptr) return;
            if (pluginErrors.isNotEmpty())
                safe->showError("The scene was loaded, but some plugins could not be restored:\n"
                                + pluginErrors);
            safe->audioEngine.openStripsDevicesAsync();
        });
        if (startError.isNotEmpty()) {
            showError(startError);
            audioEngine.openDefaultDevicesAsync();
        }
    } else {
        audioEngine.openDefaultDevicesAsync();
    }
    // A failed exit autosave restored the previous session instead of the one
    // the user left: say so, or the state looks inexplicably stale.
    const auto saveFailure = SceneBridge::takeSaveFailureNote();
    if (saveFailure.isNotEmpty()) {
        showError("The session from your last quit could not be saved, so the previous "
                  "session was restored.\n" + saveFailure);
    }
    timerCallback();
}

void MainComponent::saveSceneOnExit() {
    if (audioEngine.isSceneRestoreActive()) {
        // The live strips are mid-replacement; saving now would write the old
        // rig. The previous exit autosave stays as the last good session.
        juce::Logger::writeToLog("exit autosave skipped: a scene restore was still running");
        return;
    }
    // shutdown() terminates the process right after this returns, so a dialog
    // cannot be shown here. Log it and leave a note the next launch surfaces.
    const auto error = SceneBridge::saveLast(audioEngine.makeScene());
    if (error.isNotEmpty()) {
        juce::Logger::writeToLog("exit autosave failed: " + error);
        SceneBridge::writeSaveFailureNote(error);
    }
}

void MainComponent::pollSceneChanges() {
    const auto nowMs = juce::Time::getMillisecondCounterHiRes();
    if (nowMs < nextScenePollMs_) return;
    nextScenePollMs_ = nowMs + kScenePollIntervalMs;

    // A restore replaces the rig wholesale; the scene it installs is the new
    // baseline, not an edit that should be autosaved. While one runs this poll
    // is often not reached at all (timerCallback returns early), so the reset
    // also happens on the first poll after it finishes.
    if (sceneLoading_ || audioEngine.isSceneRestoreActive()) {
        haveChangeBaseline_ = false;
        haveObservedBaseline_ = false;
        sceneChangedSinceSave_ = false;
        return;
    }
    if (audioEngine.isOpeningDevices()) return;
    if (audioEngine.getChannels().empty() || audioEngine.getOutputs().empty()) return;

    // Both counters are lock-free reads published by the engine: an idle poll
    // costs two atomic loads and nothing else. A setter that changes saved
    // state bumps the structure revision; a plugin editor knob bumps its
    // parameter revision.
    const auto structureRevision = audioEngine.getStructureRevision();
    const auto pluginRevision = audioEngine.getPluginEditRevision();
    if (!haveChangeBaseline_) {
        lastStructureHash_ =
            sceneFingerprint(pluginrack::toJson(audioEngine.makeScene(false)));
        lastStructureRevision_ = structureRevision;
        lastPluginRevision_ = pluginRevision;
        haveChangeBaseline_ = true;
        return;
    }
    // Early return: the scene can only have changed if one of the two
    // revisions moved, so no makeScene, no toJson, no fingerprint.
    if (structureRevision == lastStructureRevision_ && pluginRevision == lastPluginRevision_)
        return;

    // A revision moved, but the edit may serialise exactly like the last
    // polled scene (an undo, or a setter that reapplied the same value). Only
    // this exact-fingerprint comparison decides the save, as before.
    const auto structureHash =
        sceneFingerprint(pluginrack::toJson(audioEngine.makeScene(false)));
    const bool unchanged = structureHash == lastStructureHash_
                           && pluginRevision == lastPluginRevision_;
    lastStructureHash_ = structureHash;
    lastStructureRevision_ = structureRevision;
    lastPluginRevision_ = pluginRevision;
    if (unchanged) return;

    lastSceneChangeMs_ = nowMs;
    if (!sceneChangedSinceSave_) {
        sceneChangedSinceSave_ = true;
        juce::Logger::writeToLog("autosave: change detected, saving after 30 s of quiet");
    }
}

void MainComponent::checkFullSceneSafetyNet() {
    if (sceneLoading_ || audioEngine.isSceneRestoreActive() || audioEngine.isOpeningDevices())
        return;
    if (audioEngine.getChannels().empty() || audioEngine.getOutputs().empty()) return;

    // The only path that captures plugin state on a timer. A plugin that loads
    // a preset without touching a parameter is invisible to the cheap poll, so
    // this runs every 30 s and treats any difference as an edit.
    const auto scene = audioEngine.makeScene();
    const auto json = pluginrack::toJson(scene);
    const auto fingerprint = sceneFingerprint(json);
    const auto size = static_cast<juce::uint64>(json.size());
    if (!haveObservedBaseline_) {
        lastObservedHash_ = fingerprint;
        lastObservedSize_ = size;
        haveObservedBaseline_ = true;
        return;
    }
    if (fingerprint == lastObservedHash_ && size == lastObservedSize_) return;

    lastObservedHash_ = fingerprint;
    lastObservedSize_ = size;
    if (!sceneChangedSinceSave_) {
        lastSceneChangeMs_ = juce::Time::getMillisecondCounterHiRes();
        sceneChangedSinceSave_ = true;
        juce::Logger::writeToLog("autosave: scene changed without a parameter notification");
    }
}

void MainComponent::autosaveIfChanged() {
    // A restore or device open owns the rig: capturing now would mix the old
    // and new strips, or save a half-built output list. Leave the pending
    // change in place; the next quiet interval retries it.
    if (sceneLoading_ || audioEngine.isSceneRestoreActive() || audioEngine.isOpeningDevices())
        return;
    if (audioEngine.getChannels().empty() || audioEngine.getOutputs().empty())
        return;

    auto scene = audioEngine.makeScene();
    const auto json = pluginrack::toJson(scene);
    const auto fingerprint = sceneFingerprint(json);
    const auto size = static_cast<juce::uint64>(json.size());

    // This capture is also the newest full-scene observation, or the safety
    // net would report the just-saved scene as a change on its next run.
    lastObservedHash_ = fingerprint;
    lastObservedSize_ = size;
    haveObservedBaseline_ = true;

    if (haveSavedBaseline_ && fingerprint == lastSavedHash_ && size == lastSavedSize_) {
        // The edit was undone back to what is already on disk.
        sceneChangedSinceSave_ = false;
        juce::Logger::writeToLog("autosave: scene matches the save, write skipped");
        return;
    }
    // Errors are logged, not surfaced in a dialog: this runs while the user is
    // playing, and an exit save remains the second line of defence. No failure
    // note here, so a transient autosave failure does not make the next launch
    // claim the previous quit was lost.
    const auto error = SceneBridge::saveLast(scene);
    if (error.isNotEmpty()) {
        juce::Logger::writeToLog("autosave failed: " + error);
        // Retry after another quiet interval instead of on every tick.
        lastSceneChangeMs_ = juce::Time::getMillisecondCounterHiRes();
        return;
    }
    haveSavedBaseline_ = true;
    lastSavedHash_ = fingerprint;
    lastSavedSize_ = size;
    sceneChangedSinceSave_ = false;
    juce::Logger::writeToLog("autosave: wrote last scene");
}

void MainComponent::refreshViews() {
    // Only drop the docked editor when its insert is gone: a structure change
    // (an insert being added) must not destroy and rebuild a live plugin UI.
    router.releaseOrphanedEditor();
    mixer.rebuild();
    router.refreshViews();
}

void MainComponent::showError(const juce::String& message) {
    juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "PluginRack", message);
}
