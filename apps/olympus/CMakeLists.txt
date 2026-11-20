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
# Audio Units are hosted on macOS only (found through the system registry; see Source/Catalog.h).
if(APPLE)
    list(APPEND OLYMPUS_DEFINITIONS JUCE_PLUGINHOST_AU=1)
endif()

# ---------------------------------------------------------------------------------------
# The core: JUCE and the window code, compiled once and linked by every Olympus target below
# (before, each of the five targets compiled its own copy of JUCE and of the two UI files).
# Static, so each executable links only what it uses. JUCE_STANDALONE_APPLICATION is the define
# juce_add_gui_app / juce_add_console_app set on each executable; it is repeated here so the
# core and the executables compile JUCE with the same definitions.
# ---------------------------------------------------------------------------------------
# JUCE 8 builds each module as an INTERFACE library: every target that links a module compiles the module's .cpp
# files itself, so a static library in between would make each executable compile all of JUCE again. The function
# below moves those source lists into OlympusCore (compiled once) and empties the module targets of them; the modules'
# include directories and definitions still reach every executable through the PUBLIC link.
function(olympus_move_juce_sources core)
    set(todo ${ARGN})
    set(seen "")
    set(sources "")
    while(todo)
        list(POP_FRONT todo t)
        if(NOT TARGET ${t})
            continue()
        endif()
        get_target_property(real ${t} ALIASED_TARGET)
        if(real)
            set(t ${real})
        endif()
        if(t IN_LIST seen)
            continue()
        endif()
        list(APPEND seen ${t})
        get_target_property(links ${t} INTERFACE_LINK_LIBRARIES)
        if(links)
            list(APPEND todo ${links})
        endif()
        get_target_property(srcs ${t} INTERFACE_SOURCES)
        if(srcs)
            list(APPEND sources ${srcs})
            set_property(TARGET ${t} PROPERTY INTERFACE_SOURCES "")
        endif()
    endwhile()
    target_sources(${core} PRIVATE ${sources})
endfunction()

add_library(OlympusCore STATIC)
target_sources(OlympusCore PRIVATE
    Source/UI/MainComponent.cpp
    Source/UI/Arrange.cpp)
target_include_directories(OlympusCore PUBLIC Source)
target_compile_definitions(OlympusCore PUBLIC ${OLYMPUS_DEFINITIONS} JUCE_STANDALONE_APPLICATION=1)
target_link_libraries(OlympusCore
    PUBLIC
        pantheon_ui
        ${OLYMPUS_LIBRARIES}
        juce::juce_recommended_config_flags
    PRIVATE
        juce::juce_recommended_warning_flags) # the app compiled its window code with these; the core is that code now
# Compile JUCE once (about 3x faster cold build on Linux). Off by default on macOS, where JUCE's Objective-C++ module files have not been tried here:
# -DOLYMPUS_SHARE_JUCE_BUILD=ON turns it on there, OFF turns it off anywhere (every target then compiles JUCE itself, as before).
if(APPLE)
    option(OLYMPUS_SHARE_JUCE_BUILD "Compile the JUCE modules once, in OlympusCore" OFF)
else()
    option(OLYMPUS_SHARE_JUCE_BUILD "Compile the JUCE modules once, in OlympusCore" ON)
endif()
if(OLYMPUS_SHARE_JUCE_BUILD)
    olympus_move_juce_sources(OlympusCore pantheon_ui ${OLYMPUS_LIBRARIES})
endif()

# ---------------------------------------------------------------------------------------
# The app (standalone, not a plugin)
# ---------------------------------------------------------------------------------------
juce_add_gui_app(Olympus
    COMPANY_NAME "The Pantheon authors"
    BUNDLE_ID org.pantheon.olympus
    PRODUCT_NAME "Olympus"
    DOCUMENT_EXTENSIONS olympus
    DESCRIPTION "Session host for the Pantheon plugin suite")

target_sources(Olympus PRIVATE Source/Main.cpp)
target_compile_definitions(Olympus PRIVATE
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:Olympus,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="${PROJECT_VERSION}")
target_link_libraries(Olympus
    PRIVATE OlympusCore
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
target_link_libraries(OlympusMcp
    PRIVATE OlympusCore
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
target_include_directories(OlympusTests PRIVATE Tests)
target_compile_definitions(OlympusTests PRIVATE PANTHEON_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../..")
target_link_libraries(OlympusTests
    PRIVATE OlympusCore
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
set_target_properties(OlympusTests PROPERTIES EXCLUDE_FROM_ALL TRUE OSX_ARCHITECTURES "${CMAKE_HOST_SYSTEM_PROCESSOR}")

# ---------------------------------------------------------------------------------------
# UI check: the window driven without a screen (stems, chains, plugin windows, projects, render, notes, layout).
#   cmake --build build --target OlympusUiCheck
# ---------------------------------------------------------------------------------------
juce_add_console_app(OlympusUiCheck PRODUCT_NAME "OlympusUiCheck")
target_sources(OlympusUiCheck PRIVATE Tests/UiCheck.cpp)
target_include_directories(OlympusUiCheck PRIVATE Tests)
target_link_libraries(OlympusUiCheck
    PRIVATE OlympusCore
    PUBLIC
        juce::juce_recommended_config_flags)
set_target_properties(OlympusUiCheck PROPERTIES EXCLUDE_FROM_ALL TRUE OSX_ARCHITECTURES "${CMAKE_HOST_SYSTEM_PROCESSOR}")

# ---------------------------------------------------------------------------------------
# Snapshot: renders the real window to a PNG over four synthetic stems, for design review and the README.
#   cmake --build build --target OlympusSnapshot
# ---------------------------------------------------------------------------------------
juce_add_console_app(OlympusSnapshot PRODUCT_NAME "OlympusSnapshot")
target_sources(OlympusSnapshot PRIVATE Tests/Snapshot.cpp)
target_include_directories(OlympusSnapshot PRIVATE Tests)
target_link_libraries(OlympusSnapshot
    PRIVATE OlympusCore
    PUBLIC
        juce::juce_recommended_config_flags)
set_target_properties(OlympusSnapshot PROPERTIES EXCLUDE_FROM_ALL TRUE OSX_ARCHITECTURES "${CMAKE_HOST_SYSTEM_PROCESSOR}")
