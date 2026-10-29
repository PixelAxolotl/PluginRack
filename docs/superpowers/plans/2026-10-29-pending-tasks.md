# PluginRack Pending Tasks

## Pending Feature: Make channels renamable

**Status:** Not started.

**Goal:** Allow the user to rename channel strips (and possibly outputs) in the UI, persisting the name in the scene model.

**Scope:**
- AudioEngine: `setChannelName` already exists (added during split-multichannel-inputs Task 4). Ensure it is wired to UI.
- UI: Add a rename affordance (double-click on channel name label, or edit-in-place, or a small dialog) in ChannelStrip header.
- Scene model: `Channel::name` already persisted in codec. No schema change needed.
- makeScene/restoreScene: already save/restore `name`.

**Files likely touched:**
- app/src/ChannelStrip.h/.cpp (rename affordance)
- app/src/MixerComponent.cpp (rebuild on rename)
- Possibly app/src/RackView.cpp (when Rack UI Task 2 is implemented)

**Notes:**
- `AudioEngine::setChannelName(uint32_t channelId, const juce::String& name)` already exists.
- Output renaming: `Output::name` persisted; add `AudioEngine::setOutputName` if not present.

## Pending: Rack UI Tasks 2-7

See docs/superpowers/plans/2026-10-29-rack-ui.md for full plan. Task 1 (app shell + toolbar + view nav) is complete. Tasks 2-7 (rack view, chain selection, mixer view, routing/settings views, meters/states, legacy removal) remain.