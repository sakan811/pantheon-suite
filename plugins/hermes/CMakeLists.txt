# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 The Pantheon authors

cmake_minimum_required(VERSION 3.22)

# Universal binary so the plugin loads whether the host runs natively or under Rosetta.
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "")
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")

project(Hermes VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(HERMES_INSTALL_AFTER_BUILD "Copy the plugin into the user plugin folders after building (macOS)" OFF)

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
set(HERMES_PLUGIN_NAME "Hermes Level")

juce_add_plugin(Hermes
    COMPANY_NAME "Hermes Audio"
    BUNDLE_ID com.hermesaudio.hermes
    PLUGIN_MANUFACTURER_CODE Herm
    PLUGIN_CODE Hrlv
    FORMATS VST3 AU Standalone
    PRODUCT_NAME "${HERMES_PLUGIN_NAME}"
    PLUGIN_NAME "${HERMES_PLUGIN_NAME}"
    DESCRIPTION "Gain staging and loudness: trim, polarity, mono / left / right / side listening, LUFS and true-peak meters, loudness-matched reference A/B"
    VST3_CATEGORIES Fx Analyzer
    AU_MAIN_TYPE kAudioUnitType_Effect
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    COPY_PLUGIN_AFTER_BUILD ${HERMES_INSTALL_AFTER_BUILD})

target_sources(Hermes PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

target_compile_definitions(Hermes PUBLIC ${PANTHEON_JUCE_DEFINITIONS})

target_link_libraries(Hermes
    PRIVATE
        pantheon_ui
        pantheon_dsp
        juce::juce_audio_utils
        juce::juce_audio_formats
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# ---------------------------------------------------------------------------------------
# Headless DSP tests (no plugin, no GUI):  cmake --build build --target HermesTests
# ---------------------------------------------------------------------------------------
juce_add_console_app(HermesTests PRODUCT_NAME "HermesTests")
target_sources(HermesTests PRIVATE Tests/Tests.cpp)
target_include_directories(HermesTests PRIVATE Source Tests)
target_link_libraries(HermesTests
    PRIVATE
        pantheon_dsp
        juce::juce_dsp
        juce::juce_audio_basics
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
set_target_properties(HermesTests PROPERTIES EXCLUDE_FROM_ALL TRUE OSX_ARCHITECTURES "${CMAKE_HOST_SYSTEM_PROCESSOR}")

# ---------------------------------------------------------------------------------------
# Snapshot: renders the real editor component to a PNG (for design review / documentation).
#   cmake --build build --target HermesSnapshot
# ---------------------------------------------------------------------------------------
juce_add_console_app(HermesSnapshot PRODUCT_NAME "HermesSnapshot")
target_sources(HermesSnapshot PRIVATE Tests/Snapshot.cpp Source/PluginProcessor.cpp Source/PluginEditor.cpp)
target_include_directories(HermesSnapshot PRIVATE Source Tests)
target_compile_definitions(HermesSnapshot PRIVATE JucePlugin_Name="${HERMES_PLUGIN_NAME}" JUCE_MODAL_LOOPS_PERMITTED=1 ${PANTHEON_JUCE_DEFINITIONS})
target_link_libraries(HermesSnapshot
    PRIVATE
        pantheon_ui
        pantheon_dsp
        juce::juce_audio_utils
        juce::juce_audio_formats
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags)
set_target_properties(HermesSnapshot PROPERTIES EXCLUDE_FROM_ALL TRUE OSX_ARCHITECTURES "${CMAKE_HOST_SYSTEM_PROCESSOR}")

# ---------------------------------------------------------------------------------------
# UI check: the editor built on the real processor (parameters, solo and delta buttons).
#   cmake --build build --target HermesUiCheck
# ---------------------------------------------------------------------------------------
juce_add_console_app(HermesUiCheck PRODUCT_NAME "HermesUiCheck")
target_sources(HermesUiCheck PRIVATE Tests/UiCheck.cpp Source/PluginProcessor.cpp Source/PluginEditor.cpp)
target_include_directories(HermesUiCheck PRIVATE Source Tests)
target_compile_definitions(HermesUiCheck PRIVATE JucePlugin_Name="${HERMES_PLUGIN_NAME}" JUCE_MODAL_LOOPS_PERMITTED=1 ${PANTHEON_JUCE_DEFINITIONS})
target_link_libraries(HermesUiCheck
    PRIVATE
        pantheon_ui
        pantheon_dsp
        juce::juce_audio_utils
        juce::juce_audio_formats
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags)
set_target_properties(HermesUiCheck PROPERTIES EXCLUDE_FROM_ALL TRUE OSX_ARCHITECTURES "${CMAKE_HOST_SYSTEM_PROCESSOR}")

