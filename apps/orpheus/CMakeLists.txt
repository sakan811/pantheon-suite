# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 The Pantheon authors

cmake_minimum_required(VERSION 3.22)

# Universal binary so the app runs natively on both kinds of Mac.
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "")
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")

project(Orpheus VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# JUCE is not vendored: cmake/Juce.cmake fetches it (and owns the version).
include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/Juce.cmake) # JUCE, one version for the repository

# The interface kit shared with the plugins. (Orpheus needs none of shared/dsp: its signal processing is its own, in Source/Dsp.)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../shared/ui ${CMAKE_BINARY_DIR}/shared/ui)

# ---------------------------------------------------------------------------------------
# The app (standalone, not a plugin)
# ---------------------------------------------------------------------------------------
juce_add_gui_app(Orpheus
    COMPANY_NAME "The Pantheon authors"
    BUNDLE_ID org.pantheon.orpheus
    PRODUCT_NAME "Orpheus"
    DOCUMENT_EXTENSIONS orpheus
    DESCRIPTION "Offline pitch editor for solo vocals")

target_sources(Orpheus PRIVATE
    Source/Main.cpp
    Source/UI/MainComponent.cpp)

target_compile_definitions(Orpheus PRIVATE
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:Orpheus,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="${PROJECT_VERSION}")

target_link_libraries(Orpheus
    PRIVATE
        pantheon_ui
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# ---------------------------------------------------------------------------------------
# Headless tests of the signal processing (no GUI, no audio device):  cmake --build build --target OrpheusTests
# ---------------------------------------------------------------------------------------
juce_add_console_app(OrpheusTests PRODUCT_NAME "OrpheusTests")
target_sources(OrpheusTests PRIVATE Tests/Tests.cpp)
target_include_directories(OrpheusTests PRIVATE Source Tests)
target_link_libraries(OrpheusTests
    PRIVATE
        juce::juce_core
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
set_target_properties(OrpheusTests PROPERTIES EXCLUDE_FROM_ALL TRUE OSX_ARCHITECTURES "${CMAKE_HOST_SYSTEM_PROCESSOR}")

# ---------------------------------------------------------------------------------------
# UI check: the session (undo, project files, export) and the window driven with synthetic mouse events and keys.
#   cmake --build build --target OrpheusUiCheck
# ---------------------------------------------------------------------------------------
juce_add_console_app(OrpheusUiCheck PRODUCT_NAME "OrpheusUiCheck")
target_sources(OrpheusUiCheck PRIVATE Tests/UiCheck.cpp Source/UI/MainComponent.cpp)
target_include_directories(OrpheusUiCheck PRIVATE Source Tests)
target_compile_definitions(OrpheusUiCheck PRIVATE JUCE_MODAL_LOOPS_PERMITTED=1 ${PANTHEON_JUCE_DEFINITIONS})
target_link_libraries(OrpheusUiCheck
    PRIVATE
        pantheon_ui
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags)
set_target_properties(OrpheusUiCheck PROPERTIES EXCLUDE_FROM_ALL TRUE OSX_ARCHITECTURES "${CMAKE_HOST_SYSTEM_PROCESSOR}")

# ---------------------------------------------------------------------------------------
# Snapshot: renders the real window to a PNG, with a synthetic singer, for design review and the README.
#   cmake --build build --target OrpheusSnapshot
# ---------------------------------------------------------------------------------------
juce_add_console_app(OrpheusSnapshot PRODUCT_NAME "OrpheusSnapshot")
target_sources(OrpheusSnapshot PRIVATE Tests/Snapshot.cpp Source/UI/MainComponent.cpp)
target_include_directories(OrpheusSnapshot PRIVATE Source Tests)
target_compile_definitions(OrpheusSnapshot PRIVATE JUCE_MODAL_LOOPS_PERMITTED=1 ${PANTHEON_JUCE_DEFINITIONS})
target_link_libraries(OrpheusSnapshot
    PRIVATE
        pantheon_ui
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags)
set_target_properties(OrpheusSnapshot PROPERTIES EXCLUDE_FROM_ALL TRUE OSX_ARCHITECTURES "${CMAKE_HOST_SYSTEM_PROCESSOR}")
