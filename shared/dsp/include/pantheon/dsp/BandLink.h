// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 The Pantheon authors

#pragma once

#include <cmath>
#include <cstdio>
#include <cstdlib>
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
//   fill lo=40 hi=90
//
// Unknown keys are ignored and a band with an unknown kind is skipped, so a newer writer does not break an older reader.
struct LinkBand
{
    enum class Kind { bell, lowShelf, highShelf, lowCut, highCut, fill };
    enum class Place { stereo, left, right, mid, side };

    Kind kind = Kind::bell;
    Place place = Place::stereo;
    double freq = 1000.0, gainDb = 0.0, q = 0.7071; // equaliser bands; a cut's q is unused, its slope is `slopeDbPerOct`
    double slopeDbPerOct = 24.0;
    double loHz = 40.0, hiHz = 120.0;                  // a fill band's range
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

inline std::string encode (const std::vector<LinkBand>& bands)
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
    }
    return out;
}

// False when the text is not a band list (no header); otherwise the bands it could read, in order.
inline bool decode (const std::string& text, std::vector<LinkBand>& out)
{
    out.clear();
    std::istringstream in (text);
    std::string line;
    bool header = false;
    while (std::getline (in, line))
    {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
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
        if (valid && b.freq > 0.0 && b.q > 0.0 && b.loHz > 0.0 && b.hiHz > b.loHz)
            out.push_back (b);
    }
    return header;
}
} // namespace link
} // namespace pdsp
