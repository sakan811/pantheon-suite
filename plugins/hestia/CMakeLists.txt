# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 The Pantheon authors

cmake_minimum_required(VERSION 3.22)

# Universal binary so the plugin loads whether the host runs natively or under Rosetta.
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "")
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")

project(Hestia VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(HESTIA_INSTALL_AFTER_BUILD "Copy the plugin into the user plugin folders after building (macOS)" OFF)

# JUCE is not vendored: cmake/Juce.cmake fetches it (and owns the version).
include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/Juce.cmake) # JUCE, one version for the repository

# The interface kit and the DSP shared by the plugins of this repository.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../shared/ui ${CMAKE_BINARY_DIR}/shared/ui)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../shared/dsp ${CMAKE_BINARY_DIR}/shared/dsp)

# ---------------------------------------------------------------------------------------
# Plugin
# ---------------------------------------------------------------------------------------
# The plugin's name, written once. JUCE turns PLUGIN_NAME into JucePlugin_Name, which the processor reports to the host;
# the check targets that compile the processor are not plugin targets, so they get the same value as a definition.
set(HESTIA_PLUGIN_NAME "Hestia Space")

juce_add_plugin(Hestia
    COMPANY_NAME "Hestia Audio"
    BUNDLE_ID com.hestiaaudio.hestia
    PLUGIN_MANUFACTURER_CODE Hsta
    PLUGIN_CODE Hsps
    FORMATS VST3 AU Standalone
    PRODUCT_NAME "${HESTIA_PLUGIN_NAME}"
    PLUGIN_NAME "${HESTIA_PLUGIN_NAME}"
    DESCRIPTION "Stem spacer: each stem gives up the bands where a more important stem is playing, found through the other instances in the session"
    VST3_CATEGORIES Fx EQ
    AU_MAIN_TYPE kAudioUnitType_Effect
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    COPY_PLUGIN_AFTER_BUILD ${HESTIA_INSTALL_AFTER_BUILD})

target_sources(Hestia PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

target_compile_definitions(Hestia PUBLIC ${PANTHEON_JUCE_DEFINITIONS})

target_link_libraries(Hestia
    PRIVATE
        pantheon_ui
        pantheon_dsp
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# ---------------------------------------------------------------------------------------
# Headless DSP tests (no plugin, no GUI):  cmake --build build --target HestiaTests
# ---------------------------------------------------------------------------------------
juce_add_console_app(HestiaTests PRODUCT_NAME "HestiaTests")
target_sources(HestiaTests PRIVATE Tests/Tests.cpp)
target_include_directories(HestiaTests PRIVATE Source Tests)
target_link_libraries(HestiaTests
    PRIVATE
        pantheon_dsp
        juce::juce_audio_basics
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
set_target_properties(HestiaTests PROPERTIES EXCLUDE_FROM_ALL TRUE OSX_ARCHITECTURES "${CMAKE_HOST_SYSTEM_PROCESSOR}")

# ---------------------------------------------------------------------------------------
# Snapshot: renders the real editor component to a PNG (for design review / documentation).
#   cmake --build build --target HestiaSnapshot
# ---------------------------------------------------------------------------------------
juce_add_console_app(HestiaSnapshot PRODUCT_NAME "HestiaSnapshot")
target_sources(HestiaSnapshot PRIVATE Tests/Snapshot.cpp Source/PluginProcessor.cpp Source/PluginEditor.cpp)
target_include_directories(HestiaSnapshot PRIVATE Source Tests)
target_compile_definitions(HestiaSnapshot PRIVATE JucePlugin_Name="${HESTIA_PLUGIN_NAME}" JUCE_MODAL_LOOPS_PERMITTED=1 ${PANTHEON_JUCE_DEFINITIONS})
target_link_libraries(HestiaSnapshot
    PRIVATE
        pantheon_ui
        pantheon_dsp
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags)
set_target_properties(HestiaSnapshot PROPERTIES EXCLUDE_FROM_ALL TRUE OSX_ARCHITECTURES "${CMAKE_HOST_SYSTEM_PROCESSOR}")

# ---------------------------------------------------------------------------------------
# UI check: the editor built on the real processor (parameters, solo and delta buttons).
#   cmake --build build --target HestiaUiCheck
# ---------------------------------------------------------------------------------------
juce_add_console_app(HestiaUiCheck PRODUCT_NAME "HestiaUiCheck")
target_sources(HestiaUiCheck PRIVATE Tests/UiCheck.cpp Source/PluginProcessor.cpp Source/PluginEditor.cpp)
target_include_directories(HestiaUiCheck PRIVATE Source Tests)
target_compile_definitions(HestiaUiCheck PRIVATE JucePlugin_Name="${HESTIA_PLUGIN_NAME}" JUCE_MODAL_LOOPS_PERMITTED=1 ${PANTHEON_JUCE_DEFINITIONS})
target_link_libraries(HestiaUiCheck
    PRIVATE
        pantheon_ui
        pantheon_dsp
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags)
set_target_properties(HestiaUiCheck PROPERTIES EXCLUDE_FROM_ALL TRUE OSX_ARCHITECTURES "${CMAKE_HOST_SYSTEM_PROCESSOR}")

