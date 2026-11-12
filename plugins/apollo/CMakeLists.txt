# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 The Pantheon authors

cmake_minimum_required(VERSION 3.22)

# Universal binary so the plugin loads whether the host runs natively or under Rosetta.
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "")
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")

project(Apollo VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(APOLLO_INSTALL_AFTER_BUILD "Copy the plugin into the user plugin folders after building (macOS)" OFF)

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
set(APOLLO_PLUGIN_NAME "Apollo Curve")

juce_add_plugin(Apollo
    COMPANY_NAME "Apollo Audio"
    BUNDLE_ID com.apolloaudio.apollo
    PLUGIN_MANUFACTURER_CODE Apol
    PLUGIN_CODE Apcv
    FORMATS VST3 AU Standalone
    PRODUCT_NAME "${APOLLO_PLUGIN_NAME}"
    PLUGIN_NAME "${APOLLO_PLUGIN_NAME}"
    DESCRIPTION "Eight-band parametric equaliser: bell, shelves, high and low-pass up to 48 dB per octave, mid/side per band, minimum or linear phase"
    VST3_CATEGORIES Fx EQ
    AU_MAIN_TYPE kAudioUnitType_Effect
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    COPY_PLUGIN_AFTER_BUILD ${APOLLO_INSTALL_AFTER_BUILD})

target_sources(Apollo PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

target_compile_definitions(Apollo PUBLIC ${PANTHEON_JUCE_DEFINITIONS})

target_link_libraries(Apollo
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
# Headless DSP tests (no plugin, no GUI):  cmake --build build --target ApolloTests
# ---------------------------------------------------------------------------------------
juce_add_console_app(ApolloTests PRODUCT_NAME "ApolloTests")
target_sources(ApolloTests PRIVATE Tests/Tests.cpp)
target_include_directories(ApolloTests PRIVATE Source Tests)
target_link_libraries(ApolloTests
    PRIVATE
        pantheon_dsp
        juce::juce_dsp
        juce::juce_audio_basics
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
set_target_properties(ApolloTests PROPERTIES EXCLUDE_FROM_ALL TRUE OSX_ARCHITECTURES "${CMAKE_HOST_SYSTEM_PROCESSOR}")

# ---------------------------------------------------------------------------------------
# Snapshot: renders the real editor component to a PNG (for design review / documentation).
#   cmake --build build --target ApolloSnapshot
# ---------------------------------------------------------------------------------------
juce_add_console_app(ApolloSnapshot PRODUCT_NAME "ApolloSnapshot")
target_sources(ApolloSnapshot PRIVATE Tests/Snapshot.cpp Source/PluginProcessor.cpp Source/PluginEditor.cpp)
target_include_directories(ApolloSnapshot PRIVATE Source Tests)
target_compile_definitions(ApolloSnapshot PRIVATE JucePlugin_Name="${APOLLO_PLUGIN_NAME}" JUCE_MODAL_LOOPS_PERMITTED=1 ${PANTHEON_JUCE_DEFINITIONS})
target_link_libraries(ApolloSnapshot
    PRIVATE
        pantheon_ui
        pantheon_dsp
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags)
set_target_properties(ApolloSnapshot PROPERTIES EXCLUDE_FROM_ALL TRUE OSX_ARCHITECTURES "${CMAKE_HOST_SYSTEM_PROCESSOR}")

# ---------------------------------------------------------------------------------------
# UI check: the editor built on the real processor (parameters, solo and delta buttons).
#   cmake --build build --target ApolloUiCheck
# ---------------------------------------------------------------------------------------
juce_add_console_app(ApolloUiCheck PRODUCT_NAME "ApolloUiCheck")
target_sources(ApolloUiCheck PRIVATE Tests/UiCheck.cpp Source/PluginProcessor.cpp Source/PluginEditor.cpp)
target_include_directories(ApolloUiCheck PRIVATE Source Tests)
target_compile_definitions(ApolloUiCheck PRIVATE JucePlugin_Name="${APOLLO_PLUGIN_NAME}" JUCE_MODAL_LOOPS_PERMITTED=1 ${PANTHEON_JUCE_DEFINITIONS})
target_link_libraries(ApolloUiCheck
    PRIVATE
        pantheon_ui
        pantheon_dsp
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags)
set_target_properties(ApolloUiCheck PROPERTIES EXCLUDE_FROM_ALL TRUE OSX_ARCHITECTURES "${CMAKE_HOST_SYSTEM_PROCESSOR}")

