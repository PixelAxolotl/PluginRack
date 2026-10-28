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
    src/DeviceMenu.h
    src/DeviceStream.cpp
    src/DeviceStream.h
    src/LevelMeter.cpp
    src/LevelMeter.h
    src/Main.cpp
    src/MainComponent.cpp
    src/MainComponent.h
    src/MixerComponent.cpp
    src/MixerComponent.h
    src/PluginHost.cpp
    src/PluginHost.h
    src/RoutingMatrixComponent.cpp
    src/RoutingMatrixComponent.h
    src/SceneBridge.cpp
    src/SceneBridge.h)

target_compile_features(pluginrack_app PRIVATE cxx_std_20)
target_compile_definitions(pluginrack_app PRIVATE
    JUCE_PLUGINHOST_VST=0
    JUCE_PLUGINHOST_VST3=1
    JUCE_STANDALONE_APPLICATION=1
    JUCE_USE_CURL=0
    JUCE_VST3_HOST_CROSS_PLATFORM_UID=1
    JUCE_WEB_BROWSER=0)

target_link_libraries(pluginrack_app PRIVATE
    pluginrack_core
    juce::juce_audio_basics
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_core
    juce::juce_data_structures
    juce::juce_events
    juce::juce_graphics
    juce::juce_gui_basics
    juce::juce_gui_extra
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

if(MSVC)
    target_compile_options(pluginrack_app PRIVATE /W4 /WX)
endif()
