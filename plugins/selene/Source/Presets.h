// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "Parameters.h"
#include <pantheon/ui/Presets.h>

// Curated City Pop starting points for Selene Poly. A preset resets every parameter to its default first, then sets the values listed here.
namespace selene
{
inline const std::vector<ui::Preset>& presets()
{
    static const std::vector<ui::Preset> list
    {
        { "Init", {},
          "Clean dual-saw starting point with 24 dB low-pass filter, gentle Mode I chorus, and subtle tape echo." },

        { "Tokyo Night Pad",
          { { id::osc1Wave, 1.0f }, { id::osc1Pwm, 0.35f }, { id::subLevel, 0.45f }, { id::osc2Wave, 0.0f }, { id::osc2Detune, 14.0f },
            { id::oscMix, 0.50f }, { id::cutoff, 1850.0f }, { id::resonance, 0.22f }, { id::filterEnvAmt, 0.25f }, { id::drive, 0.20f },
            { id::ampAttack, 380.0f }, { id::ampDecay, 900.0f }, { id::ampSustain, 0.85f }, { id::ampRelease, 1100.0f },
            { id::fltAttack, 650.0f }, { id::fltDecay, 1400.0f }, { id::fltSustain, 0.40f }, { id::fltRelease, 1200.0f },
            { id::lfoRate, 0.85f }, { id::lfoPwm, 0.45f }, { id::lfoFilter, 0.12f },
            { id::chorusMode, 3.0f }, { id::echoTime, 380.0f }, { id::echoFeedback, 0.35f }, { id::echoMix, 0.22f } },
          "Lush late-night City Pop chord pad with PWM shimmer, Chorus I+II, and wide tape echo tails." },

        { "Sunset Brass",
          { { id::osc1Wave, 0.0f }, { id::subLevel, 0.30f }, { id::osc2Wave, 0.0f }, { id::osc2Detune, 11.0f }, { id::oscMix, 0.50f },
            { id::cutoff, 1400.0f }, { id::resonance, 0.24f }, { id::filterEnvAmt, 0.62f }, { id::drive, 0.25f },
            { id::ampAttack, 18.0f }, { id::ampDecay, 350.0f }, { id::ampSustain, 0.82f }, { id::ampRelease, 180.0f },
            { id::fltAttack, 45.0f }, { id::fltDecay, 420.0f }, { id::fltSustain, 0.30f }, { id::fltRelease, 220.0f },
            { id::lfoRate, 5.2f }, { id::lfoPitch, 6.0f }, { id::chorusMode, 1.0f }, { id::echoMix, 0.14f } },
          "Warm 80s poly synth brass with blooming filter attack and gentle vibrato for bright horn stabs." },

        { "Shibuya Lead",
          { { id::voiceMode, 1.0f }, { id::glide, 65.0f }, { id::osc1Wave, 0.0f }, { id::subLevel, 0.20f }, { id::osc2Wave, 1.0f },
            { id::osc2Detune, 6.0f }, { id::oscMix, 0.45f }, { id::cutoff, 4200.0f }, { id::resonance, 0.30f }, { id::filterEnvAmt, 0.40f },
            { id::drive, 0.30f }, { id::ampAttack, 4.0f }, { id::ampSustain, 0.90f }, { id::ampRelease, 140.0f },
            { id::fltAttack, 8.0f }, { id::fltDecay, 260.0f }, { id::fltSustain, 0.45f },
            { id::lfoRate, 5.8f }, { id::lfoPitch, 14.0f }, { id::chorusMode, 2.0f }, { id::echoTime, 310.0f }, { id::echoFeedback, 0.32f }, { id::echoMix, 0.20f } },
          "Expressive mono glide lead with singing resonance, vibrato, and stereo tape delay." },

        { "City FM EP",
          { { id::osc1Wave, 3.0f }, { id::subLevel, 0.10f }, { id::osc2Wave, 3.0f }, { id::osc2Semi, 12.0f }, { id::osc2Detune, 3.0f },
            { id::osc2Fm, 0.55f }, { id::oscMix, 0.15f }, { id::filterMode, 1.0f }, { id::cutoff, 5200.0f }, { id::resonance, 0.08f },
            { id::filterEnvAmt, 0.35f }, { id::ampAttack, 2.0f }, { id::ampDecay, 1200.0f }, { id::ampSustain, 0.18f }, { id::ampRelease, 320.0f },
            { id::fltAttack, 2.0f }, { id::fltDecay, 320.0f }, { id::fltSustain, 0.15f },
            { id::chorusMode, 1.0f }, { id::echoTime, 340.0f }, { id::echoFeedback, 0.25f }, { id::echoMix, 0.15f } },
          "Glassy FM electric piano using Osc 2 FM tine bark and classic Mode I stereo chorus." },

        { "Slap Synth Bass",
          { { id::voiceMode, 1.0f }, { id::glide, 18.0f }, { id::osc1Wave, 0.0f }, { id::subLevel, 0.70f }, { id::osc2Wave, 1.0f },
            { id::osc2Detune, 5.0f }, { id::oscMix, 0.35f }, { id::cutoff, 320.0f }, { id::resonance, 0.38f }, { id::filterEnvAmt, 0.78f },
            { id::keytrack, 0.30f }, { id::drive, 0.35f }, { id::ampAttack, 2.0f }, { id::ampDecay, 220.0f }, { id::ampSustain, 0.45f }, { id::ampRelease, 80.0f },
            { id::fltAttack, 1.0f }, { id::fltDecay, 160.0f }, { id::fltSustain, 0.08f }, { id::fltRelease, 90.0f },
            { id::chorusMode, 0.0f }, { id::echoMix, 0.0f } },
          "Punchy dry mono synth bass with heavy sub oscillator and snappy filter pop for tight grooves." },

        { "Neon Arp Pluck",
          { { id::osc1Wave, 1.0f }, { id::osc1Pwm, 0.28f }, { id::subLevel, 0.20f }, { id::osc2Wave, 0.0f }, { id::osc2Semi, 12.0f },
            { id::osc2Detune, -6.0f }, { id::oscMix, 0.40f }, { id::cutoff, 720.0f }, { id::resonance, 0.26f }, { id::filterEnvAmt, 0.68f },
            { id::ampAttack, 2.0f }, { id::ampDecay, 260.0f }, { id::ampSustain, 0.0f }, { id::ampRelease, 160.0f },
            { id::fltAttack, 1.0f }, { id::fltDecay, 190.0f }, { id::fltSustain, 0.0f }, { id::fltRelease, 140.0f },
            { id::chorusMode, 1.0f }, { id::echoTime, 240.0f }, { id::echoFeedback, 0.42f }, { id::echoMix, 0.28f } },
          "Crisp rhythmic City Pop arpeggio pluck with octave Osc 2 and dotted-feel stereo tape echo." },

        { "Velvet Strings",
          { { id::voiceMode, 2.0f }, { id::drift, 0.35f }, { id::osc1Wave, 0.0f }, { id::subLevel, 0.25f }, { id::osc2Wave, 0.0f },
            { id::osc2Detune, 16.0f }, { id::oscMix, 0.50f }, { id::filterMode, 1.0f }, { id::cutoff, 4800.0f }, { id::resonance, 0.12f },
            { id::filterEnvAmt, 0.15f }, { id::ampAttack, 420.0f }, { id::ampDecay, 600.0f }, { id::ampSustain, 0.90f }, { id::ampRelease, 850.0f },
            { id::lfoRate, 4.8f }, { id::lfoPitch, 8.0f }, { id::chorusMode, 3.0f }, { id::echoMix, 0.18f }, { id::masterGain, -5.0f } },
          "Wide unison string ensemble with rich detuned saws and dual-rate Chorus I+II." },

        { "Midnight Bell",
          { { id::osc1Wave, 3.0f }, { id::subLevel, 0.0f }, { id::osc2Wave, 3.0f }, { id::osc2Semi, 19.0f }, { id::osc2Detune, 4.0f },
            { id::osc2Fm, 0.72f }, { id::oscMix, 0.20f }, { id::cutoff, 8800.0f }, { id::resonance, 0.10f }, { id::filterEnvAmt, 0.25f },
            { id::ampAttack, 2.0f }, { id::ampDecay, 1600.0f }, { id::ampSustain, 0.0f }, { id::ampRelease, 900.0f },
            { id::fltAttack, 1.0f }, { id::fltDecay, 550.0f }, { id::fltSustain, 0.10f },
            { id::chorusMode, 2.0f }, { id::echoTime, 420.0f }, { id::echoFeedback, 0.38f }, { id::echoMix, 0.30f } },
          "Sparkling FM fifth bell chime with long glassy decay and deep Mode II chorus." },

        { "Funky Clav",
          { { id::osc1Wave, 1.0f }, { id::osc1Pwm, 0.18f }, { id::subLevel, 0.15f }, { id::osc2Wave, 0.0f }, { id::osc2Fm, 0.22f },
            { id::oscMix, 0.35f }, { id::cutoff, 1100.0f }, { id::resonance, 0.34f }, { id::filterEnvAmt, 0.70f }, { id::drive, 0.28f },
            { id::ampAttack, 1.0f }, { id::ampDecay, 180.0f }, { id::ampSustain, 0.12f }, { id::ampRelease, 75.0f },
            { id::fltAttack, 1.0f }, { id::fltDecay, 130.0f }, { id::fltSustain, 0.05f },
            { id::chorusMode, 1.0f }, { id::echoMix, 0.05f } },
          "Biting narrow-pulse clavinet comping patch with fast filter snap for tight chord stabs." },

        // ---- Pads & Strings ----
        { "Harbor Twilight Pad",
          { { id::osc1Wave, 2.0f }, { id::subLevel, 0.35f }, { id::osc2Wave, 1.0f }, { id::osc2Semi, 12.0f }, { id::osc2Detune, -10.0f },
            { id::oscMix, 0.45f }, { id::filterMode, 1.0f }, { id::cutoff, 1450.0f }, { id::resonance, 0.16f }, { id::filterEnvAmt, 0.32f },
            { id::keytrack, 0.45f }, { id::drive, 0.12f },
            { id::ampAttack, 520.0f }, { id::ampDecay, 1100.0f }, { id::ampSustain, 0.82f }, { id::ampRelease, 1400.0f },
            { id::fltAttack, 800.0f }, { id::fltDecay, 1600.0f }, { id::fltSustain, 0.35f }, { id::fltRelease, 1500.0f },
            { id::lfoWave, 1.0f }, { id::lfoRate, 0.45f }, { id::lfoFilter, 0.16f }, { id::lfoPwm, 0.25f },
            { id::chorusMode, 2.0f }, { id::echoTime, 440.0f }, { id::echoFeedback, 0.40f }, { id::echoMix, 0.28f } },
          "Airy twilight pad blending triangle body, octave pulse shimmer, Mode II chorus, and wide echo." },

        { "Warm Tape Pad",
          { { id::drift, 0.55f }, { id::osc1Wave, 0.0f }, { id::subLevel, 0.40f }, { id::osc2Wave, 0.0f }, { id::osc2Detune, -13.0f },
            { id::oscMix, 0.50f }, { id::noiseLevel, 0.04f }, { id::cutoff, 1150.0f }, { id::resonance, 0.14f }, { id::filterEnvAmt, 0.22f },
            { id::drive, 0.32f }, { id::ampAttack, 460.0f }, { id::ampDecay, 950.0f }, { id::ampSustain, 0.86f }, { id::ampRelease, 1250.0f },
            { id::fltAttack, 720.0f }, { id::fltDecay, 1200.0f }, { id::fltSustain, 0.32f }, { id::fltRelease, 1300.0f },
            { id::lfoRate, 0.65f }, { id::lfoPitch, 5.0f }, { id::lfoFilter, 0.08f },
            { id::chorusMode, 1.0f }, { id::echoTime, 360.0f }, { id::echoFeedback, 0.36f }, { id::echoMix, 0.24f } },
          "Nostalgic analog-drifting dual-saw pad with saturated 24 dB filter warmth and Mode I chorus." },

        { "Starlight Sweep",
          { { id::osc1Wave, 1.0f }, { id::osc1Pwm, 0.30f }, { id::subLevel, 0.30f }, { id::osc2Wave, 0.0f }, { id::osc2Semi, 7.0f },
            { id::osc2Detune, 9.0f }, { id::oscMix, 0.38f }, { id::cutoff, 580.0f }, { id::resonance, 0.46f }, { id::filterEnvAmt, 0.72f },
            { id::keytrack, 0.55f }, { id::drive, 0.18f },
            { id::ampAttack, 600.0f }, { id::ampDecay, 1400.0f }, { id::ampSustain, 0.80f }, { id::ampRelease, 1600.0f },
            { id::fltAttack, 1450.0f }, { id::fltDecay, 2200.0f }, { id::fltSustain, 0.45f }, { id::fltRelease, 1700.0f },
            { id::lfoRate, 0.75f }, { id::lfoPwm, 0.50f }, { id::lfoFilter, 0.14f },
            { id::chorusMode, 3.0f }, { id::echoTime, 480.0f }, { id::echoFeedback, 0.45f }, { id::echoMix, 0.32f } },
          "Slow-blooming resonant filter sweep over fifth-tuned oscillators and Chorus I+II shimmer." },

        { "Breezy Ensemble",
          { { id::voiceMode, 2.0f }, { id::drift, 0.30f }, { id::osc1Wave, 0.0f }, { id::subLevel, 0.20f }, { id::osc2Wave, 1.0f },
            { id::osc2Semi, 12.0f }, { id::osc2Detune, 12.0f }, { id::oscMix, 0.42f }, { id::filterMode, 1.0f }, { id::cutoff, 3400.0f },
            { id::resonance, 0.18f }, { id::filterEnvAmt, 0.28f }, { id::drive, 0.14f },
            { id::ampAttack, 240.0f }, { id::ampDecay, 700.0f }, { id::ampSustain, 0.88f }, { id::ampRelease, 750.0f },
            { id::fltAttack, 320.0f }, { id::fltDecay, 900.0f }, { id::fltSustain, 0.42f }, { id::fltRelease, 800.0f },
            { id::lfoRate, 4.6f }, { id::lfoPitch, 7.0f }, { id::lfoPwm, 0.30f },
            { id::chorusMode, 3.0f }, { id::echoTime, 340.0f }, { id::echoFeedback, 0.28f }, { id::echoMix, 0.18f }, { id::masterGain, -5.5f } },
          "Wide unison string-synth ensemble with gentle 12 dB filter lift for breezy coastal chord voicings." },

        // ---- Brass & Stabs ----
        { "80s Horn Section",
          { { id::voiceMode, 2.0f }, { id::drift, 0.28f }, { id::osc1Wave, 0.0f }, { id::subLevel, 0.25f }, { id::osc2Wave, 0.0f },
            { id::osc2Detune, 14.0f }, { id::oscMix, 0.50f }, { id::cutoff, 1150.0f }, { id::resonance, 0.22f }, { id::filterEnvAmt, 0.68f },
            { id::keytrack, 0.55f }, { id::drive, 0.28f },
            { id::ampAttack, 22.0f }, { id::ampDecay, 420.0f }, { id::ampSustain, 0.80f }, { id::ampRelease, 210.0f }, { id::velAmp, 0.75f },
            { id::fltAttack, 55.0f }, { id::fltDecay, 500.0f }, { id::fltSustain, 0.32f }, { id::fltRelease, 240.0f }, { id::velFlt, 0.70f },
            { id::lfoRate, 5.4f }, { id::lfoPitch, 5.5f }, { id::chorusMode, 1.0f }, { id::echoTime, 280.0f }, { id::echoMix, 0.15f }, { id::masterGain, -5.5f } },
          "Fat stacked unison synth brass section with expressive velocity bloom and punchy stereo spread." },

        { "Poly Synth Stab",
          { { id::osc1Wave, 0.0f }, { id::subLevel, 0.35f }, { id::osc2Wave, 1.0f }, { id::osc2Detune, -9.0f }, { id::oscMix, 0.45f },
            { id::cutoff, 900.0f }, { id::resonance, 0.28f }, { id::filterEnvAmt, 0.76f }, { id::drive, 0.30f },
            { id::ampAttack, 2.0f }, { id::ampDecay, 240.0f }, { id::ampSustain, 0.15f }, { id::ampRelease, 110.0f },
            { id::fltAttack, 2.0f }, { id::fltDecay, 180.0f }, { id::fltSustain, 0.08f }, { id::fltRelease, 120.0f },
            { id::chorusMode, 1.0f }, { id::echoTime, 260.0f }, { id::echoFeedback, 0.24f }, { id::echoMix, 0.16f } },
          "Snappy polyphonic chord stab with tight envelope decay for upbeat funk and disco syncopation." },

        { "Neon Fanfare",
          { { id::osc1Wave, 0.0f }, { id::subLevel, 0.20f }, { id::osc2Wave, 0.0f }, { id::osc2Semi, 7.0f }, { id::osc2Detune, 7.0f },
            { id::oscMix, 0.42f }, { id::cutoff, 1650.0f }, { id::resonance, 0.24f }, { id::filterEnvAmt, 0.58f }, { id::drive, 0.24f },
            { id::ampAttack, 14.0f }, { id::ampDecay, 360.0f }, { id::ampSustain, 0.78f }, { id::ampRelease, 220.0f },
            { id::fltAttack, 28.0f }, { id::fltDecay, 380.0f }, { id::fltSustain, 0.34f }, { id::fltRelease, 220.0f },
            { id::lfoRate, 5.0f }, { id::lfoPitch, 6.0f }, { id::chorusMode, 3.0f },
            { id::echoTime, 310.0f }, { id::echoFeedback, 0.30f }, { id::echoMix, 0.20f } },
          "Bright fifth-stacked poly brass lead and stab voice with dual-rate chorus and lively tape echo." },

        // ---- Leads ----
        { "Silky Portamento",
          { { id::voiceMode, 1.0f }, { id::glide, 110.0f }, { id::osc1Wave, 2.0f }, { id::subLevel, 0.15f }, { id::osc2Wave, 3.0f },
            { id::osc2Semi, 12.0f }, { id::osc2Fm, 0.18f }, { id::oscMix, 0.25f }, { id::filterMode, 1.0f }, { id::cutoff, 3200.0f },
            { id::resonance, 0.20f }, { id::filterEnvAmt, 0.24f }, { id::drive, 0.16f },
            { id::ampAttack, 16.0f }, { id::ampDecay, 400.0f }, { id::ampSustain, 0.88f }, { id::ampRelease, 240.0f },
            { id::fltAttack, 25.0f }, { id::fltDecay, 450.0f }, { id::fltSustain, 0.50f },
            { id::lfoRate, 5.5f }, { id::lfoPitch, 12.0f }, { id::chorusMode, 1.0f },
            { id::echoTime, 360.0f }, { id::echoFeedback, 0.35f }, { id::echoMix, 0.25f } },
          "Smooth gliding mono synth lead with rounded triangle tone, singing vibrato, and echo tail." },

        { "Midnight Sax Lead",
          { { id::voiceMode, 1.0f }, { id::glide, 42.0f }, { id::osc1Wave, 0.0f }, { id::subLevel, 0.22f }, { id::osc2Wave, 1.0f },
            { id::osc2Detune, 5.0f }, { id::osc2Fm, 0.24f }, { id::oscMix, 0.38f }, { id::noiseLevel, 0.10f },
            { id::cutoff, 1850.0f }, { id::resonance, 0.36f }, { id::filterEnvAmt, 0.48f }, { id::drive, 0.34f },
            { id::ampAttack, 28.0f }, { id::ampDecay, 380.0f }, { id::ampSustain, 0.84f }, { id::ampRelease, 160.0f }, { id::velAmp, 0.80f },
            { id::fltAttack, 40.0f }, { id::fltDecay, 340.0f }, { id::fltSustain, 0.38f }, { id::velFlt, 0.78f },
            { id::lfoRate, 5.6f }, { id::lfoPitch, 11.0f }, { id::chorusMode, 2.0f },
            { id::echoTime, 320.0f }, { id::echoFeedback, 0.30f }, { id::echoMix, 0.22f } },
          "Expressive breathy mono solo lead with subtle FM edge, filter resonance, and velvet vibrato." },

        { "Retro Square Lead",
          { { id::voiceMode, 1.0f }, { id::glide, 52.0f }, { id::osc1Wave, 1.0f }, { id::osc1Pwm, 0.48f }, { id::subLevel, 0.25f },
            { id::osc2Wave, 1.0f }, { id::osc2Detune, 8.0f }, { id::oscMix, 0.45f }, { id::cutoff, 2800.0f }, { id::resonance, 0.28f },
            { id::filterEnvAmt, 0.35f }, { id::drive, 0.22f },
            { id::ampAttack, 4.0f }, { id::ampDecay, 300.0f }, { id::ampSustain, 0.88f }, { id::ampRelease, 140.0f },
            { id::fltAttack, 6.0f }, { id::fltDecay, 240.0f }, { id::fltSustain, 0.45f },
            { id::lfoRate, 5.0f }, { id::lfoPitch, 9.0f }, { id::lfoPwm, 0.18f }, { id::chorusMode, 2.0f },
            { id::echoTime, 270.0f }, { id::echoFeedback, 0.28f }, { id::echoMix, 0.18f } },
          "Classic hollow dual-square mono hook lead with crisp glide, subtle PWM motion, and Mode II chorus." },

        // ---- Keys & Plucks ----
        { "Tine Electric Piano",
          { { id::osc1Wave, 2.0f }, { id::subLevel, 0.12f }, { id::osc2Wave, 3.0f }, { id::osc2Semi, 12.0f }, { id::osc2Detune, 4.0f },
            { id::osc2Fm, 0.42f }, { id::oscMix, 0.20f }, { id::filterMode, 1.0f }, { id::cutoff, 3800.0f }, { id::resonance, 0.10f },
            { id::filterEnvAmt, 0.40f }, { id::drive, 0.18f },
            { id::ampAttack, 2.0f }, { id::ampDecay, 1450.0f }, { id::ampSustain, 0.22f }, { id::ampRelease, 360.0f }, { id::velAmp, 0.80f },
            { id::fltAttack, 1.0f }, { id::fltDecay, 260.0f }, { id::fltSustain, 0.12f }, { id::velFlt, 0.78f },
            { id::lfoRate, 3.6f }, { id::lfoFilter, 0.06f }, { id::chorusMode, 1.0f },
            { id::echoTime, 330.0f }, { id::echoFeedback, 0.22f }, { id::echoMix, 0.14f } },
          "Warm bell-tine electric piano with velocity-driven FM bark, triangle body, and Mode I chorus." },

        { "Crystal Celesta",
          { { id::osc1Wave, 3.0f }, { id::subLevel, 0.0f }, { id::osc2Wave, 2.0f }, { id::osc2Semi, 24.0f }, { id::osc2Detune, 5.0f },
            { id::osc2Fm, 0.60f }, { id::oscMix, 0.22f }, { id::filterMode, 1.0f }, { id::cutoff, 9200.0f }, { id::resonance, 0.14f },
            { id::filterEnvAmt, 0.22f },
            { id::ampAttack, 2.0f }, { id::ampDecay, 1150.0f }, { id::ampSustain, 0.0f }, { id::ampRelease, 700.0f },
            { id::fltAttack, 1.0f }, { id::fltDecay, 420.0f }, { id::fltSustain, 0.15f },
            { id::chorusMode, 3.0f }, { id::echoTime, 390.0f }, { id::echoFeedback, 0.36f }, { id::echoMix, 0.28f } },
          "Sparkling high-octave FM sparkle keys with airy top end, Chorus I+II, and crystalline echo." },

        // ---- Bass ----
        { "Sub Funk Bass",
          { { id::voiceMode, 1.0f }, { id::glide, 12.0f }, { id::osc1Wave, 2.0f }, { id::subLevel, 0.85f }, { id::osc2Wave, 0.0f },
            { id::osc2Detune, 4.0f }, { id::oscMix, 0.28f }, { id::cutoff, 240.0f }, { id::resonance, 0.30f }, { id::filterEnvAmt, 0.65f },
            { id::keytrack, 0.35f }, { id::drive, 0.32f },
            { id::ampAttack, 2.0f }, { id::ampDecay, 260.0f }, { id::ampSustain, 0.55f }, { id::ampRelease, 75.0f },
            { id::fltAttack, 1.0f }, { id::fltDecay, 170.0f }, { id::fltSustain, 0.08f }, { id::fltRelease, 85.0f },
            { id::chorusMode, 0.0f }, { id::echoMix, 0.0f } },
          "Deep dry mono sub-bass with rounded triangle weight and tight low-pass pocket punch." },

        { "Boogie Octave Bass",
          { { id::voiceMode, 1.0f }, { id::glide, 10.0f }, { id::osc1Wave, 1.0f }, { id::osc1Pwm, 0.32f }, { id::subLevel, 0.65f },
            { id::osc2Wave, 0.0f }, { id::osc2Semi, 12.0f }, { id::osc2Detune, -6.0f }, { id::oscMix, 0.36f },
            { id::cutoff, 420.0f }, { id::resonance, 0.34f }, { id::filterEnvAmt, 0.72f }, { id::keytrack, 0.40f }, { id::drive, 0.36f },
            { id::ampAttack, 2.0f }, { id::ampDecay, 240.0f }, { id::ampSustain, 0.40f }, { id::ampRelease, 70.0f },
            { id::fltAttack, 1.0f }, { id::fltDecay, 140.0f }, { id::fltSustain, 0.10f }, { id::fltRelease, 80.0f },
            { id::chorusMode, 0.0f }, { id::echoMix, 0.0f } },
          "Driving 80s boogie mono bass pairing tight pulse + sub with an octave-up saw bite." },
    };
    return list;
}
} // namespace selene
