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

// The Infusionist's arithmetic that needs no core (Emberfall docs/design/classes/mage-infusionist.md §3): the prepared
// potions (a counted resource of vials that recharge on their own, several at once, the rest in a queue, a faster rate
// while Rapid Synthesis holds, restores, Perfect Formula's free fifth potion), the EFPOT message body, the independent
// applications of a damage over time and the chain heal's percents. src/emberfall_classkit_infuse.cpp uses it;
// tests/classkit_infuse_tests.cpp (tests/run_tests.sh) checks it.
//
// The EFPOT message (an addon whisper from the player to themselves, prefix "EFPOT\t"; EmberfallUI SpecResource.lua):
//   1;<ready>;<vials>;<queued>;<pf>;<pfEvery>;<left>/<total>,<left>/<total>,...
// ready the vials at hand, vials the most (0: the potion system is off: the spec changed), queued the spent vials waiting
// for a free recharge, pf Perfect Formula's count (potions spent since the last free one) and pfEvery its period (0: no
// Perfect Formula), then one pair per vial recharging now, in ms: time left and the whole recharge at the current rate.

#ifndef EMBERFALL_INFUSE_LOGIC_H
#define EMBERFALL_INFUSE_LOGIC_H

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Emberfall::ClassKit::Infuse
{
    constexpr std::string_view kPrefix = "EFPOT\t";
    constexpr std::uint32_t kVersion = 1;

    // One vial recharging: the ms left and the whole recharge at the rate it runs at now.
    struct Recharge
    {
        std::int32_t leftMs = 0;
        std::int32_t totalMs = 0;
    };

    // A player's prepared potions. ready + recharging + queued = vials, always.
    struct Potions
    {
        std::uint32_t vials = 0;
        std::uint32_t ready = 0;
        std::vector<Recharge> recharging;   // in the order they started
        std::uint32_t queued = 0;
        std::uint32_t pf = 0;               // Perfect Formula: potions spent since the last free one
    };

    // A fresh store: every vial ready.
    inline Potions Full(std::uint32_t vials)
    {
        Potions p;
        p.vials = vials;
        p.ready = vials;
        return p;
    }

    // Of `cost` potions, how many are taken from the vials: with Perfect Formula (every `every`-th spent is free,
    // `every` 0: off) the free one is counted in order, so a 2-potion cast takes one vial when either of its potions is
    // the free one.
    inline std::uint32_t Paid(std::uint32_t cost, std::uint32_t pf, std::uint32_t every)
    {
        std::uint32_t paid = 0;
        for (std::uint32_t i = 0; i < cost; ++i)
        {
            if (every >= 2 && pf + 1 >= every)
                pf = 0;            // the free one
            else
            {
                ++paid;
                if (every >= 2)
                    ++pf;
            }
        }
        return paid;
    }

    // Whether a cast costing `cost` potions can be paid now.
    inline bool CanPay(Potions const& p, std::uint32_t cost, std::uint32_t every)
    {
        return Paid(cost, p.pf, every) <= p.ready;
    }

    // Whether the next potion is free (Perfect Formula): Restorative Flask stays a flask at 0 ready.
    inline bool NextFree(Potions const& p, std::uint32_t every)
    {
        return every >= 2 && p.pf + 1 >= every;
    }

    // Whether Restorative Flask's button is Arcane Restoration: no potion ready and the next not free.
    inline bool Depleted(Potions const& p, std::uint32_t every)
    {
        return p.ready == 0 && !NextFree(p, every);
    }

    // Starts as many queued vials as the free recharge slots allow (`limit` at once) at `rateMs`.
    inline void StartQueued(Potions& p, std::uint32_t limit, std::int32_t rateMs)
    {
        while (p.queued > 0 && p.recharging.size() < limit)
        {
            p.recharging.push_back({ rateMs, rateMs });
            --p.queued;
        }
    }

    // Spends `cost` potions (CanPay first). Returns the vials taken (the free one takes none); each starts recharging
    // while a slot is free, else waits in the queue. Perfect Formula's count advances by every potion, free or not.
    inline std::uint32_t Spend(Potions& p, std::uint32_t cost, std::uint32_t every, std::uint32_t limit, std::int32_t rateMs)
    {
        std::uint32_t paid = 0;
        for (std::uint32_t i = 0; i < cost; ++i)
        {
            if (every >= 2 && p.pf + 1 >= every)
            {
                p.pf = 0;
                continue;
            }
            if (every >= 2)
                ++p.pf;
            if (p.ready == 0)
                continue;          // not paid for (the caller checked CanPay): nothing to take
            --p.ready;
            ++p.queued;
            ++paid;
        }
        StartQueued(p, limit, rateMs);
        return paid;
    }

    // `ms` pass: the running recharges count down (never haste); every one that ends is a vial ready, and a queued vial
    // starts in its slot with the time left over. Returns the vials that came back.
    inline std::uint32_t Advance(Potions& p, std::int32_t ms, std::uint32_t limit, std::int32_t rateMs)
    {
        std::uint32_t back = 0;
        while (ms > 0 && !p.recharging.empty())
        {
            // the next to end
            std::int32_t step = ms;
            for (Recharge const& r : p.recharging)
                step = std::min(step, std::max(0, r.leftMs));
            for (Recharge& r : p.recharging)
                r.leftMs -= step;
            ms -= step;
            for (auto it = p.recharging.begin(); it != p.recharging.end();)
                if (it->leftMs <= 0)
                {
                    it = p.recharging.erase(it);
                    ++p.ready;
                    ++back;
                }
                else
                    ++it;
            StartQueued(p, limit, rateMs);
            if (step == 0 && ms > 0 && p.recharging.empty())
                break;
        }
        return back;
    }

    // The rate changes (Rapid Synthesis comes or goes): every running recharge keeps the share it has done and runs the
    // rest at the new rate (4 of 8 s done: 1.5 of 3 s left) [proposal, mage-infusionist.md §9].
    inline void Rescale(Potions& p, std::int32_t newRateMs)
    {
        for (Recharge& r : p.recharging)
        {
            if (r.totalMs <= 0)
                continue;
            std::int64_t const left = std::int64_t(r.leftMs) * newRateMs / r.totalMs;
            r.leftMs = std::int32_t(std::max<std::int64_t>(1, left));
            r.totalMs = newRateMs;
        }
    }

    // `count` spent vials are ready again at once, by the design's priority: one waiting in the queue first, else the
    // recharging one with the most time left; all ready: nothing. Returns the vials restored. A recharge slot a restore
    // frees takes the next queued vial.
    inline std::uint32_t Restore(Potions& p, std::uint32_t count, std::uint32_t limit, std::int32_t rateMs)
    {
        std::uint32_t done = 0;
        for (; done < count; ++done)
        {
            if (p.queued > 0)
                --p.queued;
            else if (!p.recharging.empty())
            {
                auto most = std::max_element(p.recharging.begin(), p.recharging.end(),
                    [](Recharge const& a, Recharge const& b) { return a.leftMs < b.leftMs; });
                p.recharging.erase(most);
            }
            else
                break;
            ++p.ready;
        }
        StartQueued(p, limit, rateMs);
        return done;
    }

    // Perfect Formula's count after `restored` potions came back by Immediate Preparation: each advances it by one, but
    // never past the free one (it stops at "the next is free"; a restore never spends it) [proposal, §9].
    inline std::uint32_t AdvanceFormula(std::uint32_t pf, std::uint32_t restored, std::uint32_t every)
    {
        if (every < 2)
            return 0;
        return std::min<std::uint32_t>(every - 1, pf + restored);
    }

    // The EFPOT body.
    inline std::string Message(Potions const& p, std::uint32_t every)
    {
        std::string out = std::to_string(kVersion) + ";" + std::to_string(p.ready) + ";" + std::to_string(p.vials) + ";"
            + std::to_string(p.queued) + ";" + std::to_string(every >= 2 ? p.pf : 0) + ";" + std::to_string(every >= 2 ? every : 0)
            + ";";
        bool first = true;
        for (Recharge const& r : p.recharging)
        {
            if (!first)
                out += ",";
            first = false;
            out += std::to_string(std::max(0, r.leftMs)) + "/" + std::to_string(std::max(0, r.totalMs));
        }
        return out;
    }

    // The message when the potion system is off for the player (the spec changed): no vials.
    inline std::string OffMessage()
    {
        return std::to_string(kVersion) + ";0;0;0;0;0;";
    }

    // What the player is shown changes (a vial ready or spent, the queue, a recharge started or rescaled, the count):
    // the key of a state without the running times.
    inline std::string Shape(Potions const& p, std::uint32_t every)
    {
        std::string out = std::to_string(p.ready) + ":" + std::to_string(p.queued) + ":" + std::to_string(p.pf) + ":"
            + std::to_string(every) + ":";
        for (Recharge const& r : p.recharging)
            out += std::to_string(r.totalMs) + ",";
        return out;
    }

    // ---- Volatile Flask: every application its own stack and timer ----
    // The applications' end times (ms) at `nowMs`: the ones that ran out go. Returns how many are left.
    inline std::uint32_t Prune(std::vector<std::uint64_t>& ends, std::uint64_t nowMs)
    {
        ends.erase(std::remove_if(ends.begin(), ends.end(), [nowMs](std::uint64_t e) { return e <= nowMs; }), ends.end());
        return std::uint32_t(ends.size());
    }

    // The ms the aura still has: the latest application's end.
    inline std::int32_t LongestLeft(std::vector<std::uint64_t> const& ends, std::uint64_t nowMs)
    {
        std::uint64_t last = 0;
        for (std::uint64_t e : ends)
            last = std::max(last, e);
        return last > nowMs ? std::int32_t(last - nowMs) : 0;
    }

    // ---- Flowing Remedy ----
    // The percent of the heal the chain's `jump`-th heal lands at (jump 0: the first target): Restorative Momentum's
    // `momentum` percent more each jump (100, 110, 120, ...); the rebound counts as the next jump [proposal, §9].
    inline std::int32_t JumpPct(std::uint32_t jump, std::int32_t momentum)
    {
        return 100 + std::int32_t(jump) * std::max(0, momentum);
    }
}

#endif
