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

// Standalone tests of the living waters (tests/run_tests.sh builds and runs them): the shared rules and fight
// (src/fishing/EfFishRules.h, EfFishFight.h: verbatim copies of the client's) against the golden vectors the client's
// tests assert too, the bounds (a scripted angler passes, a too-fast, over-break or under-pull report fails, the
// legendary's phases), the kind F messages (EfFishWire.h) with a decode fuzz, and the bite roll (EfFishLogic.h: junk,
// gates, refusals, no bait, the shoal, the legendary's gate). No test pins a species of the content.

#include "EfFishLogic.h"
#include "EfFishWire.h"

#include <cstdio>
#include <iostream>
#include <random>
#include <string>

using namespace effish;
namespace F = Emberfall::Fishing;

namespace
{
    int g_failed = 0, g_checks = 0;

    void check(bool ok, std::string const& what)
    {
        ++g_checks;
        if (!ok)
        {
            ++g_failed;
            std::cerr << "FAIL: " << what << "\n";
        }
    }

    // --- the golden vectors (the same numbers in the client's tests) ----------------------------------------------
    Style const kDarter{1, StyleKind::Darter, 450, 1200, 1500, 3000, 250, 3000, 8000};
    Style const kBulldog{2, StyleKind::Bulldog, 700, 800, 4000, 7000, 350, 3000, 8000};
    Style const kSnagger{3, StyleKind::Snagger, 550, 1000, 7000, 12000, 0, 3000, 8000};
    Style styleOf(uint32_t id) { return id == 1 ? kDarter : id == 2 ? kBulldog : kSnagger; }

    // the scripted angler: give through a snag, steer against a run, give through warnings and surges, else keep mid-band
    Input bot(Fight const& f)
    {
        Input in;
        auto t = f.tension();
        auto const& p = f.params();
        Move m = f.move();
        if (m == Move::Snagged) { in.give = true; return in; }
        if (m == Move::Run) in.steer = static_cast<int8_t>(-f.heading());
        if (m == Move::JumpWarn) { if (t > 550) in.give = true; return in; }
        if (m == Move::SurgeWarn || m == Move::Surge) { if (t > p.safeLo) in.give = true; return in; }
        if (t < (p.safeLo + p.safeHi) / 2) in.reel = true;
        else if (t > p.safeHi + 50) in.give = true;
        return in;
    }

    template <typename Policy>
    EndReport Run(FightParams const& p, Policy policy)
    {
        Fight f(p);
        while (f.Step(policy(f)) == Outcome::None) {}
        return f.report();
    }

    Species Uncommon(uint32_t style)
    {
        Species s;
        s.id = style;
        s.rarity = Rarity::Uncommon;
        s.style = style;
        s.sizeTypMm = 620;
        s.sizeMaxMm = 1180;
        return s;
    }

    Species OldInch()
    {
        Species s;
        s.id = 9;
        s.rarity = Rarity::Legendary;
        s.sizeTypMm = 2100;
        s.sizeMaxMm = 2100;
        s.phases = {Phase{2, 600}, Phase{3, 250}, Phase{1, 0}};
        return s;
    }

