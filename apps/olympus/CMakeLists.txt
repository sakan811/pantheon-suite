# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 The Pantheon authors

cmake_minimum_required(VERSION 3.22)

# Universal binary so the app runs natively on both kinds of Mac.
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "")
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")

project(Olympus VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# JUCE is not vendored: cmake/Juce.cmake fetches it (and owns the version).
include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/Juce.cmake) # JUCE, one version for the repository

# The interface kit shared with the plugins and the other app.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../shared/ui ${CMAKE_BINARY_DIR}/shared/ui)

# Olympus hosts the suite's plugins: it loads built VST3 bundles at run time, and links none of them.
# Every Olympus target needs the plugin-hosting switches; the rest are the repository's own (cmake/Juce.cmake).
set(OLYMPUS_DEFINITIONS ${PANTHEON_JUCE_DEFINITIONS} JUCE_PLUGINHOST_VST3=1 JUCE_PLUGINHOST_LADSPA=0 JUCE_MODAL_LOOPS_PERMITTED=1)
set(OLYMPUS_LIBRARIES juce::juce_audio_utils juce::juce_audio_processors juce::juce_audio_formats)

# ---------------------------------------------------------------------------------------
# The app (standalone, not a plugin)
# ---------------------------------------------------------------------------------------
juce_add_gui_app(Olympus
    COMPANY_NAME "The Pantheon authors"
    BUNDLE_ID org.pantheon.olympus
    PRODUCT_NAME "Olympus"
    DOCUMENT_EXTENSIONS olympus
    DESCRIPTION "Session host for the Pantheon plugin suite")

target_sources(Olympus PRIVATE
    Source/Main.cpp
    Source/UI/MainComponent.cpp)
target_include_directories(Olympus PRIVATE Source)
target_compile_definitions(Olympus PRIVATE
    ${OLYMPUS_DEFINITIONS}
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:Olympus,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="${PROJECT_VERSION}")
target_link_libraries(Olympus
    PRIVATE
        pantheon_ui
        ${OLYMPUS_LIBRARIES}
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# ---------------------------------------------------------------------------------------
# The MCP server: the same session without a window, speaking MCP on stdin and stdout (what an agent starts).
#   cmake --build build --target OlympusMcp
# ---------------------------------------------------------------------------------------
juce_add_console_app(OlympusMcp PRODUCT_NAME "OlympusMcp")
target_sources(OlympusMcp PRIVATE Source/McpMain.cpp)
target_include_directories(OlympusMcp PRIVATE Source)
target_compile_definitions(OlympusMcp PRIVATE ${OLYMPUS_DEFINITIONS})
target_link_libraries(OlympusMcp
    PRIVATE ${OLYMPUS_LIBRARIES}
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# ---------------------------------------------------------------------------------------
# Headless tests of the session, the render, the project file and the server (no window, no audio device, no built plugin):
#   cmake --build build --target OlympusTests
# ---------------------------------------------------------------------------------------
juce_add_console_app(OlympusTests PRODUCT_NAME "OlympusTests")
target_sources(OlympusTests PRIVATE Tests/Tests.cpp)
target_include_directories(OlympusTests PRIVATE Source Tests)
target_compile_definitions(OlympusTests PRIVATE ${OLYMPUS_DEFINITIONS} PANTHEON_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../..")
target_link_libraries(OlympusTests
    PRIVATE ${OLYMPUS_LIBRARIES}
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
set_target_properties(OlympusTests PROPERTIES EXCLUDE_FROM_ALL TRUE OSX_ARCHITECTURES "${CMAKE_HOST_SYSTEM_PROCESSOR}")

# ---------------------------------------------------------------------------------------
# UI check: the window driven without a screen (stems, chains, plugin windows, projects, render, notes, layout).
#   cmake --build build --target OlympusUiCheck
# ---------------------------------------------------------------------------------------
juce_add_console_app(OlympusUiCheck PRODUCT_NAME "OlympusUiCheck")
target_sources(OlympusUiCheck PRIVATE Tests/UiCheck.cpp Source/UI/MainComponent.cpp)
target_include_directories(OlympusUiCheck PRIVATE Source Tests)
target_compile_definitions(OlympusUiCheck PRIVATE ${OLYMPUS_DEFINITIONS})
target_link_libraries(OlympusUiCheck
    PRIVATE
        pantheon_ui
        ${OLYMPUS_LIBRARIES}
    PUBLIC
        juce::juce_recommended_config_flags)
set_target_properties(OlympusUiCheck PROPERTIES EXCLUDE_FROM_ALL TRUE OSX_ARCHITECTURES "${CMAKE_HOST_SYSTEM_PROCESSOR}")

# ---------------------------------------------------------------------------------------
# Snapshot: renders the real window to a PNG over four synthetic stems, for design review and the README.
#   cmake --build build --target OlympusSnapshot
# ---------------------------------------------------------------------------------------
juce_add_console_app(OlympusSnapshot PRODUCT_NAME "OlympusSnapshot")
target_sources(OlympusSnapshot PRIVATE Tests/Snapshot.cpp Source/UI/MainComponent.cpp)
target_include_directories(OlympusSnapshot PRIVATE Source Tests)
target_compile_definitions(OlympusSnapshot PRIVATE ${OLYMPUS_DEFINITIONS})
target_link_libraries(OlympusSnapshot
    PRIVATE
        pantheon_ui
        ${OLYMPUS_LIBRARIES}
    PUBLIC
        juce::juce_recommended_config_flags)
set_target_properties(OlympusSnapshot PROPERTIES EXCLUDE_FROM_ALL TRUE OSX_ARCHITECTURES "${CMAKE_HOST_SYSTEM_PROCESSOR}")
