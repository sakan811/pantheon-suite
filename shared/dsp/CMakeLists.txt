# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 The Pantheon authors

# DSP that more than one plugin uses: double-precision biquads (with the parametric-EQ designs), the six main bands of music and
# their splitter (MusicBands.h), and the lock-free taps and the
# FFT analyzer behind the spectrum displays. Header-only, namespace `pdsp`, and nothing from any plugin. The analyzer needs
# JUCE's dsp module (its FFT); the biquads need nothing.
add_library(pantheon_dsp INTERFACE)
target_include_directories(pantheon_dsp INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(pantheon_dsp INTERFACE cxx_std_17)
target_link_libraries(pantheon_dsp INTERFACE juce::juce_dsp juce::juce_audio_basics)