    void TestGolden()
    {
        struct Want { char outcome; uint32_t dur, peak, inputs, fastest; char idle; uint32_t idleDur; };
        Want const want[3] = {{'L', 18400, 572, 18, 10050, 'L', 34850},
                              {'L', 34950, 739, 27, 11550, 'S', 7350},
                              {'L', 20550, 580, 17, 10700, 'L', 36000}};
        for (uint32_t style = 1; style <= 3; ++style)
        {
            Species const s = Uncommon(style);
            FightParams const p = MakeParams(s, styleOf, 700, SafeBand(100, 30), 11000, 42);
            EndReport const r = Run(p, bot);
            Want const& w = want[style - 1];
            std::string const tag = "golden style " + std::to_string(style);
            check(char(r.outcome) == w.outcome && r.durationMs == w.dur && r.peakTension == w.peak && r.over900Ms == 0 &&
                      r.inputSeconds == w.inputs,
                  tag + ": " + char(r.outcome) + " " + std::to_string(r.durationMs) + " peak " + std::to_string(r.peakTension) +
                      " inputs " + std::to_string(r.inputSeconds));
            check(FastestLanding(p).landMs == w.fastest, tag + " fastest " + std::to_string(FastestLanding(p).landMs));
            check(CheckEnd(p, s.rarity, r, r.durationMs) == Verdict::Ok, tag + " verdict");
            EndReport const idle = Run(p, [](Fight const&) { return Input{}; });
            check(char(idle.outcome) == w.idle && idle.durationMs == w.idleDur,
                  tag + " idle: " + char(idle.outcome) + " " + std::to_string(idle.durationMs));
        }

        Species const inch = OldInch();
        FightParams const p = MakeParams(inch, styleOf, 2100, SafeBand(125, 30), 12000, 7);
        EndReport const r = Run(p, bot);
        auto const mins = PhaseMinimums(p);
        check(r.outcome == Outcome::Landed && r.durationMs == 75850 && r.phaseEndMs[0] == 27750 && r.phaseEndMs[1] == 42150,
              "golden legendary: " + std::to_string(r.durationMs) + " " + std::to_string(r.phaseEndMs[0]) + "/" +
                  std::to_string(r.phaseEndMs[1]));
        check(mins[0] == 16850 && mins[1] == 11750, "golden phase minimums");
        check(CheckEnd(p, inch.rarity, r, r.durationMs) == Verdict::Ok, "golden legendary verdict");

        Rng rng(7);
        uint32_t const sizes[5] = {550, 630, 404, 701, 452};
        for (uint32_t want : sizes)
            check(PickSizeMm(rng, 620, 1180) == want, "golden size " + std::to_string(want));
        check(WeightGrams(690, 840) == 4089, "golden weight");

        Species pike;
        pike.id = 4;
        pike.rarity = Rarity::Uncommon;
        pike.clue = Clue::Swirl;
        pike.waters = {2};
        pike.habitatPermil = {0, 400, 0, 1000, 600, 0};
        pike.timeMask = kDawn | kDay | kDusk;
        uint64_t const act = Activity(pike, 2, kReeds | kShallows, kDay, kClear);
        check(act == 30000000, "golden activity");
        std::string shows;
        for (int cx = 0; cx < 10; ++cx)
            shows += ClueShows(pike, CellKey(1, cx, -500), 490000, act) ? '1' : '0';
        check(shows == "0000010000", "golden clues " + shows);
        check(CellKey(1, -3, 7) == 0x1fffffd000007ull, "golden cell key");
        check(TimeOfHour(19) == kDusk && WeatherClassOf(4) == kRain && WeatherBits(kClear, 600) == 17, "golden buckets");
    }

