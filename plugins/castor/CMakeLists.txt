# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 The Pantheon authors

cmake_minimum_required(VERSION 3.22)

# Universal binary so the plugin loads whether the host runs natively or under Rosetta.
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "")
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")

project(Castor VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(CASTOR_INSTALL_AFTER_BUILD "Copy the plugin into the user plugin folders after building (macOS)" OFF)

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
set(CASTOR_PLUGIN_NAME "Castor Pair")

juce_add_plugin(Castor
    COMPANY_NAME "Castor Audio"
    BUNDLE_ID com.castoraudio.castor
    PLUGIN_MANUFACTURER_CODE Cast
    PLUGIN_CODE Cspr
    FORMATS VST3 AU Standalone
    PRODUCT_NAME "${CASTOR_PLUGIN_NAME}"
    PLUGIN_NAME "${CASTOR_PLUGIN_NAME}"
    DESCRIPTION "Stereo imager: width in six bands of music, mid and side level, mono bass, balance, correlation meter and goniometer"
    VST3_CATEGORIES Fx Stereo
    AU_MAIN_TYPE kAudioUnitType_Effect
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    COPY_PLUGIN_AFTER_BUILD ${CASTOR_INSTALL_AFTER_BUILD})

target_sources(Castor PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

target_compile_definitions(Castor PUBLIC ${PANTHEON_JUCE_DEFINITIONS})

target_link_libraries(Castor
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
# Headless DSP tests (no plugin, no GUI):  cmake --build build --target CastorTests
# ---------------------------------------------------------------------------------------
juce_add_console_app(CastorTests PRODUCT_NAME "CastorTests")
target_sources(CastorTests PRIVATE Tests/Tests.cpp)
target_include_directories(CastorTests PRIVATE Source Tests)
target_link_libraries(CastorTests
    PRIVATE
        pantheon_dsp
        juce::juce_dsp
        juce::juce_audio_basics
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
set_target_properties(CastorTests PROPERTIES EXCLUDE_FROM_ALL TRUE OSX_ARCHITECTURES "${CMAKE_HOST_SYSTEM_PROCESSOR}")

# ---------------------------------------------------------------------------------------
# Snapshot: renders the real editor component to a PNG (for design review / documentation).
#   cmake --build build --target CastorSnapshot
# ---------------------------------------------------------------------------------------
juce_add_console_app(CastorSnapshot PRODUCT_NAME "CastorSnapshot")
target_sources(CastorSnapshot PRIVATE Tests/Snapshot.cpp Source/PluginProcessor.cpp Source/PluginEditor.cpp)
target_include_directories(CastorSnapshot PRIVATE Source Tests)
target_compile_definitions(CastorSnapshot PRIVATE JucePlugin_Name="${CASTOR_PLUGIN_NAME}" JUCE_MODAL_LOOPS_PERMITTED=1 ${PANTHEON_JUCE_DEFINITIONS})
target_link_libraries(CastorSnapshot
    PRIVATE
        pantheon_ui
        pantheon_dsp
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags)
set_target_properties(CastorSnapshot PROPERTIES EXCLUDE_FROM_ALL TRUE OSX_ARCHITECTURES "${CMAKE_HOST_SYSTEM_PROCESSOR}")

# ---------------------------------------------------------------------------------------
# UI check: the editor built on the real processor (parameters, solo and delta buttons).
#   cmake --build build --target CastorUiCheck
# ---------------------------------------------------------------------------------------
juce_add_console_app(CastorUiCheck PRODUCT_NAME "CastorUiCheck")
target_sources(CastorUiCheck PRIVATE Tests/UiCheck.cpp Source/PluginProcessor.cpp Source/PluginEditor.cpp)
target_include_directories(CastorUiCheck PRIVATE Source Tests)
target_compile_definitions(CastorUiCheck PRIVATE JucePlugin_Name="${CASTOR_PLUGIN_NAME}" JUCE_MODAL_LOOPS_PERMITTED=1 ${PANTHEON_JUCE_DEFINITIONS})
target_link_libraries(CastorUiCheck
    PRIVATE
        pantheon_ui
        pantheon_dsp
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags)
set_target_properties(CastorUiCheck PROPERTIES EXCLUDE_FROM_ALL TRUE OSX_ARCHITECTURES "${CMAKE_HOST_SYSTEM_PROCESSOR}")

