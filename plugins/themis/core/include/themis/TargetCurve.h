// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "TonalAnalyzer.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <locale>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace themis
{
// A tonal-balance target: a median curve plus a +/- half-width tolerance, both per BandGrid band.
// Levels are in dB, normalised so the anchor region (200 Hz - 5 kHz) averages 0.
struct TargetCurve
{
    std::string name = "Flat (pink)";
    BandArray medianDb {};
    BandArray halfWidthDb {};

    using Anchors = std::vector<std::pair<float, float>>; // (Hz, dB), ascending Hz

    static float interpolate (const Anchors& a, double hz)
    {
        if (hz <= a.front().first)
            return a.front().second;
        if (hz >= a.back().first)
            return a.back().second;
        for (size_t i = 1; i < a.size(); ++i)
        {
            if (hz <= a[i].first)
            {
                const double t = std::log (hz / a[i - 1].first) / std::log (static_cast<double> (a[i].first) / a[i - 1].first);
                return static_cast<float> (a[i - 1].second + t * (a[i].second - a[i - 1].second));
            }
        }
        return a.back().second;
    }

    static TargetCurve fromAnchors (std::string name, const Anchors& median, const Anchors& width)
    {
        TargetCurve c;
        c.name = std::move (name);
        for (int i = 0; i < BandGrid::kNum; ++i)
        {
            const double f = BandGrid::centreHz (i);
            c.medianDb[static_cast<size_t> (i)] = interpolate (median, f);
            c.halfWidthDb[static_cast<size_t> (i)] = interpolate (width, f);
        }
        normaliseAnchor (c.medianDb);
        return c;
    }

    // Average several analysed tracks. Width = spread between tracks, never tighter than minWidthDb.
    static TargetCurve fromTracks (std::string name, const std::vector<BandArray>& tracks, float minWidthDb = 1.5f,
                                   float singleTrackWidthDb = 2.0f)
    {
        TargetCurve c;
        c.name = std::move (name);
        if (tracks.empty())
            return c;
        // Bands a track has no data for (above its Nyquist) are left out of that band's statistics, so a
        // low-sample-rate file can't drag the curve down.
        std::array<bool, BandGrid::kNum> hasData {};
        for (int i = 0; i < BandGrid::kNum; ++i)
        {
            const size_t s = static_cast<size_t> (i);
            double mean = 0.0;
            int n = 0;
            for (const auto& t : tracks)
                if (t[s] > kNoDataDb)
                {
                    mean += t[s];
                    ++n;
                }
            if (n == 0)
                continue;
            mean /= n;
            double var = 0.0;
            for (const auto& t : tracks)
                if (t[s] > kNoDataDb)
                    var += (t[s] - mean) * (t[s] - mean);
            const double sd = n > 1 ? std::sqrt (var / (n - 1.0)) : 0.0;
            c.medianDb[s] = static_cast<float> (mean);
            c.halfWidthDb[s] = n > 1 ? std::max (minWidthDb, static_cast<float> (sd * 1.25)) : singleTrackWidthDb;
            hasData[s] = true;
        }
        // No track reaches these bands: hold the nearest band that has data (flat extension) instead of
        // inventing a -200 dB wall that would also leak into the smoothing below.
        const auto holdFrom = [&] (int src, int dst) {
            const auto s = static_cast<size_t> (src), d = static_cast<size_t> (dst);
            if (hasData[d] || !hasData[s])
                return;
            c.medianDb[d] = c.medianDb[s];
            c.halfWidthDb[d] = c.halfWidthDb[s];
            hasData[d] = true;
        };
        for (int i = 1; i < BandGrid::kNum; ++i)
            holdFrom (i - 1, i);
        for (int i = BandGrid::kNum - 2; i >= 0; --i)
            holdFrom (i + 1, i);
        // A single track is noisy at fine resolution; smooth over +/- 1/3 octave.
        smoothBands (c.medianDb, 2);
        smoothBands (c.halfWidthDb, 2);
        normaliseAnchor (c.medianDb);
        return c;
    }

    // ---- built-in starter curves -------------------------------------------------------------
    // These are hand-authored approximations of typical mastered material (constant-Q band energy,
    // so pink noise reads flat). They are starting points, not measured percentiles of a dataset:
    // for accuracy, learn a curve from reference tracks you trust.
    static const std::vector<TargetCurve>& builtIns()
    {
        static const std::vector<TargetCurve> list = makeBuiltIns();
        return list;
    }

    // ---- the closest shape ---------------------------------------------------------------------
    // RMS difference in dB between a measured curve and a target's median, over the bands the sample rate carries
    // (BandGrid::bandsUpTo of usableMaxHz, as the tilt does), after removing the mean offset: a shape distance, so
    // the level of the curve and the gain of the master do not count.
    static float distanceDb (const BandArray& measured, const TargetCurve& target, double sampleRate)
    {
        const int bands = BandGrid::bandsUpTo (BandGrid::usableMaxHz (sampleRate));
        double mean = 0.0;
        for (int i = 0; i < bands; ++i)
            mean += static_cast<double> (measured[static_cast<size_t> (i)]) - static_cast<double> (target.medianDb[static_cast<size_t> (i)]);
        mean /= static_cast<double> (bands);
        double squares = 0.0;
        for (int i = 0; i < bands; ++i)
        {
            const auto s = static_cast<size_t> (i);
            const double d = static_cast<double> (measured[s]) - static_cast<double> (target.medianDb[s]) - mean;
            squares += d * d;
        }
        return static_cast<float> (std::sqrt (squares / static_cast<double> (bands)));
    }

    // Index of the curve in `list` nearest to the measured one (distance in *distance when given). A tie goes to the
    // first curve in the list, so the answer does not depend on anything but the list's order.
    static size_t nearest (const BandArray& measured, const std::vector<TargetCurve>& list, double sampleRate, float* distance = nullptr)
    {
        size_t best = 0;
        float bestDb = std::numeric_limits<float>::infinity();
        for (size_t k = 0; k < list.size(); ++k)
        {
            const float d = distanceDb (measured, list[k], sampleRate);
            if (d < bestDb)
            {
                bestDb = d;
                best = k;
            }
        }
        if (distance != nullptr)
            *distance = bestDb;
        return best;
    }

    // The built-in curve whose shape is nearest the measured one ("Closest" in the header).
    static size_t nearestBuiltIn (const BandArray& measured, double sampleRate, float* distance = nullptr)
    {
        return nearest (measured, builtIns(), sampleRate, distance);
    }

    // ---- (de)serialisation -------------------------------------------------------------------
    std::string toJson() const
    {
        std::ostringstream o;
        o.setf (std::ios::fixed);
        o.precision (3);
        o << "{\"themisCurve\":1,\"name\":\"";
        for (char ch : name)
            o << (ch == '"' || ch == '\\' ? '\'' : ch);
        o << "\",\"bands\":" << BandGrid::kNum << ",\"median\":[";
        for (size_t i = 0; i < medianDb.size(); ++i)
            o << (i ? "," : "") << medianDb[i];
        o << "],\"width\":[";
        for (size_t i = 0; i < halfWidthDb.size(); ++i)
            o << (i ? "," : "") << halfWidthDb[i];
        o << "]}";
        return o.str();
    }

    static bool fromJson (const std::string& json, TargetCurve& out)
    {
        if (json.find ("\"themisCurve\"") == std::string::npos)
            return false;
        TargetCurve c;
        const auto nameKey = json.find ("\"name\":\"");
        if (nameKey != std::string::npos)
        {
            const auto start = nameKey + 8;
            const auto end = json.find ('"', start);
            if (end != std::string::npos)
                c.name = json.substr (start, end - start);
        }
        if (!parseArray (json, "\"median\":[", c.medianDb) || !parseArray (json, "\"width\":[", c.halfWidthDb))
            return false;
        out = c;
        return true;
    }

private:
    static bool parseArray (const std::string& json, const char* key, BandArray& dst)
    {
        const auto k = json.find (key);
        if (k == std::string::npos)
            return false;
        // Classic locale: strtod would follow the host's C locale and reject "1.5" under a comma-decimal one.
        std::istringstream in (json.substr (k + std::strlen (key)));
        in.imbue (std::locale::classic());
        for (size_t i = 0; i < dst.size(); ++i)
        {
            double v = 0.0;
            if (!(in >> v) || !std::isfinite (v))
                return false;
            dst[i] = static_cast<float> (v);
            in >> std::ws;
            if (in.peek() == ',')
                in.get();
        }
        in >> std::ws;
        return in.peek() == ']';
    }

    static std::vector<TargetCurve> makeBuiltIns()
    {
        const Anchors defaultWidth = { { 20, 4.0f },  { 30, 4.0f },  { 45, 3.5f },  { 60, 3.0f },  { 90, 2.5f },
                                       { 200, 2.0f }, { 500, 1.6f }, { 1000, 1.5f }, { 3000, 1.8f }, { 5000, 2.2f },
                                       { 8000, 2.8f }, { 12000, 3.5f }, { 16000, 4.5f }, { 20000, 6.0f } };
        std::vector<TargetCurve> v;
        v.push_back (fromAnchors ("Flat (pink)", { { 20, 0 }, { 20000, 0 } }, { { 20, 3 }, { 20000, 3 } }));
        v.push_back (fromAnchors ("Modern Pop / Streaming",
                                  { { 20, -14 }, { 30, -7 }, { 45, -1.5f }, { 60, 1.5f }, { 90, 2.5f }, { 130, 2 }, { 200, 1 },
                                    { 300, 0.3f }, { 500, 0 }, { 800, -0.5f }, { 1200, -1.2f }, { 2000, -2.2f }, { 3000, -3 },
                                    { 5000, -4 }, { 8000, -5.5f }, { 12000, -8.5f }, { 16000, -12 }, { 20000, -20 } },
                                  defaultWidth));
        v.push_back (fromAnchors ("Hip-Hop / Trap",
                                  { { 20, -10 }, { 30, -2 }, { 45, 3.5f }, { 60, 5.5f }, { 90, 4 }, { 130, 2.5f }, { 200, 1 },
                                    { 300, 0 }, { 500, -0.5f }, { 800, -1.2f }, { 1200, -1.8f }, { 2000, -2.5f }, { 3000, -3.2f },
                                    { 5000, -4.2f }, { 8000, -5.5f }, { 12000, -8 }, { 16000, -11.5f }, { 20000, -19 } },
                                  defaultWidth));
        v.push_back (fromAnchors ("EDM / Club",
                                  { { 20, -12 }, { 30, -4 }, { 45, 2.5f }, { 60, 4.5f }, { 90, 3.5f }, { 130, 2 }, { 200, 0.8f },
                                    { 300, -0.2f }, { 500, -0.8f }, { 800, -1 }, { 1200, -1 }, { 2000, -1.5f }, { 3000, -2 },
                                    { 5000, -2.5f }, { 8000, -3.5f }, { 12000, -5.5f }, { 16000, -9 }, { 20000, -18 } },
                                  defaultWidth));
        v.push_back (fromAnchors ("Rock",
                                  { { 20, -16 }, { 30, -9 }, { 45, -3 }, { 60, 0 }, { 90, 1.5f }, { 130, 2 }, { 200, 1.5f },
                                    { 300, 1 }, { 500, 0.3f }, { 800, 0 }, { 1200, -0.3f }, { 2000, -0.8f }, { 3000, -1.5f },
                                    { 5000, -3 }, { 8000, -4.5f }, { 12000, -7.5f }, { 16000, -11.5f }, { 20000, -20 } },
                                  defaultWidth));
        v.push_back (fromAnchors ("Acoustic / Jazz",
                                  { { 20, -20 }, { 30, -14 }, { 45, -7 }, { 60, -3.5f }, { 90, -0.5f }, { 130, 1 }, { 200, 1.5f },
                                    { 300, 1.2f }, { 500, 0.5f }, { 800, 0 }, { 1200, -0.8f }, { 2000, -1.8f }, { 3000, -2.8f },
                                    { 5000, -4.5f }, { 8000, -6.5f }, { 12000, -9.5f }, { 16000, -14 }, { 20000, -22 } },
                                  defaultWidth));
        v.push_back (fromAnchors ("Orchestral",
                                  { { 20, -18 }, { 30, -11 }, { 45, -5 }, { 60, -2 }, { 90, 0.5f }, { 130, 1.5f }, { 200, 1.5f },
                                    { 300, 1 }, { 500, 0.2f }, { 800, -0.5f }, { 1200, -1.5f }, { 2000, -2.8f }, { 3000, -3.8f },
                                    { 5000, -5.5f }, { 8000, -8 }, { 12000, -12 }, { 16000, -17 }, { 20000, -26 } },
                                  defaultWidth));
        return v;
    }
};
} // namespace themis