    // --- the rules ------------------------------------------------------------------------------------------------
    void TestRules()
    {
        check(TimeOfHour(5) == kDawn && TimeOfHour(7) == kDawn && TimeOfHour(8) == kDay && TimeOfHour(17) == kDay &&
                  TimeOfHour(18) == kDusk && TimeOfHour(20) == kDusk && TimeOfHour(21) == kNight && TimeOfHour(4) == kNight &&
                  TimeOfHour(0) == kNight && TimeOfHour(-1) == kNight,
              "time buckets");
        check(WeatherClassOf(0) == kClear && WeatherClassOf(1) == kClear && WeatherClassOf(5) == kRain &&
                  WeatherClassOf(7) == kSnow && WeatherClassOf(86) == kStorm,
              "weather classes");
        check(WeatherBits(kClear, kAfterRainSeconds + 1) == kClear && WeatherBits(kRain, 0) == kRain, "after rain only when clear");
        check(CellOf(-0.1f) == -1 && CellOf(7.99f) == 0 && CellOf(8.0f) == 1, "cells floor");
        check(CellKey(1, 0, 0) != CellKey(0, 0, 0) && CellKey(1, -1, 0) != CellKey(1, 0, -1), "cell keys differ");
        check(BaitPermil(Rarity::Common, -1) == 1000 && BaitPermil(Rarity::Uncommon, -1) == 500 &&
                  BaitPermil(Rarity::Rare, -1) == 0 && BaitPermil(Rarity::Legendary, -1) == 0 && BaitPermil(Rarity::Rare, 1500) == 1500,
              "no bait halves uncommons, refuses rares");
        check(PassesGate(Rarity::Common, 1, 30) && !PassesGate(Rarity::Uncommon, 74, 30) && PassesGate(Rarity::Uncommon, 75, 30) &&
                  !PassesGate(Rarity::Rare, 124, 30) && PassesGate(Rarity::Rare, 125, 30) && PassesGate(Rarity::Legendary, 125, 30),
              "gates at base + 45, + 95");
        Band const b0 = SafeBand(30, 30), b1 = SafeBand(55, 30), bMax = SafeBand(500, 30);
        check(b0.lo == 350 && b0.hi == 650 && b1.lo == 340 && b1.hi == 660 && bMax.hi - bMax.lo == 450, "safe band widens to 15 %");
        check(SizeFactorPermil(1180, 620, 1180) == 1400 && SizeFactorPermil(341, 620, 1180) == 800, "size factor ends");
        Species s;
        s.boosts = {Boost{kDusk, 0, 1500}, Boost{0, kRain | kAfterRain, 2000}, Boost{}};
        check(BoostPermil(s, kDusk, kClear) == 1500 && BoostPermil(s, kDusk, kAfterRain | kClear) == 3000 &&
                  BoostPermil(s, kDay, kRain) == 2000 && BoostPermil(s, kDay, kClear) == 1000,
              "boosts multiply when they match");
        Rng r1(99), r2(99);
        for (int i = 0; i < 100; ++i)
            check(r1.Next() == r2.Next(), "rng deterministic");
    }

    // --- the bounds ---------------------------------------------------------------------------------------------------
    void TestBounds()
    {
        Species const s = Uncommon(2);
        FightParams const p = MakeParams(s, styleOf, 700, SafeBand(100, 30), 11000, 42);
        EndReport good = Run(p, bot);
        check(CheckEnd(p, s.rarity, good, good.durationMs) == Verdict::Ok, "the scripted angler passes");
        // the fight is deterministic: the same seed and inputs give the same report
        EndReport again = Run(p, bot);
        check(again.durationMs == good.durationMs && again.peakTension == good.peakTension, "deterministic");

        uint32_t const fastest = FastestLanding(p).landMs;
        EndReport fast = good;
        fast.durationMs = fastest - 1000;
        check(CheckEnd(p, s.rarity, fast, fastest - 1000) == Verdict::TooFast, "landed faster than possible");
        check(CheckEnd(p, s.rarity, good, fastest - 1000) == Verdict::TooFast, "the server's clock says too fast");
        check(CheckEnd(p, s.rarity, good, p.maxMs + 10001) == Verdict::TooSlow, "past the maximum");
        EndReport brk = good;
        brk.peakTension = 1000;
        check(CheckEnd(p, s.rarity, brk, brk.durationMs) == Verdict::Break, "a peak at the break");
        EndReport under = good;
        under.peakTension = 10;
        check(CheckEnd(p, s.rarity, under, under.durationMs) == Verdict::UnderPull, "a peak under the fish's pull");
        EndReport over = good;
        over.over900Ms = 60000;
        check(CheckEnd(p, s.rarity, over, over.durationMs) == Verdict::Over900, "too long over 900");
        EndReport lost = good;
        lost.outcome = Outcome::Snapped;
        lost.durationMs = 100;
        lost.peakTension = 5000;
        check(CheckEnd(p, s.rarity, lost, 100) == Verdict::Ok, "a loss is taken as it is");
        EndReport bad = good;
        bad.outcome = static_cast<Outcome>('Q');
        check(CheckEnd(p, s.rarity, bad, bad.durationMs) == Verdict::Outcome, "not an outcome");

        // a rare landed with nobody at the controls
        Species rare = Uncommon(1);
        rare.rarity = Rarity::Rare;
        FightParams const rp = MakeParams(rare, styleOf, 700, SafeBand(130, 30), 11000, 5);
        EndReport rr = Run(rp, bot);
        check(rr.outcome == Outcome::Landed && CheckEnd(rp, rare.rarity, rr, rr.durationMs) == Verdict::Ok, "a rare, played, passes");
        EndReport idle = rr;
        idle.inputSeconds = 0;
        check(CheckEnd(rp, rare.rarity, idle, idle.durationMs) == Verdict::Inputs, "a rare landed with no inputs");
        check(!rp.passiveRecovers, "a rare does not come in by itself");

        // the legendary's phases
        Species const inch = OldInch();
        FightParams const lp = MakeParams(inch, styleOf, 2100, SafeBand(125, 30), 12000, 7);
        EndReport const lr = Run(lp, bot);
        EndReport swapped = lr;
        swapped.phaseEndMs = {lr.phaseEndMs[1], lr.phaseEndMs[0], 0};
        check(CheckEnd(lp, inch.rarity, swapped, lr.durationMs) == Verdict::Phases, "phases out of order");
        EndReport shortPhase = lr;
        shortPhase.phaseEndMs = {1000, lr.phaseEndMs[1], 0};
        check(CheckEnd(lp, inch.rarity, shortPhase, lr.durationMs) == Verdict::Phases, "a phase too short");
        EndReport missing = lr;
        missing.phaseEndMs = {0, 0, 0};
        check(CheckEnd(lp, inch.rarity, missing, lr.durationMs) == Verdict::Phases, "phases missing");

        // many scripted fights over seeds and sizes: an honest landing always passes
        int landed = 0, rejected = 0;
        for (uint32_t i = 0; i < 300; ++i)
        {
            Species sp = Uncommon(1 + i % 3);
            Rng r(1000 + i);
            uint32_t const size = PickSizeMm(r, sp.sizeTypMm, sp.sizeMaxMm);
            FightParams const fp = MakeParams(sp, styleOf, size, SafeBand(int(30 + i % 200), 30), 7000 + 30 * i, r.Next());
            EndReport const er = Run(fp, bot);
            if (er.outcome == Outcome::Landed)
            {
                ++landed;
                if (CheckEnd(fp, sp.rarity, er, er.durationMs) != Verdict::Ok)
                    ++rejected;
            }
        }
        check(landed > 280 && rejected == 0, "scripted fights land and pass: " + std::to_string(landed) + " landed, " +
                                               std::to_string(rejected) + " rejected");
    }

