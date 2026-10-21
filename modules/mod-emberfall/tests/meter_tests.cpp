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

// Standalone tests of the damage meter (src/meter/EfMeterLogic.h; tests/run_tests.sh builds and runs them): the
// segments (start, grace, idle, history, overall), attribution (pets as sources, the row cap), the overheal and
// absorb maths, death recaps, the EFM bodies and what a client is pushed, and the bandwidth of a 5-man fight.

#include "EfMeterLogic.h"

#include <cstdio>
#include <iostream>
#include <string>

using namespace Emberfall::Meter;

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

    template <typename A, typename B>
    void eq(A const& a, B const& b, std::string const& what)
    {
        bool const ok = a == b;
        if (!ok)
            std::cerr << "  got [" << a << "] want [" << b << "]\n";
        check(ok, what);
    }

    Hit H(std::uint32_t spell, std::uint32_t amount, bool crit = false, std::uint32_t absorbed = 0, std::uint32_t overkill = 0)
    {
        Hit h;
        h.spell = spell;
        h.amount = amount;
        h.crit = crit;
        h.absorbed = absorbed;
        h.overkill = overkill;
        return h;
    }

    Hit Miss(std::uint32_t spell)
    {
        Hit h;
        h.spell = spell;
        h.avoided = true;
        return h;
    }

    Enemy const kWolf{ "Grey Wolf", false };
    Enemy const kBoss{ "the Post-Keeper", true };

    // The bytes of one SMSG_MESSAGECHAT addon whisper with this body on the wire: the server header (size, opcode: 4),
    // type 1, language 4, sender 8, flags 4, receiver 8, length 4, the text with "EFM\t" and its 0, the chat tag 1.
    std::size_t WireBytes(std::string const& body) { return 4 + 1 + 4 + 8 + 4 + 8 + 4 + (kPrefix.size() + body.size() + 1) + 1; }

    void TestWire()
    {
        eq(B36(0), std::string("0"), "b36 0");
        eq(B36(35), std::string("z"), "b36 35");
        eq(B36(36), std::string("10"), "b36 36");
        eq(*ParseB36(B36(123456789012ull)), 123456789012ull, "b36 round trip");
        check(!ParseB36("A1"), "b36 lower case only");
        check(!ParseB36(""), "b36 empty");
        eq(SignedB36(-71), std::string("-1z"), "signed b36");
        eq(Clean("a;b|c,d\te"), std::string("a b c d e"), "separators cleaned");
        eq(Clean(std::string(60, 'x')).size(), kNameMax, "names cut");
        // a cut never splits a UTF-8 character: 39 x plus a two-byte character at 39..40 -> 39 bytes
        eq(Clean(std::string(39, 'x') + "\xc3\xa9" + "yy").size(), std::size_t(39), "utf-8 cut");
        eq(Overkill(500, 120), 380u, "overkill");
        eq(Overkill(100, 120), 0u, "no overkill");
        eq(int(Pct(1, 3)), 33, "pct");
        eq(int(Pct(5, 0)), 0, "pct of nothing");

        auto r = ParseRequest("1;1004;SPELLS;3;2;d");
        check(r && r->seq == 1004 && r->cmd == "SPELLS" && r->fields.size() == 3 && r->fields[2] == "d", "request parsed");
        check(!ParseRequest("2;1;HELLO"), "other version refused");
        check(!ParseRequest("1;x;HELLO"), "bad seq refused");
        check(!ParseRequest("1;1"), "no command refused");
        check(!ParseRequest(std::string("1;1;HELLO\x01")), "control characters refused");
        check(ParseMetric("p") == kDispels && !ParseMetric("x") && !ParseMetric("dd"), "metric letters");
    }

    void TestSegments()
    {
        Meter m;
        m.epoch = 7;
        std::uint8_t const me = m.PidFor(100, Kind::Player, 8, 0, "Aelin");
        std::uint8_t const healer = m.PidFor(200, Kind::Companion, 2, 0, "Priest");
        eq(int(me), 1, "first pid");
        eq(int(healer), 2, "second pid");
        eq(int(m.PidFor(100, Kind::Player, 8, 0, "Aelin")), 1, "same key, same pid");

        // Nothing before the pull: a heal, an absorb, falling damage.
        m.HealDone(1000, healer, 0, SplitHeal(2061, 500, 100));
        m.DamageTaken(1000, me, H(kEnvSpellBase + 2, 300), "", nullptr, 80, true);
        check(m.fights.empty() && m.overall.rows.empty(), "out of combat: nothing counted");

        // The pull: a hit on a wolf starts the fight, named after it.
        m.SetCombat(me, true, 2000);
        m.DamageDone(2000, me, 0, H(133, 400, true), kWolf);
        check(m.Live() && m.Live()->name == "Grey Wolf" && !m.Live()->boss && m.Live()->id == 1, "the fight starts, named after the enemy");
        m.DamageDone(3500, me, 0, Miss(133), kWolf);
        m.DamageDone(4000, me, 0, H(0, 100), kWolf);
        // The healer: 600 heal on 250 missing -> 250 effective, 350 overheal.
        m.HealDone(4200, healer, 0, SplitHeal(2061, 600, 250));
        Row const& h = m.Live()->rows.at(healer);
        eq(h.t.healing, 250ull, "effective healing");
        eq(h.t.overheal, 350ull, "overheal");
        eq(h.spells[kHealing].at(Key(0, 2061)).over, 350ull, "overheal per spell");
        // A shield absorbed 120: absorbs and healing's spell list; the target received it.
        m.AbsorbDone(4300, healer, 0, 17, 120);
        m.AbsorbReceived(4300, me, 17, 120);
        eq(m.Live()->rows.at(healer).t.absorbs, 120ull, "absorbs provided");
        eq(m.Live()->rows.at(healer).spells[kHealing].at(Key(0, 17)).total, 120ull, "the shield in the healing breakdown");
        eq(m.Live()->rows.at(me).t.received, 120ull, "absorb received");

        Row const& r = m.Live()->rows.at(me);
        SpellStats const& fireball = r.spells[kDamage].at(Key(0, 133));
        check(fireball.hits == 1 && fireball.crits == 1 && fireball.misses == 1 && fireball.total == 400 && fireball.max == 400 && fireball.min == 400,
              "per spell: hits, crits, misses, max, min");
        eq(r.t.damage, 500ull, "damage done");
        eq(r.t.activeMs, 2000ull, "active time: the gaps up to 5 s");

        // A boss joins: the fight takes its name.
        m.DamageTaken(5000, me, H(0, 50), "the Post-Keeper", &kBoss, 90);
        check(m.Live()->boss && m.Live()->name == "the Post-Keeper", "a boss names the fight");
        eq(m.Live()->rows.at(me).t.taken, 50ull, "damage taken");

        // Still in combat: the fight goes on past the grace.
        m.SetCombat(me, true, 9000);
        check(m.Live() != nullptr, "in combat: still live");
        // Everyone out of combat: the fight ends kGraceMs later; its duration is first to last event.
        m.SetCombat(me, false, 9500);
        m.SetCombat(healer, false, 9500);
        check(m.Live() != nullptr, "within the grace: still live");
        m.SetCombat(me, false, 12600);
        check(!m.Live() && m.fights.size() == 1, "after the grace: over");
        eq(m.fights.front().DurationMs(99999), 3000ull, "duration: first to last event");
        eq(m.overall.closedMs, 3000ull, "overall: the fight's duration");
        eq(m.overall.rows.at(me).t.damage, 500ull, "overall has the fight");

        // A stale combat flag (a player who left the map) does not keep a fight open; idle ends it anyway.
        m.DamageDone(20000, me, 0, H(133, 10), kWolf);
        m.SetCombat(healer, true, 20000);
        m.SetCombat(me, false, 26000);
        check(!m.Live(), "a stale flag does not hold the fight");
        m.DamageDone(30000, me, 0, H(133, 10), kWolf);
        m.SetCombat(me, true, 30000);
        m.SetCombat(me, true, 49000);
        check(m.Live() != nullptr, "in combat, quiet for 19 s: live");
        m.SetCombat(me, true, 50001);
        check(!m.Live(), "quiet for 20 s: over even in combat");
        eq(m.overall.rows.at(me).t.damage, 520ull, "overall: every fight");

        // History: the last ten fights.
        for (std::uint64_t i = 0; i < 12; ++i)
        {
            std::uint64_t const t = 100000 + i * 10000;
            m.DamageDone(t, me, 0, H(133, 1), kWolf);
            m.SetCombat(me, false, t + 4000);
        }
        eq(m.fights.size(), kHistory, "ten fights kept");
        eq(m.fights.front().id, 15u, "newest first");
        check(m.Find(5) == nullptr && m.Find(6) != nullptr && m.Find(0) == &m.overall, "the oldest dropped");
        eq(m.overall.rows.at(me).t.damage, 532ull, "overall keeps the dropped fights");

        m.Reset(9);
        check(m.fights.empty() && m.overall.rows.empty() && m.epoch == 9 && m.roster.size() == 2, "reset: fights gone, names kept");
    }

    void TestAttribution()
    {
        Meter m;
        std::uint8_t const hunter = m.PidFor(1, Kind::Player, 3, 0, "Bran");
        std::uint8_t const wolf = m.PidFor((1ull << 32) | 900, Kind::Pet, 0, hunter, "Wolf");
        std::uint8_t const totem = m.PidFor((1ull << 32) | 901, Kind::Pet, 0, hunter, "Totem");
        m.DamageDone(1000, hunter, 0, H(75, 300), kWolf);
        m.DamageDone(1100, hunter, wolf, H(0, 120), kWolf);
        m.DamageDone(1200, hunter, wolf, H(0, 80, false, 20), kWolf); // 20 absorbed by the target's shield
        m.HealDone(1300, hunter, totem, SplitHeal(5394, 50, 50));
        Row const& r = m.Live()->rows.at(hunter);
        eq(r.t.damage, 520ull, "pets add to their owner");
        eq(r.t.absorbedByTarget, 20ull, "absorbed by the target");
        check(r.spells[kDamage].count(Key(wolf, 0)) && r.spells[kDamage].at(Key(wolf, 0)).total == 220, "the pet's own rows");
        check(r.spells[kHealing].count(Key(totem, 5394)), "a totem's heal under its pid");
        check(!m.Live()->rows.count(wolf), "no row of its own for a pet");
        eq(m.Rows(), std::size_t(1), "pets are not rows");

        // A raid's worth: 40 rows, then no more (pets still fit).
        for (std::uint64_t i = 0; i < 39; ++i)
            check(m.PidFor(1000 + i, Kind::Player, 1, 0, "P") != 0, "rows up to 40");
        eq(int(m.PidFor(5000, Kind::Player, 1, 0, "Late")), 0, "the 41st row is not counted");
        check(m.PidFor(6000, Kind::Pet, 0, hunter, "Imp") != 0, "pets beyond 40 rows");
        m.DamageDone(1400, 0, 0, H(1, 1), kWolf);
        check(!m.Live()->rows.count(0), "pid 0 counts nothing");
    }

    void TestDeaths()
    {
        Meter m;
        std::uint8_t const tank = m.PidFor(1, Kind::Player, 1, 0, "Tank");
        m.DamageTaken(1000, tank, H(0, 100), "Ogre", &kWolf, 90);
        for (std::uint64_t t = 2000; t <= 7000; t += 1000)
            m.DamageTaken(t, tank, H(0, 100, t == 7000), "Ogre", &kWolf, std::uint8_t(80 - t / 1000 * 10));
        m.HealReceived(7500, tank, SplitHeal(2061, 300, 1000), "Priest", 30);
        m.DamageTaken(8000, tank, H(9001, 900, false, 0, 600), "Ogre Mage", &kWolf, 0);
        m.Died(8000, tank, "");
        Row const& r = m.Live()->rows.at(tank);
        eq(r.t.deaths, 1u, "a death");
        Death const& d = r.deaths.at(0);
        eq(d.events.size(), kRecapEvents, "the last five events");
        eq(d.events.back().spell, 9001u, "the killing blow last");
        eq(d.events.back().atMs, std::int64_t(0), "at the death");
        eq(d.events.front().atMs, std::int64_t(-3000), "three seconds before");
        eq(d.events[3].amount, std::int64_t(300), "the heal");
        eq(d.events.back().amount, std::int64_t(-900), "damage is negative");
        eq(d.spell, 9001u, "the killing spell");
        eq(d.killer, std::string("Ogre Mage"), "the killer from the last hit");
        eq(d.timeMs, 7000ull, "time since the pull");
        eq(m.overall.rows.at(tank).deaths.size(), std::size_t(1), "overall has it too");

        auto bodies = DeathBodies(12, *m.Live(), tank);
        eq(bodies.size(), std::size_t(3), "DTH, DEV, END");
        eq(bodies[0], std::string("1;0;DTH;12;1;1;1;5eg;9001;Ogre Mage"), "DTH body");
        check(bodies[1].rfind("1;0;DEV;12;1;2bc,0,-2s,30,0,Ogre|", 0) == 0, "DEV rows: 3000 ms before, melee, -100, 30 %");
        eq(bodies[2], std::string("1;0;END;12"), "END");
    }

    void TestPush()
    {
        Meter m;
        m.epoch = 3;
        std::uint8_t const a = m.PidFor(1, Kind::Player, 8, 0, "Aelin");
        Client c;
        check(Push(m, c, 0).empty(), "nothing before HELLO");
        c.hello = true;
        auto first = Push(m, c, 0);
        eq(first.size(), std::size_t(3), "ON, WHO, LIST");
        eq(first[0], std::string("1;0;ON;3"), "ON");
        eq(first[1], std::string("1;0;WHO;1;p;8;0;Aelin"), "WHO");
        eq(first[2], std::string("1;0;LIST;"), "LIST, empty");
        check(Push(m, c, 500).empty(), "no change, nothing");

        m.DamageDone(1000, a, 0, H(133, 1295), kWolf);
        auto second = Push(m, c, 1000);
        eq(second.size(), std::size_t(3), "LIST, SEG, SUM");
        eq(second[0], std::string("1;0;LIST;1"), "the new fight listed");
        eq(second[1], std::string("1;0;SEG;1;1;rs;Grey Wolf"), "SEG: live, 1 s");
        eq(second[2], std::string("1;0;SUM;1;1,zz,0,0,0,0,0,0,0,0,0"), "SUM row");

        std::uint8_t const b = m.PidFor(2, Kind::Player, 1, 0, "Bran");
        m.DamageDone(1500, b, 0, H(78, 36), kWolf);
        auto third = Push(m, c, 2000);
        eq(third.size(), std::size_t(3), "WHO, SEG, SUM");
        eq(third[2], std::string("1;0;SUM;1;2,10,0,0,0,0,0,0,0,0,0"), "only the changed row");

        // Viewing overall: it comes too, all its rows the first time.
        c.view = 0;
        auto fourth = Push(m, c, 2100);
        eq(fourth.size(), std::size_t(2), "overall SEG and SUM");
        eq(fourth[0], std::string("1;0;SEG;0;1;uk;"), "overall SEG (live while a fight is: 1.1 s)");
        eq(fourth[1], std::string("1;0;SUM;0;1,zz,0,0,0,0,0,0,0,0,0|2,10,0,0,0,0,0,0,0,0,0"), "overall rows");

        // A reset is a new meter: ON again, then the names.
        m.Reset(4);
        auto fifth = Push(m, c, 3000);
        check(fifth.size() == 5 && fifth[0] == "1;0;ON;4" && fifth[1].rfind("1;0;WHO;1;", 0) == 0 && fifth[3] == "1;0;LIST;"
                  && fifth[4] == "1;0;SEG;0;0;0;",
              "after a reset: ON, WHO again, LIST, the empty overall it views");

        // 40 rows chunk into bodies of at most 251 bytes.
        Meter big;
        big.epoch = 1;
        for (std::uint64_t i = 0; i < 40; ++i)
        {
            std::uint8_t const p = big.PidFor(10 + i, Kind::Player, 1, 0, "Somebodywithalongname");
            big.DamageDone(1000 + i, p, 0, H(1, 2000000000), kWolf);
            big.HealDone(1000 + i, p, 0, SplitHeal(2, 2000000000, 1000000000));
        }
        Client bc;
        bc.hello = true;
        std::size_t sums = 0, longest = 0;
        for (std::string const& body : Push(big, bc, 2000))
        {
            longest = std::max(longest, body.size());
            sums += body.rfind("1;0;SUM;", 0) == 0;
        }
        check(longest <= kMaxBody, "no body over 251 bytes");
        check(sums >= 6, "40 big rows: several SUM bodies");

        auto spells = SpellBodies(5, big.fights.front(), 1, kDamage);
        eq(spells.size(), std::size_t(2), "SPL and END");
        eq(spells[0], std::string("1;0;SPL;5;1;1;d;0,1,1,0,0,x2qxvk,0,0,x2qxvk,x2qxvk"), "SPL row");
        eq(SpellBodies(6, big.fights.front(), 99, kDamage).size(), std::size_t(1), "nobody: just END");

        // The request limit.
        Client lim;
        int allowed = 0;
        for (int i = 0; i < 40; ++i)
            allowed += lim.Allow(1000) ? 1 : 0;
        eq(allowed, int(kRequestsPer10s), "30 requests per 10 s");
        check(lim.Allow(11000), "a new window");
    }

    // A 5-man fight of 60 s: five players hit every 1.5 s (a pet too), the healer heals every 2 s, the tank takes a hit
    // every 2 s; every client is pushed once a second (the current fight; one of them also views overall).
    void TestBandwidth()
    {
        Meter m;
        m.epoch = 1;
        std::uint8_t pids[5];
        char const* names[5] = { "Tankwell", "Healsalot", "Fireborn", "Shadowstep", "Arrowind" };
        for (int i = 0; i < 5; ++i)
            pids[i] = m.PidFor(std::uint64_t(i + 1), Kind::Player, std::uint8_t(i + 1), 0, names[i]);
        std::uint8_t const pet = m.PidFor((5ull << 32) | 1, Kind::Pet, 0, pids[4], "Wolf");
        Client clients[5];
        for (Client& c : clients)
            c.hello = true;
        clients[1].view = 0;
        std::size_t bytes[5] = {}, messages = 0;
        std::uint64_t const start = 10000, end = start + 60000;
        for (std::uint64_t t = start; t <= end + 5000; t += 100)
        {
            if (t <= end)
            {
                if ((t - start) % 1500 == 0)
                    for (int i = 0; i < 5; ++i)
                        m.DamageDone(t, pids[i], 0, H(100 + std::uint32_t(i), 1200 + std::uint32_t(i) * 37, (t / 1500) % 4 == 0), kBoss);
                if ((t - start) % 1500 == 700)
                    m.DamageDone(t, pids[4], pet, H(0, 300), kBoss);
                if ((t - start) % 2000 == 0)
                {
                    m.DamageTaken(t, pids[0], H(0, 1800), "the Post-Keeper", &kBoss, 70);
                    m.HealDone(t, pids[1], 0, SplitHeal(2061, 2400, 1800));
                    m.HealReceived(t, pids[0], SplitHeal(2061, 2400, 1800), "Healsalot", 100);
                }
                for (int i = 0; i < 5; ++i)
                    m.SetCombat(pids[i], true, t);
            }
            else
                for (int i = 0; i < 5; ++i)
                    m.SetCombat(pids[i], false, t);
            if ((t - start) % kPushMs == 0)
                for (int i = 0; i < 5; ++i)
                    for (std::string const& body : Push(m, clients[i], t))
                    {
                        check(body.size() <= kMaxBody, "pushed body fits");
                        bytes[i] += WireBytes(body);
                        ++messages;
                    }
        }
        check(!m.Live(), "the fight ended");
        double const perClient = double(bytes[0]) / 65.0;
        double const viewer = double(bytes[1]) / 65.0;
        std::size_t total = 0;
        for (std::size_t b : bytes)
            total += b;
        std::printf("meter bandwidth, 5-man fight of 60 s (65 s with the end): %.0f B/s per client (%.0f B/s viewing overall too), "
                    "%.0f B/s for the group, %zu messages\n", perClient, viewer, double(total) / 65.0, messages);
        check(perClient < 400.0, "under 400 B/s per client");
        check(viewer < 700.0, "under 700 B/s viewing overall");
    }
}

int main()
{
    TestWire();
    TestSegments();
    TestAttribution();
    TestDeaths();
    TestPush();
    TestBandwidth();
    std::cout << "meter tests: " << g_checks << " checks, " << g_failed << " failed\n";
    return g_failed ? 1 : 0;
}
