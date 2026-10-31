// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include "TargetCurve.h"
#include "TonalAnalyzer.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace themis
{
// Rough model of a playback device: a high-pass (the speaker's bass limit), a low-pass and a few broad bumps.
// These are generic approximations of typical devices, not measurements of any particular product; they exist to
// show which parts of a balance a class of device cannot reproduce. Passband gain is 0 dB.
struct DeviceProfile
{
    struct Bump
    {
        double hz, gainDb, widthOctaves; // gaussian in log-frequency, `widthOctaves` = standard deviation
    };

    std::string name;
    double highPassHz = 0.0; // -3 dB point; 0 = no roll-off
    int highPassOrder = 2;   // Butterworth order, 6 dB/oct per order
    double lowPassHz = 0.0;
    int lowPassOrder = 2;
    std::vector<Bump> bumps;

    double responseDbAt (double hz) const
    {
        hz = std::max (hz, 1.0);
        double db = 0.0;
        if (highPassHz > 0.0)
            db -= 10.0 * std::log10 (1.0 + std::pow (highPassHz / hz, 2.0 * highPassOrder));
        if (lowPassHz > 0.0)
            db -= 10.0 * std::log10 (1.0 + std::pow (hz / lowPassHz, 2.0 * lowPassOrder));
        for (const auto& b : bumps)
        {
            const double x = std::log2 (hz / b.hz) / b.widthOctaves;
            db += b.gainDb * std::exp (-0.5 * x * x);
        }
        return db;
    }

    BandArray responseDb() const
    {
        BandArray out {};
        for (int i = 0; i < BandGrid::kNum; ++i)
            out[static_cast<size_t> (i)] = static_cast<float> (responseDbAt (BandGrid::centreHz (i)));
        return out;
    }

    // Lowest and highest frequency at which the device is within `withinDb` of its passband gain.
    void usableRangeHz (double withinDb, double& lo, double& hi) const
    {
        lo = hi = 0.0;
        for (double f = BandGrid::kF0; f <= BandGrid::kMaxHz; f *= 1.01)
            if (responseDbAt (f) >= -withinDb)
            {
                if (lo == 0.0)
                    lo = f;
                hi = f;
            }
    }

    static const std::vector<DeviceProfile>& builtIns()
    {
        static const std::vector<DeviceProfile> devices = {
            { "Phone speaker", 450.0, 3, 12000.0, 2, { { 2500.0, 3.0, 0.8 } } },
            { "Laptop", 180.0, 3, 16000.0, 2, { { 1500.0, 2.0, 1.0 } } },
            { "Earbuds", 30.0, 2, 18000.0, 2, { { 80.0, 3.0, 0.8 }, { 3000.0, 3.5, 0.7 } } },
            { "Bluetooth speaker", 85.0, 4, 14000.0, 2, { { 140.0, 3.0, 0.6 } } },
            { "Car", 35.0, 2, 16000.0, 2, { { 60.0, 5.0, 0.7 }, { 2500.0, -3.0, 0.8 } } },
            { "Small monitors", 55.0, 4, 20000.0, 2, {} },
        };
        return devices;
    }
};

// Traffic-light scale for the translation checks. The limits are hand-authored rules of thumb, not measured data.
enum class Level { good, warn, bad };

namespace translation
{
inline constexpr double kLowEndHz = 150.0;        // below this, bass is expected to be (near) mono
inline constexpr double kRangeFloorDb = 12.0;     // a device "reproduces" a band while it is within this of its passband
inline constexpr double kWorstBandMinHz = 30.0, kWorstBandMaxHz = 16000.0;
inline constexpr float kWorstBandFloorDb = -24.0f; // bands further below the anchor level than this are ignored

inline constexpr float kDeviceWarnDb = 1.5f, kDeviceBadDb = 3.0f; // extra loss versus the reference
inline constexpr float kMonoWarnDb = 2.0f, kMonoBadDb = 4.0f;
inline constexpr float kLowMonoWarnDb = 1.0f, kLowMonoBadDb = 2.5f;
inline constexpr float kBandMonoWarnDb = 3.5f, kBandMonoBadDb = 6.0f; // single band above the low end (3 dB = uncorrelated)

inline Level classify (float value, float warnAbove, float badAbove)
{
    return value <= warnAbove ? Level::good : value <= badAbove ? Level::warn : Level::bad;
}

// Verdict for a deviation from a target: within the tolerance is good, within twice it a warning, beyond that bad.
inline Level toleranceLevel (float deltaDb, float toleranceDb)
{
    return classify (std::abs (deltaDb), toleranceDb, 2.0f * toleranceDb);
}

// Verdict for the mono loss of one band: the low end is held to a tighter limit than the rest.
inline Level bandMonoLevel (int band, float lossDb)
{
    return BandGrid::centreHz (band) < kLowEndHz ? classify (lossDb, kLowMonoWarnDb, kLowMonoBadDb)
                                                 : classify (lossDb, kBandMonoWarnDb, kBandMonoBadDb);
}
} // namespace translation

struct TranslationReport
{
    bool haveDevice = false;
    float deviceChangeDb = 0.0f;    // overall level change of the track through the device (<= 0 for a roll-off device)
    float referenceChangeDb = 0.0f; // the same for the target curve
    float extraLossDb = 0.0f;       // how much more the track loses than the target does (negative: less)
    Level deviceLevel = Level::good;

    bool haveFold = false;
    float monoLossDb = 0.0f;    // overall power lost when summing to mono (>= 0)
    float lowMonoLossDb = 0.0f; // the same below kLowEndHz
    int worstBand = -1;
    float worstBandLossDb = 0.0f;
    Level monoLevel = Level::good, lowMonoLevel = Level::good;
};

// Level change, in dB, of a spectrum (shape only, any common offset) passed through `responseDb`.
inline float netChangeDb (const BandArray& spectrumDb, const BandArray& responseDb)
{
    double before = 0.0, after = 0.0;
    for (size_t i = 0; i < spectrumDb.size(); ++i)
    {
        if (spectrumDb[i] <= kNoDataDb)
            continue;
        const double p = dbToPower (spectrumDb[i]);
        before += p;
        after += p * dbToPower (responseDb[i]);
    }
    return before > 0.0 && after > 0.0 ? static_cast<float> (powerToDb (after / before, 0.0)) : 0.0f;
}

// Power lost by a sum to mono, in dB (>= 0), over the bands in [loHz, hiHz): the mid power is the part that
// survives, and mid + side is what the stereo signal carried. `midDb` weights the bands, `foldDb` is the per-band
// mono-fold curve (10 log10 (M / (M + S))).
inline float monoLossDb (const BandArray& midDb, const BandArray& foldDb, double loHz, double hiHz)
{
    double kept = 0.0, total = 0.0;
    for (int i = 0; i < BandGrid::kNum; ++i)
    {
        const auto s = static_cast<size_t> (i);
        const double f = BandGrid::centreHz (i);
        if (f < loHz || f >= hiHz || midDb[s] <= kNoDataDb)
            continue;
        const double m = dbToPower (midDb[s]);
        const double share = std::max (dbToPower (foldDb[s]), 1.0e-6);
        kept += m;
        total += m / share;
    }
    return kept > 0.0 && total > 0.0 ? static_cast<float> (std::max (0.0, powerToDb (total / kept, 0.0))) : 0.0f;
}

// The mono-fold part of a report: the power a sum to mono loses, overall and in the low end, and the worst band. Needs no
// target or device, so the master plan uses it on its own.
inline void addMonoFold (TranslationReport& r, const BandArray& measured, const BandArray& fold)
{
    r.haveFold = true;
    r.monoLossDb = monoLossDb (measured, fold, 0.0, 1.0e9);
    r.lowMonoLossDb = monoLossDb (measured, fold, 0.0, translation::kLowEndHz);
    r.monoLevel = translation::classify (r.monoLossDb, translation::kMonoWarnDb, translation::kMonoBadDb);
    r.lowMonoLevel = translation::classify (r.lowMonoLossDb, translation::kLowMonoWarnDb, translation::kLowMonoBadDb);

    float worst = 0.0f;
    for (int i = 0; i < BandGrid::kNum; ++i)
    {
        const auto s = static_cast<size_t> (i);
        const double f = BandGrid::centreHz (i);
        if (f < translation::kWorstBandMinHz || f > translation::kWorstBandMaxHz || measured[s] < translation::kWorstBandFloorDb)
            continue;
        if (-fold[s] > worst)
        {
            worst = -fold[s];
            r.worstBand = i;
        }
    }
    r.worstBandLossDb = worst;
}

// Summarises how the measured balance is likely to translate. `fold` and `device` are optional.
inline TranslationReport analyseTranslation (const BandArray& measured, const BandArray* fold, const TargetCurve& target,
                                             const DeviceProfile* device)
{
    TranslationReport r;
    if (device != nullptr)
    {
        const auto response = device->responseDb();
        r.haveDevice = true;
        r.deviceChangeDb = netChangeDb (measured, response);
        r.referenceChangeDb = netChangeDb (target.medianDb, response);
        r.extraLossDb = r.referenceChangeDb - r.deviceChangeDb;
        r.deviceLevel = translation::classify (r.extraLossDb, translation::kDeviceWarnDb, translation::kDeviceBadDb);
    }
    if (fold != nullptr)
        addMonoFold (r, measured, *fold);
    return r;
}
} // namespace themis
