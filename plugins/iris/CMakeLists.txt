# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 The Pantheon authors

cmake_minimum_required(VERSION 3.22)

# Universal binary so the plugin loads whether the host runs natively or under Rosetta.
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "")
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")

project(Iris VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(IRIS_INSTALL_AFTER_BUILD "Copy the plugin into the user plugin folders after building (macOS)" OFF)

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
set(IRIS_PLUGIN_NAME "Iris Prism")

juce_add_plugin(Iris
    COMPANY_NAME "Iris Audio"
    BUNDLE_ID com.irisaudio.iris
    PLUGIN_MANUFACTURER_CODE Irsa
    PLUGIN_CODE Irsp
    FORMATS VST3 AU Standalone
    PRODUCT_NAME "${IRIS_PLUGIN_NAME}"
    PLUGIN_NAME "${IRIS_PLUGIN_NAME}"
    DESCRIPTION "Dynamic equaliser with mid/side, clean and saturated modes, and solo for bands and frequency regions"
    VST3_CATEGORIES Fx EQ
    AU_MAIN_TYPE kAudioUnitType_Effect
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    COPY_PLUGIN_AFTER_BUILD ${IRIS_INSTALL_AFTER_BUILD})

target_sources(Iris PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

target_compile_definitions(Iris PUBLIC ${PANTHEON_JUCE_DEFINITIONS})

target_link_libraries(Iris
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
# Headless DSP tests (no plugin, no GUI):  cmake --build build --target IrisTests
# ---------------------------------------------------------------------------------------
juce_add_console_app(IrisTests PRODUCT_NAME "IrisTests")
target_sources(IrisTests PRIVATE Tests/Tests.cpp)
target_include_directories(IrisTests PRIVATE Source Tests)
target_link_libraries(IrisTests
    PRIVATE
        pantheon_dsp
        juce::juce_dsp
        juce::juce_audio_basics
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
set_target_properties(IrisTests PROPERTIES EXCLUDE_FROM_ALL TRUE OSX_ARCHITECTURES "${CMAKE_HOST_SYSTEM_PROCESSOR}")

# ---------------------------------------------------------------------------------------
# Snapshot: renders the real editor component to a PNG (for design review / documentation).
#   cmake --build build --target IrisSnapshot
# ---------------------------------------------------------------------------------------
juce_add_console_app(IrisSnapshot PRODUCT_NAME "IrisSnapshot")
target_sources(IrisSnapshot PRIVATE Tests/Snapshot.cpp Source/PluginProcessor.cpp Source/PluginEditor.cpp)
target_include_directories(IrisSnapshot PRIVATE Source Tests)
target_compile_definitions(IrisSnapshot PRIVATE JucePlugin_Name="${IRIS_PLUGIN_NAME}" JUCE_MODAL_LOOPS_PERMITTED=1 ${PANTHEON_JUCE_DEFINITIONS})
target_link_libraries(IrisSnapshot
    PRIVATE
        pantheon_ui
        pantheon_dsp
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags)
set_target_properties(IrisSnapshot PROPERTIES EXCLUDE_FROM_ALL TRUE OSX_ARCHITECTURES "${CMAKE_HOST_SYSTEM_PROCESSOR}")

# ---------------------------------------------------------------------------------------
# UI check: drives the display with synthetic mouse events (nodes, wheel, region solo, crossovers) and the editor's panels.
#   cmake --build build --target IrisUiCheck
# ---------------------------------------------------------------------------------------
juce_add_console_app(IrisUiCheck PRODUCT_NAME "IrisUiCheck")
target_sources(IrisUiCheck PRIVATE Tests/UiCheck.cpp Source/PluginProcessor.cpp Source/PluginEditor.cpp)
target_include_directories(IrisUiCheck PRIVATE Source Tests)
target_compile_definitions(IrisUiCheck PRIVATE JucePlugin_Name="${IRIS_PLUGIN_NAME}" JUCE_MODAL_LOOPS_PERMITTED=1 ${PANTHEON_JUCE_DEFINITIONS})
target_link_libraries(IrisUiCheck
    PRIVATE
        pantheon_ui
        pantheon_dsp
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags)
set_target_properties(IrisUiCheck PROPERTIES EXCLUDE_FROM_ALL TRUE OSX_ARCHITECTURES "${CMAKE_HOST_SYSTEM_PROCESSOR}")

# ---------------------------------------------------------------------------------------
# Host check: loads the built VST3 like a DAW would and fuzzes it.
#   cmake --build build --target IrisHostCheck
# ---------------------------------------------------------------------------------------
juce_add_console_app(IrisHostCheck PRODUCT_NAME "IrisHostCheck")
target_sources(IrisHostCheck PRIVATE Tests/HostCheck.cpp)
target_include_directories(IrisHostCheck PRIVATE Tests)
target_compile_definitions(IrisHostCheck PRIVATE
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_AU=0
    JUCE_MODAL_LOOPS_PERMITTED=1
    ${PANTHEON_JUCE_DEFINITIONS})
target_link_libraries(IrisHostCheck
    PRIVATE
        juce::juce_audio_processors
        juce::juce_gui_basics
    PUBLIC
        juce::juce_recommended_config_flags)
set_target_properties(IrisHostCheck PROPERTIES EXCLUDE_FROM_ALL TRUE OSX_ARCHITECTURES "${CMAKE_HOST_SYSTEM_PROCESSOR}")
