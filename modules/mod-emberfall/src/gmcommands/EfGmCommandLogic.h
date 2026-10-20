/*
 * This file is part of Emberfall's AzerothCore fork (modules/mod-emberfall). See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

// The pure parts of Emberfall's GM command fixes (src/emberfall_gmcommands.cpp; tests/gmcommand_tests.cpp): which typed
// command is `.modify speed ...` or `.die` (as the core's command tree matches it: every word a case-insensitive prefix
// of the command's name), the move types each `.modify speed` variant sets, and the per-player store of the GM's rates.
// No core headers: the move types are UnitMoveType's numbers (UnitDefines.h).

#pragma once

#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Emberfall::GmCommands
{
    // UnitMoveType (UnitDefines.h).
    constexpr int kMoveWalk = 0, kMoveRun = 1, kMoveRunBack = 2, kMoveSwim = 3, kMoveFlight = 6;
    constexpr int kMoveTypes = 9;

    // cs_modify's bounds of every `.modify speed` variant.
    constexpr float kMinRate = 0.1f, kMaxRate = 50.0f;

    // True if token is a non-empty case-insensitive prefix of name (the core's StringStartsWithI on a command word).
    inline bool IsPrefixI(std::string_view token, std::string_view name)
    {
        if (token.empty() || token.size() > name.size())
            return false;
        for (size_t i = 0; i < token.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(token[i])) != std::tolower(static_cast<unsigned char>(name[i])))
                return false;
        return true;
    }

    // The words of a command line (the leading '.' or '!' of a chat command dropped; blanks between words skipped).
    inline std::vector<std::string_view> Words(std::string_view line)
    {
        while (!line.empty() && (line.front() == '.' || line.front() == '!' || line.front() == ' '))
            line.remove_prefix(1);
        std::vector<std::string_view> words;
        while (!line.empty())
        {
            size_t end = line.find(' ');
            std::string_view w = line.substr(0, end);
            if (!w.empty())
                words.push_back(w);
            if (end == std::string_view::npos)
                break;
            line.remove_prefix(end + 1);
        }
        return words;
    }

    // A whole word as a float (cs_modify's argument), else NAN.
    inline float ParseRate(std::string_view word)
    {
        if (word.empty() || word.size() > 32)
            return NAN;
        std::string s(word);
        char* end = nullptr;
        float v = std::strtof(s.c_str(), &end);
        if (end != s.c_str() + s.size() || !std::isfinite(v))
            return NAN;
        return v;
    }

    // The `.modify speed` variants (cs_modify's modifyspeedCommandTable).
    enum class SpeedKind { All, Walk, BackWalk, Swim, Fly };

    // The move types a variant sets, as the core's handlers do: all (and the bare `.modify speed N`) walk, run, swim and
    // flight; `walk` the run speed (HandleModifySpeedCommand sets MOVE_RUN); backwalk run back; swim; fly flight.
    inline std::vector<int> MoveTypesOf(SpeedKind kind)
    {
        switch (kind)
        {
            case SpeedKind::All: return { kMoveWalk, kMoveRun, kMoveSwim, kMoveFlight };
            case SpeedKind::Walk: return { kMoveRun };
            case SpeedKind::BackWalk: return { kMoveRunBack };
            case SpeedKind::Swim: return { kMoveSwim };
            case SpeedKind::Fly: return { kMoveFlight };
        }
        return {};
    }

    enum class Command { None, Speed, Die };

    struct Parsed
    {
        Command command = Command::None;
        SpeedKind kind = SpeedKind::All;
        float rate = NAN; // Speed: the argument, NAN if it is not one number
    };

    // What a typed command line is. The core calls the module (AllCommandScript::OnTryExecuteCommand) only after its
    // tree matched every word unambiguously (an exact name, or the only name a word begins), so a word that begins
    // "modify" did match `modify` (when the invoker may see it: the caller checks the permission). Anything else, extra
    // words included, is None: the core's own handler runs (and shows its usage or error).
    inline Parsed Parse(std::string_view line)
    {
        Parsed p;
        std::vector<std::string_view> w = Words(line);
        if (w.size() == 1 && IsPrefixI(w[0], "die"))
        {
            p.command = Command::Die;
            return p;
        }
        if (w.size() < 3 || !IsPrefixI(w[0], "modify") || !IsPrefixI(w[1], "speed"))
            return p;
        size_t arg = 2;
        if (w.size() == 4)
        {
            static constexpr std::pair<std::string_view, SpeedKind> kSub[] = {
                { "all", SpeedKind::All }, { "walk", SpeedKind::Walk }, { "backwalk", SpeedKind::BackWalk },
                { "swim", SpeedKind::Swim }, { "fly", SpeedKind::Fly } };
            bool found = false;
            for (auto const& [name, kind] : kSub)
                if (IsPrefixI(w[2], name))
                {
                    p.kind = kind;
                    found = true;
                    break;
                }
            if (!found)
                return p;
            arg = 3;
        }
        else if (w.size() != 3)
            return p;
        p.rate = ParseRate(w[arg]);
        // `.modify speed walk` (a variant, no value) or `.modify speed abc`: the stock handler's usage.
        if (arg == 2 && std::isnan(p.rate))
            return p;
        p.command = Command::Speed;
        return p;
    }

    inline bool RateInBounds(float rate) { return std::isfinite(rate) && rate >= kMinRate && rate <= kMaxRate; }

    // A player's GM rates per move type (0: none, the core's own calculation stands).
    struct Rates
    {
        std::array<float, kMoveTypes> rate{};

        // Rate 1 clears the type (the core's calculation again: auras, mount); any other rate holds it.
        void Set(int type, float value)
        {
            if (type < 0 || type >= kMoveTypes)
                return;
            rate[type] = std::fabs(value - 1.0f) < 1e-4f ? 0.0f : value;
        }

        bool Holds(int type) const { return type >= 0 && type < kMoveTypes && rate[type] > 0.0f; }

        bool Empty() const
        {
            for (float r : rate)
                if (r > 0.0f)
                    return false;
            return true;
        }

        // The types whose current rate lost the GM's (a recalculation overwrote it): to set again.
        template <typename CurrentRate>
        std::vector<int> Drifted(CurrentRate&& current) const
        {
            std::vector<int> out;
            for (int t = 0; t < kMoveTypes; ++t)
                if (rate[t] > 0.0f && std::fabs(current(t) - rate[t]) > 1e-4f)
                    out.push_back(t);
            return out;
        }
    };
}
