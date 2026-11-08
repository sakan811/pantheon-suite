// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

// A synthetic singer with a known pitch at every sample: a harmonic series (1/h tilt) shaped by two formant resonances, so the
// spectral envelope is a fixed thing the tests can check after a pitch shift. The truth is returned with the audio.
namespace test
{
struct SungNote
{
    double start, duration;     // seconds
    double midi;                // sung pitch (the centre)
    double vibratoHz = 0.0, vibratoCents = 0.0;
    double driftCents = 0.0;    // a slow sine of 0.7 Hz on top
    double glideMs = 40.0;      // time to slide in from the previous note
};

struct Voice
{
    std::vector<float> audio;
    std::vector<double> midi;   // true pitch per sample, 0 where silent
    double rate = 48000.0;
};

inline double resonance (double f, double centre, double q)
{
    const double r = f / centre;
    return 1.0 / std::sqrt ((1.0 - r * r) * (1.0 - r * r) + (r / q) * (r / q));
}

inline Voice sing (const std::vector<SungNote>& notes, double seconds, double rate = 48000.0, double f1 = 700.0, double f2 = 1150.0, double level = 0.2)
// (resonances of Q 3.5 and 4.5: the bandwidth of a real vowel, about 200 Hz, wide enough that the harmonics sample the envelope)
{
    Voice v;
    v.rate = rate;
    const int n = (int) (seconds * rate);
    v.audio.assign ((size_t) n, 0.0f);
    v.midi.assign ((size_t) n, 0.0);
    double phase = 0.0;
    double lastMidi = notes.empty() ? 60.0 : notes.front().midi;
    for (int i = 0; i < n; ++i)
    {
        const double t = i / rate;
        const SungNote* current = nullptr;
        const SungNote* previous = nullptr;
        for (size_t k = 0; k < notes.size(); ++k)
            if (t >= notes[k].start && t < notes[k].start + notes[k].duration)
            {
                current = &notes[k];
                previous = k > 0 && notes[k - 1].start + notes[k - 1].duration >= notes[k].start - 1.0e-9 ? &notes[k - 1] : nullptr;
            }
        if (current == nullptr)
            continue;
        const double local = t - current->start;
        double m = current->midi + (current->vibratoCents / 100.0) * std::sin (2.0 * M_PI * current->vibratoHz * local) * std::min (1.0, local / 0.3)
                   + (current->driftCents / 100.0) * std::sin (2.0 * M_PI * 0.7 * local);
        if (previous != nullptr && local < current->glideMs * 0.001)
        {
            const double u = local / (current->glideMs * 0.001), s = 0.5 - 0.5 * std::cos (M_PI * u);
            m = previous->midi + s * (m - previous->midi);
        }
        lastMidi = m;
        v.midi[(size_t) i] = m;
        const double f0 = 440.0 * std::exp2 ((m - 69.0) / 12.0);
        phase += 2.0 * M_PI * f0 / rate;
        const double fade = std::min ({ 1.0, local / 0.02, (current->duration - local) / 0.02 });
        double sum = 0.0;
        for (int h = 1; h * f0 < 0.45 * rate && h <= 60; ++h)
        {
            const double f = h * f0;
            const double amp = (resonance (f, f1, 3.5) + 0.8 * resonance (f, f2, 4.5)) / h;
            sum += amp * std::sin (h * phase + 0.2 * h);
        }
        v.audio[(size_t) i] = (float) (level * fade * sum / 6.0);
    }
    (void) lastMidi;
    return v;
}

// Nine notes in C major, each a little sharp or flat; one with vibrato, one that drifts. The phrase the screenshots show.
inline std::vector<SungNote> outOfTunePhrase()
{
    return {
        { 0.30, 0.55, 60.0 - 0.34 },
        { 0.85, 0.45, 62.0 + 0.27 },
        { 1.30, 0.75, 64.0 - 0.18, 5.6, 42.0 },
        { 2.30, 0.50, 65.0 + 0.39 },
        { 2.80, 0.60, 67.0 - 0.12, 0.0, 0.0, 28.0 },
        { 3.70, 0.40, 69.0 + 0.45 },
        { 4.10, 1.10, 67.0 - 0.26, 5.9, 55.0 },
        { 5.50, 0.60, 64.0 + 0.14 },
        { 6.10, 0.50, 60.0 - 0.41 },
    };
}

inline double rms (const std::vector<float>& x, int from, int to)
{
    double s = 0.0;
    for (int i = from; i < to; ++i)
        s += (double) x[(size_t) i] * x[(size_t) i];
    return std::sqrt (s / std::max (1, to - from));
}

// Spectral centroid between lo and hi Hz of x[from, to), by a direct DFT on a 5 Hz grid (small inputs only).
inline double centroid (const std::vector<float>& x, int from, int to, double rate, double lo = 300.0, double hi = 2500.0)
{
    double num = 0.0, den = 0.0;
    for (double f = lo; f < hi; f += 5.0)
    {
        double re = 0.0, im = 0.0;
        for (int i = from; i < to; ++i)
        {
            const double w = 0.5 - 0.5 * std::cos (2.0 * M_PI * (i - from) / (to - from));
            re += w * x[(size_t) i] * std::cos (2.0 * M_PI * f * i / rate);
            im += w * x[(size_t) i] * std::sin (2.0 * M_PI * f * i / rate);
        }
        const double p = re * re + im * im;
        num += f * p;
        den += p;
    }
    return den > 0.0 ? num / den : 0.0;
}
} // namespace test