    // --- the messages -------------------------------------------------------------------------------------------------
    void TestWire()
    {
        wire::Open o;
        o.fight = 7;
        o.bobber = 0xF110000000001234ull;
        o.species = 9;
        o.sizeMm = 2100;
        o.params = MakeParams(OldInch(), styleOf, 2100, SafeBand(125, 30), 12000, 0x1a2b3c4d5e6f7788ull);
        o.styleIds = {2, 3, 1};
        std::string const body = wire::EncodeOpen(12, o);
        check(!body.empty() && body.size() + 4 <= efenc::kMaxBytes, "OPEN fits");
        auto m = efenc::Decode(body);
        auto o2 = m ? wire::DecodeOpen(*m) : std::nullopt;
        check(o2 && o2->bobber == o.bobber && o2->params.seed == o.params.seed && o2->params.phaseCount == 3 &&
                  o2->styleIds[1] == 3 && o2->params.untilPermil[0] == 600 && o2->params.safeLo == o.params.safeLo &&
                  o2->params.maxMs == o.params.maxMs && o2->params.passiveRecovers == o.params.passiveRecovers,
              "OPEN round trip");

        wire::End e;
        e.fight = 7;
        e.report.outcome = Outcome::Landed;
        e.report.durationMs = 30000;
        e.report.peakTension = 640;
        e.report.phaseEndMs = {1000, 2000, 0};
        auto e2 = wire::DecodeEnd(*efenc::Decode(wire::EncodeEnd(13, e)));
        check(e2 && e2->report.outcome == Outcome::Landed && e2->report.peakTension == 640 && e2->report.phaseEndMs[1] == 2000,
              "END round trip");
        check(!wire::DecodeEnd(*efenc::Decode("1;1;F;END;7;Q;1;1;1;1;0/0/0")), "END with a bad outcome");
        check(!wire::DecodeEnd(*efenc::Decode("1;1;F;END;7;L;1;1;1;1;0/0")), "END with two phases");

        wire::Result r;
        r.fight = 7;
        r.ok = true;
        r.item = 99264;
        r.sizeMm = 840;
        r.grams = 4100;
        r.best = true;
        auto r2 = wire::DecodeResult(*efenc::Decode(wire::EncodeResult(1, r)));
        check(r2 && r2->ok && r2->grams == 4100 && r2->best, "RESULT OK round trip");
        r.ok = false;
        r.code = "FAST";
        r2 = wire::DecodeResult(*efenc::Decode(wire::EncodeResult(1, r)));
        check(r2 && !r2->ok && r2->code == "FAST", "RESULT REJ round trip");
        auto c = wire::DecodeClose(*efenc::Decode(wire::EncodeClose(1, {7, "MOVED"})));
        check(c && c->reason == "MOVED", "CLOSE round trip");
        check(efenc::Decode(wire::EncodeHello(1, 0xdeadbeefcafef00dull))->fields[0] == "deadbeefcafef00d", "HELLO");

        // fuzz: mutated OPEN and END bodies never crash
        std::mt19937 rng(77);
        std::string const seeds[2] = {body, wire::EncodeEnd(13, e)};
        for (int i = 0; i < 20000; ++i)
        {
            std::string s = seeds[i % 2];
            int const edits = 1 + int(rng() % 4);
            for (int k = 0; k < edits && !s.empty(); ++k)
            {
                std::size_t const at = rng() % s.size();
                switch (rng() % 3)
                {
                    case 0: s[at] = char("0123456789;/:afLX"[rng() % 17]); break;
                    case 1: s.erase(at, 1); break;
                    default: s.insert(at, 1, char("0123456789;/:"[rng() % 13])); break;
                }
            }
            if (auto d = efenc::Decode(s))
            {
                (void)wire::DecodeOpen(*d);
                (void)wire::DecodeEnd(*d);
                (void)wire::DecodeResult(*d);
                (void)wire::DecodeClose(*d);
            }
        }
        check(true, "fuzz survived");
    }

