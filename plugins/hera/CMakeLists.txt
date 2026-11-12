# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 The Pantheon authors

cmake_minimum_required(VERSION 3.22)

# Universal binary so the plugin loads whether the host runs natively or under Rosetta.
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "")
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")

project(Hera VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(HERA_INSTALL_AFTER_BUILD "Copy the plugin into the user plugin folders after building (macOS)" OFF)

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
set(HERA_PLUGIN_NAME "Hera Bond")

juce_add_plugin(Hera
    COMPANY_NAME "Hera Audio"
    BUNDLE_ID com.heraaudio.hera
    PLUGIN_MANUFACTURER_CODE Hera
    PLUGIN_CODE Hrbd
    FORMATS VST3 AU Standalone
    PRODUCT_NAME "${HERA_PLUGIN_NAME}"
    PLUGIN_NAME "${HERA_PLUGIN_NAME}"
    DESCRIPTION "Compressor: peak or RMS detector, soft knee, auto release and makeup, parallel mix, sidechain with high-pass, stereo link"
    VST3_CATEGORIES Fx Dynamics
    AU_MAIN_TYPE kAudioUnitType_Effect
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    COPY_PLUGIN_AFTER_BUILD ${HERA_INSTALL_AFTER_BUILD})

target_sources(Hera PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

target_compile_definitions(Hera PUBLIC ${PANTHEON_JUCE_DEFINITIONS})

target_link_libraries(Hera
    PRIVATE
        pantheon_ui
        pantheon_dsp
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# ---------------------------------------------------------------------------------------
# Headless DSP tests (no plugin, no GUI):  cmake --build build --target HeraTests
# ---------------------------------------------------------------------------------------
juce_add_console_app(HeraTests PRODUCT_NAME "HeraTests")
target_sources(HeraTests PRIVATE Tests/Tests.cpp)
target_include_directories(HeraTests PRIVATE Source Tests)
target_link_libraries(HeraTests
    PRIVATE
        pantheon_dsp
        juce::juce_dsp
        juce::juce_audio_basics
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
set_target_properties(HeraTests PROPERTIES EXCLUDE_FROM_ALL TRUE OSX_ARCHITECTURES "${CMAKE_HOST_SYSTEM_PROCESSOR}")

# ---------------------------------------------------------------------------------------
# Snapshot: renders the real editor component to a PNG (for design review / documentation).
#   cmake --build build --target HeraSnapshot
# ---------------------------------------------------------------------------------------
juce_add_console_app(HeraSnapshot PRODUCT_NAME "HeraSnapshot")
target_sources(HeraSnapshot PRIVATE Tests/Snapshot.cpp Source/PluginProcessor.cpp Source/PluginEditor.cpp)
target_include_directories(HeraSnapshot PRIVATE Source Tests)
target_compile_definitions(HeraSnapshot PRIVATE JucePlugin_Name="${HERA_PLUGIN_NAME}" JUCE_MODAL_LOOPS_PERMITTED=1 ${PANTHEON_JUCE_DEFINITIONS})
target_link_libraries(HeraSnapshot
    PRIVATE
        pantheon_ui
        pantheon_dsp
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags)
set_target_properties(HeraSnapshot PROPERTIES EXCLUDE_FROM_ALL TRUE OSX_ARCHITECTURES "${CMAKE_HOST_SYSTEM_PROCESSOR}")

# ---------------------------------------------------------------------------------------
# UI check: the editor built on the real processor (parameters, solo and delta buttons).
#   cmake --build build --target HeraUiCheck
# ---------------------------------------------------------------------------------------
juce_add_console_app(HeraUiCheck PRODUCT_NAME "HeraUiCheck")
target_sources(HeraUiCheck PRIVATE Tests/UiCheck.cpp Source/PluginProcessor.cpp Source/PluginEditor.cpp)
target_include_directories(HeraUiCheck PRIVATE Source Tests)
target_compile_definitions(HeraUiCheck PRIVATE JucePlugin_Name="${HERA_PLUGIN_NAME}" JUCE_MODAL_LOOPS_PERMITTED=1 ${PANTHEON_JUCE_DEFINITIONS})
target_link_libraries(HeraUiCheck
    PRIVATE
        pantheon_ui
        pantheon_dsp
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags)
set_target_properties(HeraUiCheck PROPERTIES EXCLUDE_FROM_ALL TRUE OSX_ARCHITECTURES "${CMAKE_HOST_SYSTEM_PROCESSOR}")

