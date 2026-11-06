cmake_minimum_required(VERSION 3.22)
project(MixWrapper VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 17)

# Put (or clone) JUCE 7.0.5+ / 8 in a folder called "JUCE" next to this file:
#   git clone --depth 1 https://github.com/juce-framework/JUCE.git
add_subdirectory(JUCE)

juce_add_plugin(MixWrapper
    COMPANY_NAME "MixWrapper"
    PLUGIN_MANUFACTURER_CODE Dmwr
    PLUGIN_CODE Mxwp
    FORMATS AU
    PRODUCT_NAME "Mix Wrapper"
    AU_MAIN_TYPE kAudioUnitType_Effect
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    COPY_PLUGIN_AFTER_BUILD TRUE)

juce_generate_juce_header(MixWrapper)

target_sources(MixWrapper PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

target_compile_definitions(MixWrapper PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_PLUGINHOST_AU=1
    JUCE_PLUGINHOST_VST3=1)

target_link_libraries(MixWrapper
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_processors
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