    // --- the roll -----------------------------------------------------------------------------------------------------
    F::Data TestData()
    {
        F::Data d;
        d.styles[1] = kDarter;
        d.styles[2] = kBulldog;
        Species common;
        common.id = 1;
        common.item = 1001;
        common.rarity = Rarity::Common;
        common.sizeTypMm = 200;
        common.sizeMaxMm = 400;
        common.waters = {1};
        common.habitatPermil = {1000, 0, 0, 1000, 0, 0};   // shallows, reeds
        Species unc = common;
        unc.id = 2;
        unc.item = 1002;
        unc.rarity = Rarity::Uncommon;
        unc.style = 2;
        unc.habitatPermil = {0, 0, 0, 1000, 0, 0};          // reeds
        Species rare = unc;
        rare.id = 3;
        rare.item = 1003;
        rare.rarity = Rarity::Rare;
        rare.style = 1;
        rare.timeMask = kNight;
        Species legend = unc;
        legend.id = 4;
        legend.item = 1004;
        legend.rarity = Rarity::Legendary;
        legend.sizeTypMm = legend.sizeMaxMm = 2100;
        legend.gateQuest = 500;
        legend.needsGroundBait = true;
        d.species = {common, unc, rare, legend};
        d.likes[{2, 7001}] = 0;      // the uncommon refuses bait 7001
        d.likes[{3, 7002}] = 2000;   // the rare loves bait 7002
        return d;
    }

    std::map<uint32_t, int> Tally(F::Data const& d, F::RollInput const& in, int n = 4000)
    {
        std::map<uint32_t, int> out;
        Rng rng(31337);
        for (int i = 0; i < n; ++i)
        {
            F::RollResult r = F::Roll(d, in, rng);
            ++out[r.kind == F::RollKind::Junk ? 0u : r.species->id];
        }
        return out;
    }

