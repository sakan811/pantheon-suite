# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 The Pantheon authors

cmake_minimum_required(VERSION 3.22)

# These must be set before project(): universal (Apple Silicon + Intel) binaries, macOS 11+.
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Universal binary: Apple Silicon + Intel")

project(Themis VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(THEMIS_BUILD_PLUGIN "Build the JUCE plugin (VST3 / AU / Standalone)" ON)
option(THEMIS_BUILD_TESTS "Build the DSP unit tests" ON)
option(THEMIS_BUILD_SMOKE "Build the headless JUCE smoke test (tests/plugin_smoke.cpp)" OFF)
option(THEMIS_INSTALL_AFTER_BUILD "Copy the plugin into the user plugin folders after building (macOS)" ${APPLE})

# ---- DSP core: header-only, no dependencies ---------------------------------------------------
add_library(themis_core INTERFACE)
target_include_directories(themis_core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/core/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../shared/dsp/include) # MusicBands.h and Biquad.h: plain C++, no JUCE
target_compile_features(themis_core INTERFACE cxx_std_17)

if(THEMIS_BUILD_TESTS)
    enable_testing()
    add_executable(themis_tests tests/test_main.cpp)
    target_link_libraries(themis_tests PRIVATE themis_core)
    add_test(NAME themis_dsp COMMAND themis_tests)
endif()

# ---- Plugin -------------------------------------------------------------------------------------
if(THEMIS_BUILD_PLUGIN)
    include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/Juce.cmake) # JUCE, one version for the repository

    # The interface kit shared by the plugins of this repository (shared/ui), which needs JUCE's GUI module.
    if(NOT TARGET pantheon_ui)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../shared/ui ${CMAKE_BINARY_DIR}/shared/ui)
    endif()
    # The shared DSP (shared/dsp): Themis uses only its band hand-off format (BandLink.h), to copy a fix to the equaliser.
    if(NOT TARGET pantheon_dsp)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../shared/dsp ${CMAKE_BINARY_DIR}/shared/dsp)
    endif()

    set(THEMIS_FORMATS VST3 Standalone)
    if(APPLE)
        list(APPEND THEMIS_FORMATS AU)
    endif()

    # The plugin's name, written once. JUCE turns PLUGIN_NAME into JucePlugin_Name, which the processor reports to the host;
    # the smoke test is not a plugin target, so it gets the same value as a definition.
    set(THEMIS_PLUGIN_NAME "Themis Verdict")

    juce_add_plugin(Themis
        COMPANY_NAME                "Themis Audio"
        BUNDLE_ID                   com.themisaudio.themis
        PRODUCT_NAME                "${THEMIS_PLUGIN_NAME}"
        PLUGIN_NAME                 "${THEMIS_PLUGIN_NAME}"
        DESCRIPTION                 "Mastering check suite (audio passes through unchanged)"
        FORMATS                     ${THEMIS_FORMATS}
        IS_SYNTH                    FALSE
        NEEDS_MIDI_INPUT            FALSE
        NEEDS_MIDI_OUTPUT           FALSE
        IS_MIDI_EFFECT              FALSE
        EDITOR_WANTS_KEYBOARD_FOCUS FALSE
        COPY_PLUGIN_AFTER_BUILD     ${THEMIS_INSTALL_AFTER_BUILD}
        PLUGIN_MANUFACTURER_CODE    Thmi
        PLUGIN_CODE                 Thms
        AU_MAIN_TYPE                kAudioUnitType_Effect
        VST3_CATEGORIES             Fx Analyzer
        HARDENED_RUNTIME_ENABLED    TRUE)

    target_sources(Themis PRIVATE
        plugin/Source/Parameters.cpp
        plugin/Source/PluginProcessor.cpp
        plugin/Source/PluginEditor.cpp)

    target_compile_definitions(Themis PUBLIC
        ${PANTHEON_JUCE_DEFINITIONS}
        JUCE_STRICT_REFCOUNTEDPOINTER=1)

    target_link_libraries(Themis
        PRIVATE
            themis_core
            pantheon_ui
            pantheon_dsp
            juce::juce_audio_utils
            juce::juce_audio_formats
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags
            juce::juce_recommended_warning_flags)

    if(THEMIS_BUILD_SMOKE)
        juce_add_console_app(ThemisSmoke PRODUCT_NAME "ThemisSmoke")
        target_sources(ThemisSmoke PRIVATE
            tests/plugin_smoke.cpp
            plugin/Source/Parameters.cpp
            plugin/Source/PluginProcessor.cpp
            plugin/Source/PluginEditor.cpp)
        target_compile_definitions(ThemisSmoke PRIVATE JucePlugin_Name="${THEMIS_PLUGIN_NAME}" ${PANTHEON_JUCE_DEFINITIONS} JUCE_MODAL_LOOPS_PERMITTED=1)
        target_link_libraries(ThemisSmoke
            PRIVATE themis_core pantheon_ui pantheon_dsp juce::juce_audio_utils juce::juce_audio_formats
            PUBLIC juce::juce_recommended_config_flags)
    endif()
endif()
