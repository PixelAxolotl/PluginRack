add_subdirectory("${JUCE_DIR}" "${CMAKE_BINARY_DIR}/juce")
juce_set_vst3_sdk_path("${VST3_SDK_DIR}")

juce_add_gui_app(pluginrack_app
    PRODUCT_NAME "PluginRack"
    BUNDLE_ID com.pluginrack.app
    MICROPHONE_PERMISSION_ENABLED TRUE
    NEEDS_WEB_BROWSER FALSE
    NEEDS_CURL FALSE)

juce_generate_juce_header(pluginrack_app)

target_sources(pluginrack_app PRIVATE
    src/AudioEngine.cpp
    src/AudioEngine.h
    src/ChannelStrip.cpp
    src/ChannelStrip.h
    src/ConsoleLookAndFeel.cpp
    src/ConsoleLookAndFeel.h
    src/DeviceControl.cpp
    src/DeviceControl.h
    src/DeviceMenu.cpp
    src/InsertHost.h
    src/InsertMiniView.cpp
    src/InsertMiniView.h
    src/IoView.cpp
    src/IoView.h
    src/PageHeader.h
    src/DeviceMenu.h
    src/DynamicsWindow.cpp
    src/DynamicsWindow.h
    src/DynamicsCurveComponent.cpp
    src/DynamicsCurveComponent.h
    src/DeviceStream.cpp
    src/DeviceStream.h
    src/EQCurveComponent.cpp
    src/EQCurveComponent.h
    src/EQWindow.cpp
    src/EQWindow.h
    src/LevelMeter.cpp
    src/LevelMeter.h
    src/Main.cpp
    src/MainComponent.cpp
    src/MainComponent.h
    src/MixerComponent.cpp
    src/MixerComponent.h
    src/NativeThumbnail.cpp
    src/NativeThumbnail.h
    src/PluginHost.cpp
    src/PluginHost.h
    src/PluginLibrary.cpp
    src/PluginLibrary.h
    src/PluginMenu.cpp
    src/PluginMenu.h
    src/PluginKnobWatcher.cpp
    src/PluginKnobWatcher.h
    src/RackView.cpp
    src/RackView.h
    src/RoutingMatrixComponent.cpp
    src/RoutingMatrixComponent.h
    src/SavesView.cpp
    src/SavesView.h
    src/SceneBridge.cpp
    src/SceneBridge.h
    src/SettingsView.cpp
    src/SettingsView.h
    src/SliderTouch.cpp
    src/SliderTouch.h
    src/TouchSlideBar.cpp
    src/TouchSlideBar.h
    src/ValueEditSlider.cpp
    src/ValueEditSlider.h)

target_compile_features(pluginrack_app PRIVATE cxx_std_20)
target_compile_definitions(pluginrack_app PRIVATE
    JUCE_PLUGINHOST_VST=0
    JUCE_PLUGINHOST_VST3=1
    JUCE_STANDALONE_APPLICATION=1
    JUCE_USE_CURL=0
    JUCE_VST3_HOST_CROSS_PLATFORM_UID=1
    JUCE_WEB_BROWSER=0)

# ASIO support: JUCE 9 bundles the Steinberg host headers (asio.h, asiosys.h,
# iasiodrv.h) next to juce_audio_devices. When they are present enable ASIO so
# DeviceStream can try the "ASIO" type last in its fallback chain. Never vendor
# SDK binaries here; without the headers the flag stays off and the UI labels
# ASIO as unavailable.
if(EXISTS "${JUCE_DIR}/modules/juce_audio_devices/native/asio/iasiodrv.h")
    target_compile_definitions(pluginrack_app PRIVATE JUCE_ASIO=1)
endif()

target_link_libraries(pluginrack_app PRIVATE
    pluginrack_core
    juce::juce_audio_basics
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_core
    juce::juce_data_structures
    juce::juce_dsp
    juce::juce_events
    juce::juce_graphics
    juce::juce_gui_basics
    juce::juce_gui_extra
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

if(MSVC)
    target_compile_options(pluginrack_app PRIVATE /W4 /WX)
    target_compile_options(pluginrack_app PRIVATE /utf-8)
    # Keep symbols in every configuration: a crash report only names the module
    # and a code offset, and resolving it needs the matching PDB. The build
    # script copies PluginRack.pdb next to the exe.
    target_compile_options(pluginrack_app PRIVATE /Zi)
    target_link_options(pluginrack_app PRIVATE /DEBUG)
endif()
