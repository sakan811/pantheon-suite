// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <pantheon/dsp/MusicBands.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace pdsp
{
// A band handed from one plugin to another as text (through the clipboard): an analyser says where and how much, an
// equaliser or a harmonic generator makes the band. Plain text so it survives any clipboard and can be read by a person:
//
//   pantheon-bands 1
//   bell freq=270 gain=-2.0 q=0.70 place=mid
//   fill lo=40 hi=90 amount=0.50
//   curve f0=20 oct=6 name=Modern_Pop db=-1.2,0.0,0.8,...
//   width sub=0 bass=50 lowmid=90 mid=100 uppermid=100 high=100
//   limiter ceiling=-1.0 gainchange=-3.0 amount=0.50
//
// `amount` (0..1, default 1, written only when below 1) is how much of a band or a limiter setting to take: a band's gain is
// scaled by it (a fill band's strength is scaled from its off level), a limiter's gain change is scaled by it (its ceiling is not).
//
// A width line gives, per band of `pdsp::bands` (sub, bass, lowmid, mid, uppermid, high in that order), the percent of the side
// signal to keep (0 = mono, 100 = as it is, 200 = twice); a key that is absent is not suggested. A width line has no `amount`
// key: each band's percent is taken as it is. `decodeWidth` reads it; `decode` skips it like any other kind it does not know.
//
// A curve line carries a spectrum shape (not a band): the dB of each point of a log-frequency grid (f0 is the first point's
// frequency, oct the points per octave, name has its spaces written as underscores), relative to the level around the middle of
// the spectrum. A reader that does not know curves skips the line like any other kind it does not know.
//
// A limiter line is a setting for a limiter, not a band: the ceiling to set (an absolute level in dBTP) and how far to move the
// limiter's input gain (a change in dB, negative to turn it down), either of them alone. `decode` skips it like any other kind it does
// not know; `decodeLimiter` reads it.
//
// Unknown keys are ignored and a band with an unknown kind is skipped, so a newer writer does not break an older reader.
// A reader is forgiving about the text around the data: leading and trailing white space on a line (spaces, tabs, a stray
// carriage return) and a UTF-8 byte order mark at the start are ignored; and strict about the numbers: a value that is not a
// finite number drops its band, and one outside the ranges in `pdsp::limits` is clamped into them (a pasted gain=1e9 is not
// a gain of 1e9 dB).
namespace limits
{
inline constexpr double kMinFreqHz = 10.0, kMaxFreqHz = 40000.0; // a band's frequency, a fill band's edges, a cut's corner
inline constexpr double kMaxGainDb = 36.0;                      // a bell's or a shelf's gain, either way
inline constexpr double kMinQ = 0.1, kMaxQ = 40.0;
inline constexpr double kMinSlopeDbPerOct = 6.0, kMaxSlopeDbPerOct = 96.0;
inline constexpr double kMinCurveF0Hz = 1.0, kMaxCurveF0Hz = 100000.0; // a curve's first point
inline constexpr double kMinCurvePerOctave = 0.5, kMaxCurvePerOctave = 96.0;
inline constexpr double kMaxCurveDb = 120.0;                    // a curve's level, either way
inline constexpr double kMinWidthPercent = 0.0, kMaxWidthPercent = 200.0; // a width line's share of the side signal, per band
} // namespace limits

struct LinkBand
{
    enum class Kind { bell, lowShelf, highShelf, lowCut, highCut, fill };
    enum class Place { stereo, left, right, mid, side };

    Kind kind = Kind::bell;
    Place place = Place::stereo;
    double freq = 1000.0, gainDb = 0.0, q = 0.7071; // equaliser bands; a cut's q is unused, its slope is `slopeDbPerOct`
    double slopeDbPerOct = 24.0;
    double loHz = 40.0, hiHz = 120.0;                  // a fill band's range
    double amount = 1.0;                               // 0..1: how much of the gain (or a fill band's strength) to take
};

// The width a suggestion asks for, per band of pdsp::bands (index = band): the percent of the side signal to keep. An absent
// band is not suggested (nullopt). Values are clamped into limits::kMin/kMaxWidthPercent when read.
struct LinkWidth
{
    std::array<std::optional<double>, bands::kCount> percent {};

    bool valid() const
    {
        for (const auto& p : percent)
            if (p.has_value())
                return true;
        return false;
    }
};

// A spectrum shape handed over with the bands: `db[i]` is the level at f0Hz * 2^(i / perOctave).
struct LinkCurve
{
    static constexpr size_t kMaxPoints = 512;

    double f0Hz = 20.0, perOctave = 6.0;
    std::vector<double> db;
    std::string name;

    bool valid() const
    {
        return f0Hz >= limits::kMinCurveF0Hz && f0Hz <= limits::kMaxCurveF0Hz && perOctave >= limits::kMinCurvePerOctave
               && perOctave <= limits::kMaxCurvePerOctave && db.size() >= 2 && db.size() <= kMaxPoints;
    }
    double frequency (size_t i) const { return f0Hz * std::pow (2.0, static_cast<double> (i) / perOctave); }

    // The level at any frequency, linearly interpolated on the log-frequency axis and held beyond the ends.
    double at (double hz) const
    {
        if (db.empty())
            return 0.0;
        const double pos = std::clamp (std::log2 (std::max (hz, 1.0e-3) / f0Hz) * perOctave, 0.0, static_cast<double> (db.size() - 1));
        const size_t i = std::min (static_cast<size_t> (pos), db.size() - 2);
        const double t = pos - static_cast<double> (i);
        return db[i] + t * (db[i + 1] - db[i]);
    }
};

// What a limiter is asked to change: the ceiling (absolute, dBTP) and/or its gain (relative, dB).
struct LinkLimiter
{
    bool hasCeiling = false, hasGainChange = false;
    double ceilingDb = -1.0, gainChangeDb = 0.0;
    double amount = 1.0; // 0..1: how much of the gain change to take (the ceiling is absolute and not scaled)

    bool valid() const { return hasCeiling || hasGainChange; }
};

namespace link
{
inline constexpr const char* kHeader = "pantheon-bands 1";

inline const char* kindName (LinkBand::Kind k)
{
    switch (k)
    {
        case LinkBand::Kind::bell:      return "bell";
        case LinkBand::Kind::lowShelf:  return "lowshelf";
        case LinkBand::Kind::highShelf: return "highshelf";
        case LinkBand::Kind::lowCut:    return "lowcut";
        case LinkBand::Kind::highCut:   return "highcut";
        case LinkBand::Kind::fill:      return "fill";
    }
    return "bell";
}

inline const char* placeName (LinkBand::Place p)
{
    switch (p)
    {
        case LinkBand::Place::stereo: return "stereo";
        case LinkBand::Place::left:   return "left";
        case LinkBand::Place::right:  return "right";
        case LinkBand::Place::mid:    return "mid";
        case LinkBand::Place::side:   return "side";
    }
    return "stereo";
}

// The keys of a width line, in the order of pdsp::bands.
inline constexpr std::array<const char*, bands::kCount> kWidthKeys { "sub", "bass", "lowmid", "mid", "uppermid", "high" };

// " amount=0.50" when the amount takes less than all of it, empty otherwise (the default is not written).
inline std::string amountText (double amount)
{
    const double a = std::clamp (amount, 0.0, 1.0);
    if (!(a < 1.0))
        return {};
    char buf[32];
    std::snprintf (buf, sizeof (buf), " amount=%.2f", a);
    return buf;
}

inline std::string encodeCurve (const LinkCurve& c)
{
    std::string name = c.name;
    for (auto& ch : name)
        if (ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t' || ch == '=')
            ch = '_';
    char buf[64];
    std::snprintf (buf, sizeof (buf), "\ncurve f0=%.4g oct=%.4g", c.f0Hz, c.perOctave);
    std::string out = buf;
    if (!name.empty())
        out += " name=" + name;
    out += " db=";
    for (size_t i = 0; i < c.db.size(); ++i)
    {
        std::snprintf (buf, sizeof (buf), i == 0 ? "%.1f" : ",%.1f", c.db[i]);
        out += buf;
    }
    return out;
}

inline std::string encodeLimiter (const LinkLimiter& l)
{
    std::string out;
    char buf[64];
    if (l.hasCeiling)
    {
        std::snprintf (buf, sizeof (buf), " ceiling=%.1f", l.ceilingDb);
        out += buf;
    }
    if (l.hasGainChange)
    {
        std::snprintf (buf, sizeof (buf), " gainchange=%.1f", l.gainChangeDb);
        out += buf;
    }
    return out.empty() ? out : "\nlimiter" + out + amountText (l.amount);
}

inline std::string encodeWidth (const LinkWidth& w)
{
    if (!w.valid())
        return {};
    std::string out = "\nwidth";
    char buf[32];
    for (size_t i = 0; i < bands::kCount; ++i)
        if (w.percent[i].has_value())
        {
            const double v = std::clamp (*w.percent[i], limits::kMinWidthPercent, limits::kMaxWidthPercent);
            std::snprintf (buf, sizeof (buf), " %s=%.0f", kWidthKeys[i], v);
            out += buf;
        }
    return out;
}

inline std::string encode (const std::vector<LinkBand>& bands, const LinkCurve* curve = nullptr, const LinkLimiter* limiter = nullptr,
                           const LinkWidth* width = nullptr)
{
    std::string out = kHeader;
    char line[160];
    for (const auto& b : bands)
    {
        if (b.kind == LinkBand::Kind::fill)
            std::snprintf (line, sizeof (line), "\nfill lo=%.0f hi=%.0f place=%s", b.loHz, b.hiHz, placeName (b.place));
        else if (b.kind == LinkBand::Kind::lowCut || b.kind == LinkBand::Kind::highCut)
            std::snprintf (line, sizeof (line), "\n%s freq=%.0f slope=%.0f place=%s", kindName (b.kind), b.freq, b.slopeDbPerOct, placeName (b.place));
        else
            std::snprintf (line, sizeof (line), "\n%s freq=%.0f gain=%.1f q=%.2f place=%s", kindName (b.kind), b.freq, b.gainDb, b.q, placeName (b.place));
        out += line;
        out += amountText (b.amount);
    }
    if (curve != nullptr && curve->valid())
        out += encodeCurve (*curve);
    if (width != nullptr)
        out += encodeWidth (*width);
    if (limiter != nullptr)
        out += encodeLimiter (*limiter);
    return out;
}

// The words after "curve": f0=, oct=, name= and db=; an invalid curve (a bad number, too few or too many points) comes back empty.
inline LinkCurve parseCurve (std::istringstream& words)
{
    LinkCurve c;
    std::string pair;
    while (words >> pair)
    {
        const auto eq = pair.find ('=');
        if (eq == std::string::npos)
            continue;
        const auto key = pair.substr (0, eq), value = pair.substr (eq + 1);
        if (key == "name")
        {
            c.name = value;
            std::replace (c.name.begin(), c.name.end(), '_', ' ');
        }
        else if (key == "f0" || key == "oct")
        {
            char* end = nullptr;
            const double v = std::strtod (value.c_str(), &end);
            if (end == value.c_str() || !std::isfinite (v))
                return {};
            (key == "f0" ? c.f0Hz : c.perOctave) = v;
        }
        else if (key == "db")
        {
            std::istringstream list (value);
            std::string item;
            while (std::getline (list, item, ','))
            {
                char* end = nullptr;
                const double v = std::strtod (item.c_str(), &end);
                if (end == item.c_str() || !std::isfinite (v) || c.db.size() >= LinkCurve::kMaxPoints)
                    return {};
                c.db.push_back (std::clamp (v, -limits::kMaxCurveDb, limits::kMaxCurveDb));
            }
        }
    }
    return c.valid() ? c : LinkCurve {};
}

// One line of pasted text without the white space around it (and a UTF-8 byte order mark when it is the first line).
inline void trimLine (std::string& line, bool first)
{
    constexpr const char* kWhite = " \t\r\n\f\v";
    if (first && line.size() >= 3 && line.compare (0, 3, "\xEF\xBB\xBF") == 0)
        line.erase (0, 3);
    const auto begin = line.find_first_not_of (kWhite);
    if (begin == std::string::npos)
    {
        line.clear();
        return;
    }
    line = line.substr (begin, line.find_last_not_of (kWhite) - begin + 1);
}

// False when the text is not a band list (no header); otherwise the bands it could read, in order.
inline bool decode (const std::string& text, std::vector<LinkBand>& out, LinkCurve* curve = nullptr)
{
    out.clear();
    if (curve != nullptr)
        *curve = {};
    std::istringstream in (text);
    std::string line;
    bool header = false, firstLine = true;
    while (std::getline (in, line))
    {
        trimLine (line, firstLine);
        firstLine = false;
        if (!header)
        {
            if (line.empty())
                continue;
            if (line != kHeader)
                return false;
            header = true;
            continue;
        }
        std::istringstream words (line);
        std::string kind;
        if (!(words >> kind))
            continue;
        if (kind == "curve")
        {
            if (curve != nullptr)
                *curve = parseCurve (words);
            continue;
        }
        LinkBand b;
        bool known = false;
        for (auto k : { LinkBand::Kind::bell, LinkBand::Kind::lowShelf, LinkBand::Kind::highShelf, LinkBand::Kind::lowCut,
                        LinkBand::Kind::highCut, LinkBand::Kind::fill })
            if (kind == kindName (k))
            {
                b.kind = k;
                known = true;
            }
        if (!known)
            continue;
        std::string pair;
        bool valid = true;
        while (words >> pair)
        {
            const auto eq = pair.find ('=');
            if (eq == std::string::npos)
                continue;
            const auto key = pair.substr (0, eq), value = pair.substr (eq + 1);
            if (key == "place")
            {
                for (auto p : { LinkBand::Place::stereo, LinkBand::Place::left, LinkBand::Place::right, LinkBand::Place::mid, LinkBand::Place::side })
                    if (value == placeName (p))
                        b.place = p;
                continue;
            }
            double* field = key == "freq"  ? &b.freq
                          : key == "gain"  ? &b.gainDb
                          : key == "q"     ? &b.q
                          : key == "slope" ? &b.slopeDbPerOct
                          : key == "lo"    ? &b.loHz
                          : key == "hi"    ? &b.hiHz
                          : key == "amount" ? &b.amount
                                           : nullptr;
            if (field == nullptr)
                continue; // a key this reader does not know, whatever its value: ignored, not a reason to drop the band
            char* end = nullptr;
            const double v = std::strtod (value.c_str(), &end);
            if (end == value.c_str() || !std::isfinite (v))
            {
                valid = false;
                continue;
            }
            *field = v;
        }
        if (!valid || !(b.freq > 0.0) || !(b.q > 0.0) || !(b.loHz > 0.0) || !(b.hiHz > b.loHz))
            continue;
        b.freq = std::clamp (b.freq, limits::kMinFreqHz, limits::kMaxFreqHz);
        b.gainDb = std::clamp (b.gainDb, -limits::kMaxGainDb, limits::kMaxGainDb);
        b.q = std::clamp (b.q, limits::kMinQ, limits::kMaxQ);
        b.slopeDbPerOct = std::clamp (b.slopeDbPerOct, limits::kMinSlopeDbPerOct, limits::kMaxSlopeDbPerOct);
        b.loHz = std::clamp (b.loHz, limits::kMinFreqHz, limits::kMaxFreqHz);
        b.hiHz = std::clamp (b.hiHz, limits::kMinFreqHz, limits::kMaxFreqHz);
        b.amount = std::clamp (b.amount, 0.0, 1.0);
        if (b.hiHz > b.loHz)
            out.push_back (b);
    }
    return header;
}

// False when the text is not a Pantheon list or holds no limiter line. A limiter line with a number that is not finite, or a ceiling above 0 dBTP,
// is read without that number; the last limiter line wins.
inline bool decodeLimiter (const std::string& text, LinkLimiter& out)
{
    out = {};
    std::istringstream in (text);
    std::string line;
    bool header = false, firstLine = true;
    while (std::getline (in, line))
    {
        trimLine (line, firstLine);
        firstLine = false;
        if (!header)
        {
            if (line.empty())
                continue;
            if (line != kHeader)
                return false;
            header = true;
            continue;
        }
        std::istringstream words (line);
        std::string kind;
        if (!(words >> kind) || kind != "limiter")
            continue;
        LinkLimiter l;
        std::string pair;
        while (words >> pair)
        {
            const auto eq = pair.find ('=');
            if (eq == std::string::npos)
                continue;
            const auto key = pair.substr (0, eq), value = pair.substr (eq + 1);
            char* end = nullptr;
            const double v = std::strtod (value.c_str(), &end);
            if (end == value.c_str() || !std::isfinite (v))
                continue;
            if (key == "ceiling" && v <= 0.0 && v >= -60.0)
            {
                l.hasCeiling = true;
                l.ceilingDb = v;
            }
            else if (key == "gainchange" && std::fabs (v) <= 60.0)
            {
                l.hasGainChange = true;
                l.gainChangeDb = v;
            }
            else if (key == "amount")
                l.amount = std::clamp (v, 0.0, 1.0);
        }
        if (l.valid())
            out = l;
    }
    return out.valid();
}

// False when the text is not a Pantheon list or holds no width line with a number in it. The keys are read in the order of
// pdsp::bands; a number that is not finite is left out, one outside the limits is clamped. The last width line wins.
inline bool decodeWidth (const std::string& text, LinkWidth& out)
{
    out = {};
    std::istringstream in (text);
    std::string line;
    bool header = false, firstLine = true;
    while (std::getline (in, line))
    {
        trimLine (line, firstLine);
        firstLine = false;
        if (!header)
        {
            if (line.empty())
                continue;
            if (line != kHeader)
                return false;
            header = true;
            continue;
        }
        std::istringstream words (line);
        std::string kind;
        if (!(words >> kind) || kind != "width")
            continue;
        LinkWidth w;
        std::string pair;
        while (words >> pair)
        {
            const auto eq = pair.find ('=');
            if (eq == std::string::npos)
                continue;
            const auto key = pair.substr (0, eq), value = pair.substr (eq + 1);
            size_t index = bands::kCount;
            for (size_t i = 0; i < bands::kCount; ++i)
                if (key == kWidthKeys[i])
                    index = i;
            if (index == bands::kCount)
                continue; // a band this reader does not know: ignored
            char* end = nullptr;
            const double v = std::strtod (value.c_str(), &end);
            if (end == value.c_str() || !std::isfinite (v))
                continue;
            w.percent[index] = std::clamp (v, limits::kMinWidthPercent, limits::kMaxWidthPercent);
        }
        if (w.valid())
            out = w;
    }
    return out.valid();
}
} // namespace link
} // namespace pdsp
