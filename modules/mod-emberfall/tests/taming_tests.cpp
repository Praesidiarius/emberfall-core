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

// Standalone tests of hunter taming (tests/run_tests.sh builds and runs them): the server-driven EFN messages
// (src/encounters/EfEncounterWire.h's BARS, SLOTS, PUT, TAKE and src/taming/EfTameWire.h, both the client's verbatim
// copies) with round trips and a decode fuzz, and src/taming/EfTameLogic.h: the bars by level and rank, the arrow pick,
// Drowse and its decay, the claim, Out Cold and the draughts' cap, the food's Trust by family and band, the eating order,
// the slots. Made-up items and families: the numbers are the logic's, not the content's.

#include "EfEncounterLogic.h"
#include "EfEncounterWire.h"
#include "EfTameLogic.h"
#include "EfTameWire.h"

#include <iostream>
#include <random>
#include <string>

using namespace Emberfall::Taming;
namespace E = Emberfall::Encounters;

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

    std::optional<efenc::Message> RoundTrip(std::string const& body)
    {
        check(!body.empty(), "encodes");
        return efenc::Decode(body);
    }

    void TestWire()
    {
        efenc::Bars b{7, 0xF130005800001234ull, 2, 41500, eftame::wire::kMine, {{30000, 60000}, {25000, 100000}}};
        auto m = RoundTrip(efenc::EncodeBars(3, efenc::kTaming, b));
        auto back = m ? efenc::DecodeBars(*m) : std::nullopt;
        check(back && back->id == 7 && back->subject == b.subject && back->phase == 2 && back->leftMs == 41500 &&
                  back->flags == 1 && back->bars.size() == 2 && back->bars[1].value == 25000 && back->bars[1].max == 100000,
              "BARS round trip");
        efenc::Bars none{0, 1, 0, 0, 0, {}};
        m = RoundTrip(efenc::EncodeBars(4, efenc::kTaming, none));
        check(m && efenc::DecodeBars(*m) && efenc::DecodeBars(*m)->bars.empty(), "BARS with no bars (phase 0)");
        check(efenc::EncodeBars(1, 'T', {1, 1, 1, 0, 0, {{1, 2}, {1, 2}, {1, 2}, {1, 2}}}).empty(), "at most three bars");

        efenc::Slots s{7, {{900001, 3}, {0, 0}, {900002, 1}, {0, 0}, {0, 0}, {900003, 20}}};
        m = RoundTrip(efenc::EncodeSlots(5, efenc::kTaming, s));
        auto sb = m ? efenc::DecodeSlots(*m) : std::nullopt;
        check(sb && sb->slots.size() == 6 && sb->slots[0].item == 900001 && sb->slots[0].count == 3 && sb->slots[5].count == 20,
              "SLOTS round trip");

        m = RoundTrip(efenc::EncodePut(6, efenc::kTaming, {7, 5, 4, 16, 20}));
        auto put = m ? efenc::DecodePut(*m) : std::nullopt;
        check(put && put->id == 7 && put->slot == 5 && put->bag == 4 && put->bagSlot == 16 && put->count == 20, "PUT round trip");
        m = RoundTrip(efenc::EncodeTake(7, efenc::kTaming, {7, 2}));
        auto take = m ? efenc::DecodeTake(*m) : std::nullopt;
        check(take && take->id == 7 && take->slot == 2, "TAKE round trip");
        check(!efenc::DecodePut(*efenc::Decode("1;1;T;PUT;7;5;4;x;20")), "a bad PUT field is refused");

        namespace w = eftame::wire;
        m = RoundTrip(w::EncodeOpenRequest(8, 0xF130005800001234ull));
        check(m && w::DecodeOpenRequest(*m) == 0xF130005800001234ull, "the open request round trip");
        m = RoundTrip(w::EncodeOpen(9, {7, 0xF130005800001234ull, 2, 90207, 12, 4, 6}));
        auto open = m ? w::DecodeOpen(*m) : std::nullopt;
        check(open && open->id == 7 && open->family == 2 && open->entry == 90207 && open->level == 12 && open->rank == 4 &&
                  open->slots == 6,
              "OPEN round trip");
        check(m && !w::DecodeOpenRequest(*m), "an OPEN is not an open request");
        m = RoundTrip(w::EncodeEat(10, {7, 900001, w::kFavourite, 25000}));
        auto eat = m ? w::DecodeEat(*m) : std::nullopt;
        check(eat && eat->taste == 'F' && eat->amount == 25000, "EAT round trip");
        check(w::EncodeEat(1, {7, 1, 'Z', 1}).empty(), "an unknown taste is not sent");
        m = RoundTrip(w::EncodeResult(11, {0, 0xABCDull, w::kTamed}));
        auto res = m ? w::DecodeResult(*m) : std::nullopt;
        check(res && res->id == 0 && res->beast == 0xABCD && res->outcome == 'T', "RESULT round trip");
        m = RoundTrip(w::EncodeClose(12, {7, "RANGE"}));
        auto cl = m ? w::DecodeClose(*m) : std::nullopt;
        check(cl && cl->reason == "RANGE", "CLOSE with a reason");
        m = RoundTrip(w::EncodeClose(13, {7, ""}));
        cl = m ? w::DecodeClose(*m) : std::nullopt;
        check(cl && cl->id == 7 && cl->reason.empty(), "the client's CLOSE");

        // a fuzz: nothing decodes into a crash, every decoder refuses garbage or returns sane sizes
        std::mt19937 rng(372);
        char const alphabet[] = "0123456789abcdefT;/:FBARSLOTPUEKCD";
        for (int i = 0; i < 20000; ++i)
        {
            std::string body = "1;1;T;";
            char const* cmds[] = {"BARS", "SLOTS", "PUT", "TAKE", "OPEN", "EAT", "RESULT", "CLOSE"};
            body += cmds[rng() % 8];
            for (unsigned n = rng() % 12; n; --n)
                body += alphabet[rng() % (sizeof(alphabet) - 1)];
            if (auto msg = efenc::Decode(body))
            {
                auto bb = efenc::DecodeBars(*msg);
                auto ss = efenc::DecodeSlots(*msg);
                check(!bb || bb->bars.size() <= efenc::kMaxBars, "fuzz: bars bounded");
                check(!ss || ss->slots.size() <= efenc::kMaxSlots, "fuzz: slots bounded");
                (void)efenc::DecodePut(*msg);
                (void)efenc::DecodeTake(*msg);
                (void)w::DecodeOpen(*msg);
                (void)w::DecodeOpenRequest(*msg);
                (void)w::DecodeEat(*msg);
                (void)w::DecodeResult(*msg);
                (void)w::DecodeClose(*msg);
            }
        }
    }

    void TestBars()
    {
        Rule r;
        check(DrowseMax(r, 10, Rank::Normal) == 60000, "Drowse 20 + 4 a level: 60 at 10");
        check(DrowseMax(r, 60, Rank::Normal) == 260000, "260 at 60");
        check(DrowseMax(r, 60, Rank::Rare) == 1040000, "a rare x4");
        check(DrowseMax(r, 60, Rank::RareElite) == 1560000, "a rare elite x6");
        check(DrowseMax(r, 10, Rank::Elite) == 150000, "an elite x2.5");
        check(TrustMax(r, 10, Rank::Normal) == 120000 && TrustMax(r, 30, Rank::Normal) == 160000, "Trust 100 + 2 a level");
        check(TrustMax(r, 30, Rank::Elite) == 240000 && TrustMax(r, 30, Rank::Rare) == 320000 &&
                  TrustMax(r, 30, Rank::RareElite) == 400000,
              "an elite x1.5, a rare x2, a rare elite x2.5");
        check(RankOf(0) == Rank::Normal && RankOf(1) == Rank::Elite && RankOf(2) == Rank::RareElite && RankOf(4) == Rank::Rare &&
                  RankOf(3) == Rank::Elite,
              "creature ranks");
        check(TameSkillPoints(Rank::Normal) == 1 && TameSkillPoints(Rank::Elite) == 3 && TameSkillPoints(Rank::Rare) == 5 &&
                  TameSkillPoints(Rank::RareElite) == 5,
              "a tame's skill points");
        check(BandOfLevel(1) == 1 && BandOfLevel(11) == 1 && BandOfLevel(12) == 2 && BandOfLevel(31) == 3 && BandOfLevel(42) == 5 &&
                  BandOfLevel(60) == 5,
              "the bands");
    }

    void TestArrows()
    {
        Rule r;
        std::vector<Arrow> arrows = {{1, 20, 25, 1}, {2, 40, 40, 75}, {3, 65, 55, 150}, {4, 100, 60, 225}};
        auto carries = [](std::initializer_list<std::uint32_t> items) {
            return [items](std::uint32_t i) { return std::find(items.begin(), items.end(), i) != items.end(); };
        };
        check(PickArrow(arrows, 30, carries({1, 2, 3}))->item == 3, "the strongest carried the level allows");
        check(PickArrow(arrows, 58, carries({1, 2, 3, 4}))->item == 4, "deep for a level 58");
        check(PickArrow(arrows, 58, carries({1, 2}))->item == 2, "none allowed: the strongest carried");
        check(!PickArrow(arrows, 10, carries({})), "no arrows: none");
        check(ArrowDrowse(r, arrows[0], 10) == 20000, "a mild arrow on a level 10: 20");
        check(ArrowDrowse(r, arrows[0], 30) == 5000, "above its level: a quarter");
        // the doc's sums: a level-10 hound takes 3 mild arrows, a level-30 boar 4 strong
        State s = Begin(r, 1, 1, 10, Rank::Normal, 0);
        int n = 0;
        while (!AddDrowse(s, 100, ArrowDrowse(r, arrows[0], 10), 1000 * ++n)) { }
        check(n == 3, "three mild arrows put a level-10 beast under");
        s = Begin(r, 1, 5, 30, Rank::Normal, 0);
        n = 0;
        while (!AddDrowse(s, 100, ArrowDrowse(r, arrows[1], 30), 1000 * ++n)) { }
        check(n == 4, "four strong arrows put a level-30 beast under");
        Draught d{9, 30000, 40};
        check(DraughtMs(r, d, 40) == 30000 && DraughtMs(r, d, 41) == 7500, "a draught above its level: a quarter");
    }

    void TestDrowseAndClaim()
    {
        Rule r;
        State s = Begin(r, 1, 1, 30, Rank::Normal, 0);   // 140
        AddDrowse(s, 100, 40000, 1000);
        AddDrowse(s, 200, 40000, 2000);
        check(Claimant(s) == 100, "a tie goes to the first");
        AddDrowse(s, 200, 20000, 3000);
        check(Claimant(s) == 200, "the most Drowse holds the claim");
        check(s.drowse == 100000 && s.arrows == 3, "Drowse adds up");
        Decay(r, s, 8000, 1000, false);
        check(s.drowse == 100000, "no decay within 6 s of a hit");
        Decay(r, s, 9500, 1000, false);
        check(s.drowse == 100000 - 2800, "2 % of the max a second after");
        std::uint32_t const before = s.drowse;
        Decay(r, s, 3500, 500, true);
        check(s.drowse == before - 1400, "held by crowd control: decays at once");
        Decay(r, s, 100000, 100000, false);
        check(s.drowse == 0, "decays to 0, not past it");
        check(!AddDrowse(s, 100, 999999999, 200000) || s.drowse == s.drowseMax, "the bar caps at its max");
        StartOutCold(r, s, 200000);
        check(s.phase == Phase::OutCold && s.claimant == Claimant(s) && SleepLeft(s, 200000) == 60000, "Out Cold: 60 s");
        check(!AddDrowse(s, 300, 1000, 200001), "no Drowse while asleep");
        check(AddSleep(r, s, 60000, 230000) == 60000 && SleepLeft(s, 230000) == 90000, "a draught adds its time");
        check(AddSleep(r, s, 60000, 230000) == 30000 && SleepLeft(s, 230000) == 120000, "never past 120 s left");
        check(s.sleepLenMs == 120000, "the Sleep bar's max follows the longest sleep");
    }

    void TestFood()
    {
        Rule r;
        E::ItemGroups g;
        g.Add("meat", 1, 1);
        g.Add("meat", 2, 3);
        g.Add("small_meat", 1, 1);
        g.Add("fish", 3, 2);
        g.Add("greens", 4, 1);
        g.Add("cake", 5, 3);
        g.Add("draught", 6, 1);
        Tastes t;
        t.SetFamily(7, 0);          // a hawk-like family: small meat first, then fish, other meat plain, greens refused
        t.AddLike(7, "cake", 600);
        t.AddLike(7, "small_meat", 250);
        t.AddLike(7, "fish", 100);
        t.AddLike(7, "meat", 50);
        t.AddLike(7, "greens", 0);
        t.SetFamily(5, 50);         // a boar-like family: anything else plain
        t.AddLike(5, "greens", 250);
        check(t.Resolve(7, 1, g).permil == 250 && t.Resolve(7, 1, g).group == "small_meat", "the first like wins");
        check(t.Resolve(7, 2, g).permil == 50, "other meat: plain");
        check(t.Resolve(7, 4, g).permil == 0, "greens refused");
        check(t.Resolve(7, 99, g).permil == 0, "unnamed food: the family's other (0)");
        check(t.Resolve(5, 99, g).permil == 50 && t.Resolve(5, 99, g).band == 0, "a boar takes anything else");
        check(t.Resolve(1, 1, g).permil == 0, "an unknown family takes nothing");
        check(t.BestPermil(7) == 600, "the best like");
        // Trust by band: a level-5 beast (band 1) gets the full share; a level-25 (band 3) halves band-1 food twice
        check(FoodTrust(r, t.Resolve(7, 1, g), 5) == 25000, "a favourite: 25 of a normal bar");
        check(FoodTrust(r, t.Resolve(7, 1, g), 15) == 12500, "a band below: half");
        check(FoodTrust(r, t.Resolve(7, 1, g), 25) == 6250, "two bands below: a quarter");
        check(FoodTrust(r, t.Resolve(7, 5, g), 25) == 60000, "a cake of the beast's band: 60");
        check(FoodTrust(r, t.Resolve(7, 2, g), 15) == 5000, "food above the beast's band: full");
        check(FoodTrust(r, t.Resolve(5, 99, g), 50) == 5000, "other food has no band");
        check(FoodTrust(r, t.Resolve(7, 5, g), 31) == 60000 && TrustMax(r, 31, Rank::Normal) > TrustMax(r, 21, Rank::Normal),
              "a share is of trust_base at every level; the bar grows");
        check(TasteLetter(180) == 'C' && TasteLetter(140) == 'F' && TasteLetter(70) == 'L' && TasteLetter(35) == 'P' &&
                  TasteLetter(600) == 'C' && TasteLetter(0) == 'R',
              "the taste letters");

        // the eating order: the best Trust first, draughts and refused food left alone
        State s = Begin(r, 1, 7, 15, Rank::Normal, 0);
        StartOutCold(r, s, 0);
        auto isDraught = [](std::uint32_t item) { return item == 6; };
        check(CanPut(s, 0, 4, 2, 20) == PutError::None, "an empty slot takes anything");
        Put(s, 0, 4, 2);
        Put(s, 1, 2, 3);
        Put(s, 2, 6, 1);
        Put(s, 3, 1, 1);
        check(CanPut(s, 1, 3, 1, 20) == PutError::Occupied, "a slot holds one item");
        check(CanPut(s, 1, 2, 18, 20) == PutError::Bounds, "up to the item's stack");
        check(CanPut(s, 6, 2, 1, 20) == PutError::Bounds, "six slots");
        check(PickFood(r, s, t, g, isDraught) == 3, "the favourite first");
        Consume(s, 3);
        check(s.slots[3].item == 0, "the last one eaten empties the slot");
        check(PickFood(r, s, t, g, isDraught) == 1, "then the plain meat");
        check(FirstRefused(r, s, t, g, isDraught) == 0, "the greens are refused");
        s.sniffed[0] = true;
        check(FirstRefused(r, s, t, g, isDraught) == -1, "sniffed once");
        Take(s, 1);
        check(PickFood(r, s, t, g, isDraught) == -1, "nothing it eats: none");
        check(PickDraught(s, [](std::uint32_t item) { return item == 6 ? 15000u : 0u; }) == 2, "the draught's slot");
        AddTrust(s, 99000);
        AddTrust(s, 99000);
        check(TrustFull(s) && s.trust == s.trustMax, "Trust caps at full");
    }

    // The pace (feedback #415, hunter-taming.md §6.3): with the defaults, a level-20 beast fed three cakes (0.18) and
    // then favourites (0.14), one item every 12 s, is tamed at the tenth bite, two minutes after it dropped; the sleep
    // (60 s) needs two draughts (40 s each, drunk under 15 s left) to last.
    void TestPace()
    {
        Rule r;
        State s = Begin(r, 1, 5, 20, Rank::Normal, 0);
        StartOutCold(r, s, 0);
        std::uint64_t now = 0;
        std::uint32_t bites = 0, draughts = 0;
        while (!TrustFull(s) && now < 600000)
        {
            now += 500;
            if (SleepLeft(s, now) < r.draughtUnderMs && draughts < 5)
            {
                AddSleep(r, s, 40000, now);
                ++draughts;
            }
            if (SleepLeft(s, now) == 0)
                break;
            if (now >= s.nextEatMs)
            {
                s.nextEatMs = now + r.eatEveryMs;
                AddTrust(s, (bites < 3 ? 180 : 140) * r.trustBase);
                ++bites;
            }
        }
        check(TrustFull(s) && bites == 10 && now == 120000, "a level-20 beast: ten bites, tamed at 2:00");
        check(draughts == 2, "two draughts keep it under");
    }

    // KNOWN (hunter-taming.md §16): a family's fed items, split under the message's size, round trip; bad lists refuse.
    void TestKnown()
    {
        namespace W = eftame::wire;
        W::Known all{131, {}};
        for (std::uint32_t i = 0; i < 50; ++i)
            all.items.emplace_back(99400 + i, "CFLPR"[i % 5]);
        all.items.emplace_back(99999, W::kDrank);   // a draught is no taste: left out
        std::vector<W::Known> parts = W::SplitKnown(all);
        check(parts.size() == 3 && parts[0].items.size() == W::kKnownPerMessage && parts[2].items.size() == 2,
              "KNOWN splits 50 items into 24 + 24 + 2");
        std::size_t back = 0;
        for (W::Known const& k : parts)
        {
            std::string const body = W::EncodeKnown(9, k);
            check(!body.empty() && body.size() + 4 <= efenc::kMaxBytes, "KNOWN fits one message");
            auto m = efenc::Decode(body);
            auto d = m ? W::DecodeKnown(*m) : std::nullopt;
            check(d && d->family == 131 && d->items == k.items, "KNOWN round trip");
            back += d ? d->items.size() : 0;
        }
        check(back == 50, "every taste comes back");
        check(W::SplitKnown({131, {}}).empty(), "no items, no message");
        check(W::EncodeKnown(1, {0, {{1, 'F'}}}).empty(), "no family, no message");
        check(W::EncodeKnown(1, {131, {{1, 'D'}}}).empty(), "a draught is no known taste");
        efenc::Message bad = efenc::MakeMessage(1, efenc::kTaming, "KNOWN");
        for (char const* list : {"", "99405", "99405X", "F", "99405F,,99406L", "abcF"})
        {
            bad.fields = {"131", list};
            check(!W::DecodeKnown(bad), std::string("KNOWN refuses '") + list + "'");
        }
    }
}

// The sleeping beast's peace (feedback #436, #536-#538): any tie to a fight asks for peace, the AI's engagement alone too.
static void TestPeace()
{
    check(!NeedsPeace({}), "a beast tied to nothing is left alone");
    check(NeedsPeace({false, true, false, false, false}), "engaged alone (out of combat, no threat): peace, or it evades");
    check(NeedsPeace({true, false, false, false, false}), "in combat: peace");
    check(NeedsPeace({false, false, true, false, false}), "a victim: peace");
    check(NeedsPeace({false, false, false, true, false}), "attackers: peace");
    check(NeedsPeace({false, false, false, false, true}), "threat: peace");
}

int main()
{
    TestPeace();
    TestWire();
    TestBars();
    TestArrows();
    TestDrowseAndClaim();
    TestFood();
    TestPace();
    TestKnown();
    std::cout << "taming tests: " << g_checks - g_failed << "/" << g_checks << " passed\n";
    return g_failed ? 1 : 0;
}
