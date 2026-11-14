# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 The Pantheon authors

cmake_minimum_required(VERSION 3.22)

# Universal binary so the plug-in loads whether FL Studio runs natively or under Rosetta.
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "")
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")

project(Harmonia VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# JUCE is not vendored: cmake/Juce.cmake fetches it (and owns the version).
include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/Juce.cmake) # JUCE, one version for the repository

# The interface kit shared by the plugins of this repository (shared/ui).
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../shared/ui ${CMAKE_BINARY_DIR}/shared/ui)

set(HARMONIA_SOURCES
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)


# The plugin's name, written once. JUCE turns PLUGIN_NAME into JucePlugin_Name, which the processor reports to the host;
# the developer tool is not a plugin target, so it gets the same value as a definition.
set(HARMONIA_PLUGIN_NAME "Harmonia Pitch")

juce_add_plugin(Harmonia
    COMPANY_NAME "Harmonia"
    PRODUCT_NAME "${HARMONIA_PLUGIN_NAME}"
    PLUGIN_NAME "${HARMONIA_PLUGIN_NAME}"
    PLUGIN_MANUFACTURER_CODE Hrmn
    PLUGIN_CODE Hmna
    FORMATS VST3 AU Standalone
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    VST3_CATEGORIES Fx "Pitch Shift"
    AU_MAIN_TYPE kAudioUnitType_Effect
    COPY_PLUGIN_AFTER_BUILD TRUE
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "Harmonia needs the microphone to tune your voice in the standalone app.")

target_sources(Harmonia PRIVATE ${HARMONIA_SOURCES})

target_include_directories(Harmonia PRIVATE Source)

target_compile_definitions(Harmonia PUBLIC
    ${PANTHEON_JUCE_DEFINITIONS}
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(Harmonia
    PRIVATE
        pantheon_ui
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

# Developer tool: headless self-test of the real processor + editor snapshot. Not installed.
#   cmake --build build --target HarmoniaDevTool && build/HarmoniaDevTool_artefacts/*/HarmoniaDevTool selftest
juce_add_console_app(HarmoniaDevTool PRODUCT_NAME "HarmoniaDevTool")
set_target_properties(HarmoniaDevTool PROPERTIES EXCLUDE_FROM_ALL TRUE)
target_sources(HarmoniaDevTool PRIVATE Tests/DevTool.cpp ${HARMONIA_SOURCES})
target_include_directories(HarmoniaDevTool PRIVATE Source)
target_compile_definitions(HarmoniaDevTool PRIVATE
    ${PANTHEON_JUCE_DEFINITIONS}
    JucePlugin_Name="${HARMONIA_PLUGIN_NAME}"
    JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(HarmoniaDevTool
    PRIVATE pantheon_ui juce::juce_audio_utils
    PUBLIC juce::juce_recommended_config_flags)