    void TestRoll()
    {
        F::Data const d = TestData();
        F::RollInput in;
        in.water = 1;
        in.mask = kReeds;
        in.timeBit = kDay;
        in.skill = 200;
        in.zoneBase = 30;

        check(F::CatchChance(125, 30) == 100 && F::CatchChance(63, 30) == 25 && F::CatchChance(1, 30) == 1, "the core's junk chance");

        auto t = Tally(d, in);
        check(t[0] == 0 && t[1] > 0 && t[2] > 0 && t[3] == 0 && t[4] == 0, "day, no bait: commons and uncommons, no rare, no legend");
        // no bait halves the uncommon: 100 : 15
        check(t[2] * 100 / t[1] > 10 && t[2] * 100 / t[1] < 21, "no bait halves the uncommon");

        F::RollInput night = in;
        night.timeBit = kNight;
        night.baitItem = 7002;
        auto tn = Tally(d, night);
        check(tn[3] > 0, "the rare bites at night with a bait");
        night.skill = 120;
        check(Tally(d, night)[3] == 0, "under the rare's gate");
        night.skill = 200;
        night.baitItem = 0;
        check(Tally(d, night)[3] == 0, "no bait refuses the rare");

        F::RollInput refused = in;
        refused.baitItem = 7001;
        check(Tally(d, refused)[2] == 0, "a refused bait");
        F::RollInput low = in;
        low.skill = 60;
        auto tl = Tally(d, low);
        check(tl[2] == 0 && tl[0] > 0, "under the uncommon's gate; low skill brings junk");

        F::RollInput dry = in;
        dry.water = 2;
        check(Tally(d, dry)[0] == 4000, "no species in this water: junk");

        F::RollInput shoal = low;
        shoal.shoal = true;
        shoal.shoalItem = 1001;
        check(Tally(d, shoal)[0] == 0, "a shoal brings no junk");
        F::RollInput sh = in;
        sh.shoal = true;
        sh.shoalItem = 1002;
        auto ts = Tally(d, sh);
        auto tp = Tally(d, in);
        check(ts[2] * tp[1] > tp[2] * ts[1] * 2, "a shoal triples its own fish");

        // the legendary
        F::RollInput leg = in;
        leg.legend = [](Species const&) { return F::Legend::Questing; };
        leg.baitItem = 7003;
        auto tq = Tally(d, leg);
        check(tq[4] > 4000 / 3 - 250 && tq[4] < 4000 / 3 + 250, "on his quest: about one bite in three");
        leg.baitItem = 0;
        check(Tally(d, leg)[4] == 0, "no bait refuses the legendary");
        leg.baitItem = 7003;
        leg.legend = [](Species const&) { return F::Legend::Closed; };
        check(Tally(d, leg)[4] == 0, "closed: never");
        leg.legend = [](Species const&) { return F::Legend::Open; };
        auto to = Tally(d, leg);
        check(to[4] > 0 && to[4] < to[2], "after the chain: a rare-like candidate");
        leg.skill = 100;
        leg.legend = [](Species const&) { return F::Legend::Questing; };
        check(Tally(d, leg)[4] == 0, "the legendary's gate");

        F::RollInput forced = low;
        forced.forced = 2;
        check(Tally(d, forced, 10)[2] == 10, "forced");

        // sizes stay in their range
        Rng rng(4);
        for (int i = 0; i < 2000; ++i)
        {
            F::RollResult r = F::Roll(d, in, rng);
            if (r.species)
                check(r.sizeMm >= r.species->sizeTypMm * 55 / 100 && r.sizeMm <= r.species->sizeMaxMm, "size in range");
        }

        F::Stats st;
        bool logged = false;
        for (int i = 0; i < 160; ++i)
            logged = st.Count(1000) || logged;
        check(logged && st.hourFights == 160, "over 150 an hour is logged");
        check(!st.Count(5000) && st.hourFights == 1, "a new hour");
    }
}

int main()
{
    TestGolden();
    TestRules();
    TestBounds();
    TestWire();
    TestRoll();
    if (g_failed)
    {
        std::cerr << "fishing_tests: " << g_failed << " of " << g_checks << " checks failed\n";
        return 1;
    }
    std::cout << "fishing_tests: " << g_checks << " checks passed\n";
    return 0;
}
