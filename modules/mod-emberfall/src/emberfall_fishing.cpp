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

// Emberfall fishing: the living waters (Emberfall docs/design/fishing.md, §11 the build contract), the first kind ('F')
// of the mini-encounter framework (emberfall_encounters.cpp).
//
// The switch: a bobber (game object 90190, ScriptName go_ef_fishing_bobber) whose position lies in no habitat cell
// (ef_fish_cell: every zone but those the content build gave habitats), or Emberfall.Fishing.LivingWaters 0, runs the
// core's fishing unchanged (OnGossipHello returns false). In a cell, the hook click (GameObject::Use calls the script
// first) is this module's:
//   - the roll (src/fishing/EfFishLogic.h over the shared EfFishRules.h): junk as the core rolls it (a shoal skips it),
//     else a species of the water by habitat, time of day (the part of the day by the sun: src/sun/EfSun.h), the
//     zone's weather (and "after rain"), the bait aura on the angler, the skill gates; the legendary through his quest
//     chain; the size. The skill-up exactly as the core (OnPlayerUpdateFishingSkill, UpdateFishingSkill);
//   - junk: the core's junk loot (SendLoot LOOT_FISHING_JUNK); a common: the fish in the bobber's loot, the loot window;
//   - uncommon and better: the fight. The bobber's spell id is cleared and its loot state set to activated (the core's
//     own trick for an open loot window: the channel's end does not take it, its escape timer stops), the fishing
//     channel finishes, Reeling (99264) is cast and the bobber made its channel object (the client draws the line),
//     and OPEN goes to the client (EfFishWire.h). The client runs the fight (EfFishFight.h) and sends END; the server
//     checks it (CheckEnd against its own clock; the angler alive, unmounted, channelling Reeling, within 30 yd of
//     the bobber): RESULT;OK puts the fish in the bobber's loot and opens the loot window, anything else
//     RESULT;REJ;<code>. A broken channel, death, a far bobber or the timeout end it: CLOSE;<reason>.
// Every bite writes ef_character_fish (caught, landed, lost, best size, first caught). Area trigger script
// at_ef_fish_watch and ef_fish_event: a quest done by watching the water under a species' conditions (kind 1) or by
// landing a species (kind 2). Spell script spell_ef_fish_groundbait (99265): where the ground-bait went in (5 min).
// GM: `.effish cell`, `.effish force <species>`, `.effish stats [player]`, `.effish reload`.
#include "ALEScript.h"
#include "AreaTriggerScript.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "CommandScript.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "GameObject.h"
#include "GameObjectScript.h"
#include "GameTime.h"
#include "Log.h"
#include "LootMgr.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerScript.h"
#include "QuestDef.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"
#include "Timer.h"
#include "Weather.h"
#include "WorldScript.h"
#include "WorldSession.h"
#include "encounters/EfEncounterMgr.h"
#include "fishing/EfFishLogic.h"
#include "fishing/EfFishWire.h"
#include "sun/EfSun.h"
#include <algorithm>
#include <cmath>
#include <list>
#include <mutex>
#include <unordered_map>

using namespace Acore::ChatCommands;

namespace
{
    namespace E = Emberfall::Encounters;
    namespace F = Emberfall::Fishing;

    constexpr uint32 kSpellReeling = 99264;
    constexpr float kMaxRange = 30.0f;          // the angler within this of the bobber; the ground-bait's reach
    constexpr uint32 kGroundBaitSeconds = 300;

    bool s_enable = true;
    std::mutex s_lock;   // everything below
    F::Data s_data;
    std::unordered_map<uint32, uint32> s_shoalItem;   // fishing pool template -> its own fish

    struct Event
    {
        uint32 quest = 0;
        uint8 kind = 0;      // 1 watch, 2 land
        uint32 species = 0;
        uint32 trigger = 0;
    };
    std::vector<Event> s_events;

    struct FightState
    {
        uint32 id = 0;           // the encounter's id (EFN's fight id)
        ObjectGuid bobber;
        uint32 species = 0;
        uint32 item = 0;
        uint32 sizeMm = 0;
        uint32 grams = 0;
        F::Rarity rarity = F::Rarity::Uncommon;
        effish::FightParams params;
        uint64 startMs = 0;
        uint32 map = 0;
        float x = 0, y = 0;      // where the angler stood
    };
    std::unordered_map<uint64, FightState> s_fights;   // by the angler's low GUID

    struct Weatherish
    {
        uint32 state = 0;          // WeatherState
        uint64 rainEndedSec = 0;   // 0: no rain seen since the server started
        bool raining = false;
    };
    std::unordered_map<uint32, Weatherish> s_weather;   // by zone

    struct GroundBait
    {
        uint32 map = 0;
        float x = 0, y = 0;
        uint64 untilSec = 0;
    };
    std::unordered_map<uint64, GroundBait> s_groundBait;
    // angler -> species -> the record (ef_character_fish, loaded at login): the best flag and the journal's BOOK
    std::unordered_map<uint64, std::unordered_map<uint32, effish::wire::BookEntry>> s_book;
    std::unordered_map<uint64, std::unordered_map<uint32, uint64>> s_legendDay; // angler -> species -> day landed
    std::unordered_map<uint64, uint32> s_forced;
    std::unordered_map<uint64, F::Stats> s_stats;

    uint64 NowMs() { return uint64(GameTime::GetGameTimeMS().count()); }
    uint64 NowSec() { return uint64(GameTime::GetGameTime().count()); }
    uint64 Key(Player* p) { return p->GetGUID().GetCounter(); }

    // The time of day by the sun (src/sun/EfSun.h): the part of the day on the sun clock, which the client reads from the
    // same sunrise and sunset (its water clues); fishing.md §4.2's windows.
    uint8 TimeBitNow()
    {
        return F::TimeBitOf(Emberfall::Sun::PartNow());
    }

    // under s_lock
    uint8 WeatherBitsOf(uint32 zone)
    {
        auto it = s_weather.find(zone);
        if (it == s_weather.end())
            return effish::kClear;
        Weatherish const& w = it->second;
        uint64 const since = w.rainEndedSec == 0 ? ~uint64(0) : NowSec() - w.rainEndedSec;
        return effish::WeatherBits(effish::WeatherClassOf(w.state), since > 0xFFFFFFFFull ? 0xFFFFFFFFu : uint32(since));
    }

    // ---- the data -----------------------------------------------------------------------------------------------------
    void Load()
    {
        F::Data d;
        if (QueryResult r = WorldDatabase.Query("SELECT `data_hash` FROM `ef_fish_meta` WHERE `id` = 1"))
            d.hash = r->Fetch()[0].Get<uint64>();
        if (QueryResult r = WorldDatabase.Query("SELECT `id`, `map`, `zone`, `kind`, `name` FROM `ef_fish_water`"))
            do
            {
                Field* f = r->Fetch();
                F::Water w;
                w.id = f[0].Get<uint32>();
                w.map = f[1].Get<uint32>();
                w.zone = f[2].Get<uint32>();
                w.kind = f[3].Get<uint8>();
                w.name = f[4].Get<std::string>();
                d.waters[w.id] = w;
            } while (r->NextRow());
        if (QueryResult r = WorldDatabase.Query("SELECT `map`, `cx`, `cy`, `water`, `mask`, `depth_dm` FROM `ef_fish_cell`"))
            do
            {
                Field* f = r->Fetch();
                F::Cell c;
                c.water = f[3].Get<uint32>();
                c.mask = f[4].Get<uint8>();
                c.depthDm = f[5].Get<uint16>();
                d.cells[effish::CellKey(f[0].Get<uint32>(), f[1].Get<int32>(), f[2].Get<int32>())] = c;
            } while (r->NextRow());
        if (QueryResult r = WorldDatabase.Query("SELECT `id`, `kind`, `pull`, `drain`, `every_min_ms`, `every_max_ms`, "
                                                "`magnitude`, `hold_ms`, `over900_ms` FROM `ef_fish_style`"))
            do
            {
                Field* f = r->Fetch();
                effish::Style s;
                s.id = f[0].Get<uint32>();
                s.kind = effish::StyleKind(f[1].Get<uint8>());
                s.pullPermil = f[2].Get<uint32>();
                s.drainPermil = f[3].Get<uint32>();
                s.everyMinMs = f[4].Get<uint32>();
                s.everyMaxMs = f[5].Get<uint32>();
                s.magnitudePermil = f[6].Get<uint32>();
                s.holdMs = f[7].Get<uint32>();
                s.over900Ms = f[8].Get<uint32>();
                d.styles[s.id] = s;
            } while (r->NextRow());
        if (QueryResult r = WorldDatabase.Query(
                "SELECT `id`, `name`, `item`, `rarity`, `style`, `speed`, `size_typ_mm`, `size_max_mm`, `weight_k`, `clue`, "
                "`time_mask`, `weather_mask`, `h_shallows`, `h_deep`, `h_inlet`, `h_reeds`, `h_rocks`, `h_ruins`, "
                "`boost1_time`, `boost1_weather`, `boost1_permil`, `boost2_time`, `boost2_weather`, `boost2_permil`, "
                "`boost3_time`, `boost3_weather`, `boost3_permil`, "
                "`phase1_style`, `phase1_until`, `phase2_style`, `phase2_until`, `phase3_style`, `phase3_until`, "
                "`gate_quest`, `needs_groundbait` FROM `ef_fish_species` ORDER BY `id`"))
            do
            {
                Field* f = r->Fetch();
                effish::Species s;
                s.id = f[0].Get<uint32>();
                d.names[s.id] = f[1].Get<std::string>();
                s.item = f[2].Get<uint32>();
                s.rarity = effish::Rarity(f[3].Get<uint8>());
                s.style = f[4].Get<uint32>();
                s.speedPermil = f[5].Get<uint32>();
                s.sizeTypMm = f[6].Get<uint32>();
                s.sizeMaxMm = f[7].Get<uint32>();
                s.weightK = f[8].Get<uint32>();
                s.clue = effish::Clue(f[9].Get<uint8>());
                s.timeMask = f[10].Get<uint8>();
                s.weatherMask = f[11].Get<uint8>();
                for (std::size_t i = 0; i < 6; ++i)
                    s.habitatPermil[i] = f[12 + i].Get<uint16>();
                for (std::size_t i = 0; i < 3; ++i)
                    s.boosts[i] = effish::Boost{f[18 + 3 * i].Get<uint8>(), f[19 + 3 * i].Get<uint8>(), f[20 + 3 * i].Get<uint16>()};
                for (std::size_t i = 0; i < 3; ++i)
                    s.phases[i] = effish::Phase{f[27 + 2 * i].Get<uint32>(), f[28 + 2 * i].Get<uint32>()};
                s.gateQuest = f[33].Get<uint32>();
                s.needsGroundBait = f[34].Get<uint8>() != 0;
                d.species.push_back(s);
            } while (r->NextRow());
        if (QueryResult r = WorldDatabase.Query("SELECT `species`, `water` FROM `ef_fish_species_water` ORDER BY `species`, `water`"))
            do
            {
                Field* f = r->Fetch();
                uint32 const id = f[0].Get<uint32>();
                for (effish::Species& s : d.species)
                    if (s.id == id)
                        s.waters.push_back(f[1].Get<uint16>());
            } while (r->NextRow());
        if (QueryResult r = WorldDatabase.Query("SELECT `species`, `item`, `permil` FROM `ef_fish_species_bait`"))
            do
            {
                Field* f = r->Fetch();
                d.likes[{f[0].Get<uint32>(), f[1].Get<uint32>()}] = f[2].Get<uint32>();
            } while (r->NextRow());
        if (QueryResult r = WorldDatabase.Query("SELECT `item`, `aura` FROM `ef_fish_bait`"))
            do
            {
                Field* f = r->Fetch();
                d.baitByAura[f[1].Get<uint32>()] = f[0].Get<uint32>();
            } while (r->NextRow());

        std::vector<Event> events;
        if (QueryResult r = WorldDatabase.Query("SELECT `quest`, `kind`, `species`, `trigger_id` FROM `ef_fish_event`"))
            do
            {
                Field* f = r->Fetch();
                events.push_back(Event{f[0].Get<uint32>(), f[1].Get<uint8>(), f[2].Get<uint32>(), f[3].Get<uint32>()});
            } while (r->NextRow());

        // the fishing pools' own fish: each pool template's (type 25) best loot row
        std::unordered_map<uint32, std::pair<uint32, float>> shoal;
        if (QueryResult r = WorldDatabase.Query(
                "SELECT gt.`entry`, glt.`Item`, glt.`Chance` FROM `gameobject_template` gt "
                "JOIN `gameobject_loot_template` glt ON glt.`Entry` = gt.`Data1` WHERE gt.`type` = 25 AND glt.`Reference` = 0"))
            do
            {
                Field* f = r->Fetch();
                uint32 const entry = f[0].Get<uint32>();
                float chance = f[2].Get<float>();
                if (chance == 0.0f)
                    chance = 100.0f;
                auto it = shoal.find(entry);
                if (it == shoal.end() || chance > it->second.second)
                    shoal[entry] = {f[1].Get<uint32>(), chance};
            } while (r->NextRow());

        std::lock_guard<std::mutex> guard(s_lock);
        s_data = std::move(d);
        s_events = std::move(events);
        s_shoalItem.clear();
        for (auto const& [entry, v] : shoal)
            s_shoalItem[entry] = v.first;
        LOG_INFO("server.loading", ">> Emberfall fishing: {} waters, {} cells, {} species ({}; {} styles, {} baits, {} quest events)",
                 s_data.waters.size(), s_data.cells.size(), s_data.species.size(), s_enable ? "on" : "off (Emberfall.Fishing.LivingWaters 0)",
                 s_data.styles.size(), s_data.baitByAura.size(), s_events.size());
    }

    // ---- the angler's record ----------------------------------------------------------------------------------------
    void Record(Player* player, uint32 species, bool landed, bool lost, uint32 bestMm)
    {
        {
            std::lock_guard<std::mutex> guard(s_lock);
            effish::wire::BookEntry& e = s_book[Key(player)][species];
            e.species = species;
            ++e.caught;
            e.landed += landed ? 1 : 0;
            e.lost += lost ? 1 : 0;
            e.bestMm = std::max(e.bestMm, bestMm);
            if (!e.first)
                e.first = uint32(NowSec());
        }
        CharacterDatabase.Execute(
            "INSERT INTO `ef_character_fish` (`guid`, `species`, `caught`, `landed`, `lost`, `best_mm`, `first_caught`) "
            "VALUES ({}, {}, 1, {}, {}, {}, {}) ON DUPLICATE KEY UPDATE `caught` = `caught` + VALUES(`caught`), "
            "`landed` = `landed` + VALUES(`landed`), `lost` = `lost` + VALUES(`lost`), "
            "`best_mm` = GREATEST(`best_mm`, VALUES(`best_mm`))",
            player->GetGUID().GetCounter(), species, landed ? 1 : 0, lost ? 1 : 0, bestMm, NowSec());
    }

    // A landed fish's best flag (under s_lock): true when it beats the angler's best of the species.
    bool UpdateBest(Player* player, uint32 species, uint32 sizeMm)
    {
        effish::wire::BookEntry& e = s_book[Key(player)][species];
        e.species = species;
        bool const isBest = sizeMm > e.bestMm;
        if (isBest)
            e.bestMm = sizeMm;
        return isBest;
    }

    // Kind 2 events: landing the species completes its quest.
    void LandEvents(Player* player, uint32 species)
    {
        std::vector<uint32> quests;
        {
            std::lock_guard<std::mutex> guard(s_lock);
            for (Event const& e : s_events)
                if (e.kind == 2 && e.species == species)
                    quests.push_back(e.quest);
        }
        for (uint32 q : quests)
            if (player->GetQuestStatus(q) == QUEST_STATUS_INCOMPLETE)
                player->AreaExploredOrEventHappens(q);
    }

    // ---- the fight's end ----------------------------------------------------------------------------------------------
    void DespawnBobber(Player* player, ObjectGuid bobber)
    {
        if (GameObject* go = ObjectAccessor::GetGameObject(*player, bobber))
            go->SetLootState(GO_JUST_DEACTIVATED);
    }

    void StopReeling(Player* player)
    {
        if (Spell* sp = player->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
            if (sp->GetSpellInfo() && sp->GetSpellInfo()->Id == kSpellReeling)
                player->InterruptSpell(CURRENT_CHANNELED_SPELL, true, true, true);
    }

    // Ends the fight with CLOSE (lost): `send` false at logout. Takes s_lock itself.
    void CloseFight(Player* player, char const* reason, bool send = true)
    {
        FightState st;
        {
            std::lock_guard<std::mutex> guard(s_lock);
            auto it = s_fights.find(Key(player));
            if (it == s_fights.end())
                return;
            st = it->second;
            s_fights.erase(it);
        }
        E::Finish(player, st.id);
        if (send)
            E::Send(player, effish::wire::EncodeClose(E::NextSeq(player), {st.id, reason}));
        StopReeling(player);
        DespawnBobber(player, st.bobber);
        Record(player, st.species, false, true, 0);
        LOG_DEBUG("module", "Emberfall fishing: {} lost a fight ({}): species {}", player->GetName(), reason, st.species);
    }

    // The angler as END finds them: alive, unmounted, channelling Reeling, near the bobber.
    bool AnglerHolds(Player* player, GameObject* go)
    {
        if (!player->IsAlive() || player->IsMounted() || !go)
            return false;
        Spell* sp = player->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
        if (!sp || !sp->GetSpellInfo() || sp->GetSpellInfo()->Id != kSpellReeling)
            return false;
        return player->GetMapId() == go->GetMapId() && player->GetExactDist2d(go) <= kMaxRange;
    }

    void HandleEnd(Player* player, efenc::Message const& msg)
    {
        std::optional<effish::wire::End> end = effish::wire::DecodeEnd(msg);
        if (!end)
        {
            E::SendErr(player, msg.seq, efenc::kFishing, efenc::kErrDecode, "END");
            return;
        }
        FightState st;
        {
            std::lock_guard<std::mutex> guard(s_lock);
            auto it = s_fights.find(Key(player));
            if (it == s_fights.end() || it->second.id != end->fight)
            {
                effish::wire::Result r;
                r.fight = end->fight;
                r.code = std::string(efenc::kErrNoEnc);
                E::Send(player, effish::wire::EncodeResult(E::NextSeq(player), r));
                return;
            }
            st = it->second;
            s_fights.erase(it);   // first END only
        }
        E::Finish(player, st.id);
        uint64 const serverMs = NowMs() - st.startMs;
        GameObject* go = ObjectAccessor::GetGameObject(*player, st.bobber);
        effish::Verdict verdict = effish::CheckEnd(st.params, st.rarity, end->report, uint32(std::min<uint64>(serverMs, 0xFFFFFFFFull)));
        bool const holds = AnglerHolds(player, go);
        bool const landed = end->report.outcome == effish::Outcome::Landed;
        effish::wire::Result res;
        res.fight = st.id;
        {
            std::lock_guard<std::mutex> guard(s_lock);
            F::Stats& stats = s_stats[Key(player)];
            if (landed && verdict == effish::Verdict::Ok && holds)
            {
                ++stats.landed;
                stats.landedMs += serverMs;
                res.best = UpdateBest(player, st.species, st.sizeMm);
                if (st.rarity == F::Rarity::Legendary)
                    s_legendDay[Key(player)][st.species] = NowSec() / 86400;
            }
            else if (verdict != effish::Verdict::Ok || (landed && !holds))
                ++stats.rejected;
        }
        if (landed && verdict == effish::Verdict::Ok && holds)
        {
            go->SetSpellId(0);
            go->loot.clear();
            go->loot.lootOwnerGUID = player->GetGUID();
            go->loot.sourceWorldObjectGUID = go->GetGUID();
            go->loot.AddItem(LootStoreItem(st.item, 0, 100.0f, false, LOOT_MODE_DEFAULT, 0, 1, 1));
            go->SetLootState(GO_ACTIVATED, player);
            player->SendLoot(go->GetGUID(), LOOT_FISHING);
            player->FinishSpell(CURRENT_CHANNELED_SPELL, true);
            res.ok = true;
            res.item = st.item;
            res.sizeMm = st.sizeMm;
            res.grams = st.grams;
            E::Send(player, effish::wire::EncodeResult(E::NextSeq(player), res));
            Record(player, st.species, true, false, st.sizeMm);
            LandEvents(player, st.species);
            LOG_DEBUG("module", "Emberfall fishing: {} landed species {} ({} mm) in {} ms", player->GetName(), st.species, st.sizeMm, serverMs);
            return;
        }
        if (verdict == effish::Verdict::Ok && !landed)
        {
            // an honest loss (or one we cannot tell from it): accepted, nothing landed
            res.ok = true;
            res.sizeMm = st.sizeMm;
            res.grams = st.grams;
        }
        else
        {
            res.code = landed && verdict == effish::Verdict::Ok ? std::string(efenc::kErrRange) : effish::VerdictCode(verdict);
            effish::EndReport const& r = end->report;
            LOG_WARN("module", "Emberfall fishing: {} (guid {}) END rejected {}: species {} size {} outcome {} server {} ms reported {} ms "
                     "fastest {} ms peak {} over900 {} ms inputs {} s phases {}/{}/{} angler {}",
                     player->GetName(), player->GetGUID().GetCounter(), res.code, st.species, st.sizeMm, char(r.outcome), serverMs,
                     r.durationMs, effish::FastestLanding(st.params).landMs, r.peakTension, r.over900Ms, r.inputSeconds,
                     r.phaseEndMs[0], r.phaseEndMs[1], r.phaseEndMs[2], holds ? "holds" : "gone");
        }
        E::Send(player, effish::wire::EncodeResult(E::NextSeq(player), res));
        StopReeling(player);
        DespawnBobber(player, st.bobber);
        Record(player, st.species, false, true, 0);
    }

    // ---- the kind ('F') ------------------------------------------------------------------------------------------------
    class FishingKind : public E::Kind
    {
    public:
        bool Enabled() const override { return s_enable; }

        void OnMessage(Player* player, efenc::Message const& msg) override
        {
            if (msg.cmd == "HELLO")
            {
                uint64 hash = 0;
                {
                    std::lock_guard<std::mutex> guard(s_lock);
                    hash = s_data.hash;
                }
                std::optional<uint64> theirs = msg.fields.empty() ? std::nullopt : efenc::ParseUint<uint64>(msg.fields[0], 16);
                if (theirs && *theirs == hash)
                {
                    efenc::Message ok;
                    ok.seq = E::NextSeq(player);
                    ok.kind = efenc::kFishing;
                    ok.cmd = "OK";
                    E::Send(player, efenc::Encode(ok));
                }
                else
                    E::SendErr(player, E::NextSeq(player), efenc::kFishing, efenc::kErrData, efenc::Hex(hash));
                return;
            }
            if (msg.cmd == "END")
            {
                HandleEnd(player, msg);
                return;
            }
            if (msg.cmd == "BOOK")
            {
                // the fishing journal (fishing.md §13): every species this character has hooked, in id order
                std::vector<effish::wire::BookEntry> entries;
                {
                    std::lock_guard<std::mutex> guard(s_lock);
                    auto it = s_book.find(Key(player));
                    if (it != s_book.end())
                        for (auto const& [id, e] : it->second)
                            if (e.caught || e.landed)
                                entries.push_back(e);
                }
                std::sort(entries.begin(), entries.end(), [](auto const& a, auto const& b) { return a.species < b.species; });
                uint32 const parts = uint32(entries.empty() ? 1 : (entries.size() + effish::wire::kBookPerPart - 1) / effish::wire::kBookPerPart);
                uint32 const first = E::NextSeq(player);
                for (uint32 k = 1; k < parts; ++k)
                    E::NextSeq(player);
                for (std::string const& body : effish::wire::EncodeBook(first, entries))
                    E::Send(player, body);
                return;
            }
            E::SendErr(player, msg.seq, efenc::kFishing, efenc::kErrDecode, msg.cmd);
        }

        void OnFree(Player* player, E::Open const& /*open*/, E::FreeReason reason) override
        {
            switch (reason)
            {
                case E::FreeReason::Logout: CloseFight(player, "GONE", false); break;
                case E::FreeReason::MapChange: CloseFight(player, "GONE"); break;
                case E::FreeReason::Died: CloseFight(player, "DIED"); break;
            }
        }
    };
    FishingKind s_kind;

    // ---- the hook click -----------------------------------------------------------------------------------------------
    uint32 BaitOf(Player* player)
    {
        for (auto const& [aura, item] : s_data.baitByAura)
            if (player->HasAura(aura))
                return item;
        return 0;
    }

    bool GroundBaitNear(Player* player, GameObject* go)
    {
        auto it = s_groundBait.find(Key(player));
        if (it == s_groundBait.end() || it->second.untilSec < NowSec() || it->second.map != go->GetMapId())
            return false;
        float const dx = it->second.x - go->GetPositionX(), dy = it->second.y - go->GetPositionY();
        return dx * dx + dy * dy <= kMaxRange * kMaxRange;
    }

    F::Legend LegendFor(Player* player, GameObject* go, effish::Species const& s)
    {
        if (s.needsGroundBait && !GroundBaitNear(player, go))
            return F::Legend::Closed;
        if (s.gateQuest == 0)
            return F::Legend::Open;
        if (player->GetQuestStatus(s.gateQuest) == QUEST_STATUS_INCOMPLETE)
            return F::Legend::Questing;
        if (player->GetQuestRewardStatus(s.gateQuest))
        {
            auto const& days = s_legendDay[Key(player)];
            auto d = days.find(s.id);
            if (d != days.end() && d->second == NowSec() / 86400)
                return F::Legend::Closed;   // once a day
            return F::Legend::Open;
        }
        return F::Legend::Closed;
    }

    // The hook click in a habitat cell. Returns false to let the core handle the bobber.
    bool OnHook(Player* player, GameObject* go)
    {
        if (!s_enable || !E::Enabled() || !player || !go || go->GetOwnerGUID() != player->GetGUID())
            return false;
        {
            std::lock_guard<std::mutex> guard(s_lock);
            auto f = s_fights.find(Key(player));
            if (f != s_fights.end() && f->second.bobber == go->GetGUID())
                return true;   // a click on the bobber during its fight: nothing
        }
        if (go->getLootState() != GO_READY)
            return false;      // too early or too late: the core's "nothing hooked"

        uint32 zone = 0, area = 0;
        go->GetZoneAndAreaId(zone, area);
        int32 zoneSkill = sObjectMgr->GetFishingBaseSkillLevel(area);
        if (!zoneSkill)
            zoneSkill = sObjectMgr->GetFishingBaseSkillLevel(zone);
        int32 const skill = player->GetSkillValue(SKILL_FISHING);
        GameObject* hole = go->LookupFishingHoleAround(20.0f + CONTACT_DISTANCE);

        F::RollResult roll;
        effish::Species species;
        {
            std::lock_guard<std::mutex> guard(s_lock);
            F::Cell const* c = s_data.CellAt(go->GetMapId(), go->GetPositionX(), go->GetPositionY());
            if (!c)
                return false;   // no habitat here: the core's fishing, unchanged
            F::RollInput in;
            in.water = c->water;
            in.mask = c->mask;
            in.timeBit = TimeBitNow();
            in.weatherBits = WeatherBitsOf(zone);
            in.skill = skill;
            in.zoneBase = zoneSkill;
            in.shoal = hole != nullptr;
            if (hole)
            {
                auto s = s_shoalItem.find(hole->GetEntry());
                in.shoalItem = s == s_shoalItem.end() ? 0 : s->second;
            }
            in.baitItem = BaitOf(player);
            in.legend = [&](effish::Species const& s) { return LegendFor(player, go, s); };
            auto forced = s_forced.find(Key(player));
            if (forced != s_forced.end())
            {
                in.forced = forced->second;
                s_forced.erase(forced);
            }
            effish::Rng rng((uint64(rand32()) << 32) | rand32());
            roll = F::Roll(s_data, in, rng);
            if (roll.species)
                species = *roll.species;
        }

        // the skill-up, as the core: every hooked bobber, catch or junk
        if (sScriptMgr->OnPlayerUpdateFishingSkill(player, skill, zoneSkill, roll.chance, roll.roll))
            player->UpdateFishingSkill();

        if (roll.kind == F::RollKind::Junk)
        {
            player->SendLoot(go->GetGUID(), LOOT_FISHING_JUNK);
            player->FinishSpell(CURRENT_CHANNELED_SPELL, true);
            return true;
        }

        if (hole)
        {
            // the shoal used once more, as the core counts its catches (it moves on when fished out)
            hole->AddUse();
            if (hole->GetUseCount() >= hole->GetGOValue()->FishingHole.MaxOpens)
                hole->SetLootState(GO_JUST_DEACTIVATED);
        }

        // Keep the bobber owned while its loot or its fight is open; the channel's end must not take it.
        go->SetSpellId(0);
        uint32 const grams = effish::WeightGrams(species.weightK, roll.sizeMm);

        if (species.rarity == F::Rarity::Common)
        {
            go->loot.clear();
            go->loot.lootOwnerGUID = player->GetGUID();
            go->loot.sourceWorldObjectGUID = go->GetGUID();
            go->loot.AddItem(LootStoreItem(species.item, 0, 100.0f, false, LOOT_MODE_DEFAULT, 0, 1, 1));
            go->SetLootState(GO_ACTIVATED, player);
            player->SendLoot(go->GetGUID(), LOOT_FISHING);
            player->FinishSpell(CURRENT_CHANNELED_SPELL, true);
            bool best = false;
            {
                std::lock_guard<std::mutex> guard(s_lock);
                best = UpdateBest(player, species.id, roll.sizeMm);
            }
            Record(player, species.id, true, false, roll.sizeMm);
            if (E::Enabled())
                E::Send(player, effish::wire::EncodeCatch(E::NextSeq(player), {species.id, roll.sizeMm, grams, best}));
            LandEvents(player, species.id);
            return true;
        }

        // the fight
        std::optional<E::Open> open = E::Begin(player, efenc::kFishing, go->GetGUID().GetRawValue());
        if (!open)
        {
            // another encounter is open (taming, later): the line brings junk
            player->SendLoot(go->GetGUID(), LOOT_FISHING_JUNK);
            player->FinishSpell(CURRENT_CHANNELED_SPELL, true);
            return true;
        }
        go->SetLootState(GO_ACTIVATED, player);   // no escape timer, no loot made at a later click
        player->FinishSpell(CURRENT_CHANNELED_SPELL, true);
        player->CastSpell(player, kSpellReeling, true);
        player->SetGuidValue(UNIT_FIELD_CHANNEL_OBJECT, go->GetGUID());

        FightState st;
        st.id = open->id;
        st.bobber = go->GetGUID();
        st.species = species.id;
        st.item = species.item;
        st.sizeMm = roll.sizeMm;
        st.grams = grams;
        st.rarity = species.rarity;
        st.startMs = NowMs();
        st.map = player->GetMapId();
        st.x = player->GetPositionX();
        st.y = player->GetPositionY();
        effish::wire::Open o;
        bool logRate = false;
        {
            std::lock_guard<std::mutex> guard(s_lock);
            uint64 const seed = (uint64(rand32()) << 32) | rand32();
            st.params = effish::MakeParams(species, [](uint32 id) { return s_data.StyleOf(id); }, roll.sizeMm,
                                           effish::SafeBand(skill, zoneSkill), uint32(player->GetExactDist2d(go) * 1000.0f), seed);
            s_fights[Key(player)] = st;
            logRate = s_stats[Key(player)].Count(NowSec());
            o.fight = st.id;
            o.bobber = go->GetGUID().GetRawValue();
            o.species = species.id;
            o.sizeMm = roll.sizeMm;
            o.params = st.params;
            for (uint32 k = 0; k < st.params.phaseCount && k < 3; ++k)
                o.styleIds[k] = st.params.styles[k].id;
        }
        if (logRate)
            LOG_WARN("module", "Emberfall fishing: {} (guid {}) over 150 fights this hour", player->GetName(), player->GetGUID().GetCounter());
        E::Send(player, effish::wire::EncodeOpen(E::NextSeq(player), o));
        LOG_DEBUG("module", "Emberfall fishing: {} hooked species {} ({} mm), fight {}", player->GetName(), species.id, roll.sizeMm, st.id);
        return true;
    }

    // ---- scripts ------------------------------------------------------------------------------------------------------
    class go_ef_fishing_bobber : public GameObjectScript
    {
    public:
        go_ef_fishing_bobber() : GameObjectScript("go_ef_fishing_bobber") { }
        bool OnGossipHello(Player* player, GameObject* go) override { return OnHook(player, go); }
    };

    class at_ef_fish_watch : public AreaTriggerScript
    {
    public:
        at_ef_fish_watch() : AreaTriggerScript("at_ef_fish_watch") { }

        bool OnTrigger(Player* player, AreaTrigger const* trigger) override
        {
            if (!s_enable || !player || !trigger)
                return false;
            std::vector<uint32> done;
            {
                std::lock_guard<std::mutex> guard(s_lock);
                uint8 const timeBit = TimeBitNow();
                uint8 const weather = WeatherBitsOf(player->GetZoneId());
                for (Event const& e : s_events)
                {
                    if (e.kind != 1 || e.trigger != trigger->entry)
                        continue;
                    effish::Species const* s = s_data.SpeciesById(e.species);
                    if (s && effish::ConditionsHold(*s, timeBit, weather))
                        done.push_back(e.quest);
                }
            }
            for (uint32 q : done)
                if (player->GetQuestStatus(q) == QUEST_STATUS_INCOMPLETE)
                    player->AreaExploredOrEventHappens(q);
            return false;
        }
    };

    // Every zone's weather as it changes (Weather::UpdateWeather / SetWeather call the global hook), for the conditions
    // and "after rain".
    class emberfall_fishing_weather : public ALEScript
    {
    public:
        emberfall_fishing_weather() : ALEScript("emberfall_fishing_weather") { }

        void OnWeatherChange(Weather* weather, WeatherState state, float /*grade*/) override
        {
            if (!weather)
                return;
            std::lock_guard<std::mutex> guard(s_lock);
            Weatherish& w = s_weather[weather->GetZone()];
            bool const rain = effish::WeatherClassOf(uint32(state)) == effish::kRain || effish::WeatherClassOf(uint32(state)) == effish::kStorm;
            if (w.raining && !rain)
                w.rainEndedSec = NowSec();
            w.raining = rain;
            w.state = uint32(state);
        }
    };

    class emberfall_fishing_world : public WorldScript
    {
    public:
        emberfall_fishing_world()
            : WorldScript("emberfall_fishing_world", { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_LOAD_CUSTOM_DATABASE_TABLE }) { }

        void OnAfterConfigLoad(bool /*reload*/) override
        {
            s_enable = sConfigMgr->GetOption<bool>("Emberfall.Fishing.LivingWaters", true, false);
        }

        void OnLoadCustomDatabaseTable() override { Load(); }
    };

    class emberfall_fishing_player : public PlayerScript
    {
    public:
        emberfall_fishing_player()
            : PlayerScript("emberfall_fishing_player", { PLAYERHOOK_ON_UPDATE, PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_LOGOUT }) { }

        void OnPlayerLogin(Player* player) override
        {
            std::unordered_map<uint32, effish::wire::BookEntry> book;
            if (QueryResult r = CharacterDatabase.Query("SELECT `species`, `caught`, `landed`, `lost`, `best_mm`, `first_caught` "
                                                        "FROM `ef_character_fish` WHERE `guid` = {}", player->GetGUID().GetCounter()))
                do
                {
                    Field* f = r->Fetch();
                    effish::wire::BookEntry e{f[0].Get<uint32>(), f[1].Get<uint32>(), f[2].Get<uint32>(), f[3].Get<uint32>(),
                                              f[4].Get<uint32>(), f[5].Get<uint32>()};
                    book[e.species] = e;
                } while (r->NextRow());
            std::lock_guard<std::mutex> guard(s_lock);
            s_book[Key(player)] = std::move(book);
        }

        void OnPlayerLogout(Player* player) override
        {
            std::lock_guard<std::mutex> guard(s_lock);
            s_book.erase(Key(player));
            s_groundBait.erase(Key(player));
            s_forced.erase(Key(player));
        }

        // The fight's watch: the channel held, the angler alive, unmounted and near the bobber, the time.
        void OnPlayerUpdate(Player* player, uint32 /*diff*/) override
        {
            ObjectGuid bobber;
            uint64 startMs = 0;
            uint32 maxMs = 0;
            float x = 0, y = 0;
            {
                std::lock_guard<std::mutex> guard(s_lock);
                if (s_fights.empty())
                    return;
                auto it = s_fights.find(Key(player));
                if (it == s_fights.end())
                    return;
                bobber = it->second.bobber;
                startMs = it->second.startMs;
                maxMs = it->second.params.maxMs;
                x = it->second.x;
                y = it->second.y;
            }
            GameObject* go = ObjectAccessor::GetGameObject(*player, bobber);
            if (!go)
                CloseFight(player, "GONE");
            else if (!player->IsAlive())
                CloseFight(player, "DIED");
            else if (player->IsMounted() || player->GetExactDist2d(go) > kMaxRange)
                CloseFight(player, "MOVED");
            else if (NowMs() - startMs > uint64(maxMs) + 10000)
                CloseFight(player, "TIMEOUT");
            else
            {
                Spell* sp = player->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
                if (!sp || !sp->GetSpellInfo() || sp->GetSpellInfo()->Id != kSpellReeling)
                {
                    float const dx = player->GetPositionX() - x, dy = player->GetPositionY() - y;
                    CloseFight(player, dx * dx + dy * dy > 0.25f ? "MOVED" : "CHANNEL");
                }
            }
        }
    };

    class emberfall_fishing_commands : public CommandScript
    {
    public:
        emberfall_fishing_commands() : CommandScript("emberfall_fishing_commands") { }

        ChatCommandTable GetCommands() const override
        {
            static ChatCommandTable fishTable =
            {
                { "cell",   HandleCell,   SEC_GAMEMASTER, Console::No },
                { "force",  HandleForce,  SEC_GAMEMASTER, Console::No },
                { "stats",  HandleStats,  SEC_GAMEMASTER, Console::No },
                { "reload", HandleReload, SEC_GAMEMASTER, Console::Yes },
            };
            static ChatCommandTable commandTable =
            {
                { "effish", fishTable },
            };
            return commandTable;
        }

        static char const* RarityName(F::Rarity r)
        {
            switch (r)
            {
                case F::Rarity::Common: return "common";
                case F::Rarity::Uncommon: return "uncommon";
                case F::Rarity::Rare: return "rare";
                case F::Rarity::Legendary: return "legendary";
            }
            return "?";
        }

        // The cell under the angler's bobber (a bobber of theirs within 40 yd), else under the angler.
        static bool HandleCell(ChatHandler* handler)
        {
            Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
            if (!player)
                return false;
            WorldObject* at = player;
            std::list<GameObject*> bobbers;
            player->GetGameObjectListWithEntryInGrid(bobbers, 90190, 40.0f);
            for (GameObject* b : bobbers)
                if (b->GetOwnerGUID() == player->GetGUID())
                    at = b;
            uint32 zone = 0, area = 0;
            at->GetZoneAndAreaId(zone, area);
            int32 zoneSkill = sObjectMgr->GetFishingBaseSkillLevel(area);
            if (!zoneSkill)
                zoneSkill = sObjectMgr->GetFishingBaseSkillLevel(zone);
            std::lock_guard<std::mutex> guard(s_lock);
            int32 const cx = effish::CellOf(at->GetPositionX()), cy = effish::CellOf(at->GetPositionY());
            F::Cell const* c = s_data.CellAt(at->GetMapId(), at->GetPositionX(), at->GetPositionY());
            if (!c)
            {
                handler->PSendSysMessage("EFFISH cell {} {} map {} ({}): no habitat cell: the core's fishing", cx, cy, at->GetMapId(),
                                         at == player ? "you" : "your bobber");
                return true;
            }
            auto w = s_data.waters.find(c->water);
            std::string habitats;
            static char const* const kNames[] = {"shallows", "deep", "inlet", "reeds", "rocks", "ruins"};
            for (int i = 0; i < effish::kHabitatCount; ++i)
                if (c->mask & (1u << i))
                    habitats += std::string(habitats.empty() ? "" : ",") + kNames[i];
            uint8 const timeBit = TimeBitNow();
            uint8 const weather = WeatherBitsOf(zone);
            int32 const skill = player->GetSkillValue(SKILL_FISHING);
            handler->PSendSysMessage("EFFISH cell {} {} map {} ({}): water {} \"{}\" habitats {} depth {:.1f} yd; time bit {} weather bits {} "
                                     "skill {} zone base {} bait {}", cx, cy, at->GetMapId(), at == player ? "you" : "your bobber", c->water,
                                     w == s_data.waters.end() ? "?" : w->second.name, habitats.empty() ? "none" : habitats,
                                     c->depthDm / 10.0, timeBit, weather, skill, zoneSkill, BaitOf(player));
            for (effish::Species const& s : s_data.species)
            {
                uint64 const act = effish::Activity(s, c->water, c->mask, timeBit, weather);
                if (!act)
                    continue;
                F::RollInput in;
                in.water = c->water;
                in.mask = c->mask;
                in.timeBit = timeBit;
                in.weatherBits = weather;
                in.skill = skill;
                in.zoneBase = zoneSkill;
                in.baitItem = BaitOf(player);
                handler->PSendSysMessage("EFFISH  {} \"{}\" {} activity {} weight now {}{}", s.id, s_data.names[s.id], RarityName(s.rarity),
                                         act, F::Weight(s_data, s, in),
                                         effish::PassesGate(s.rarity, skill, zoneSkill) ? "" : " (under its skill gate)");
            }
            return true;
        }

        static bool HandleForce(ChatHandler* handler, uint32 species)
        {
            Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
            if (!player)
                return false;
            std::lock_guard<std::mutex> guard(s_lock);
            if (!s_data.SpeciesById(species))
            {
                handler->PSendSysMessage("EFFISH no species {}", species);
                return true;
            }
            s_forced[Key(player)] = species;
            handler->PSendSysMessage("EFFISH your next bite in a habitat cell of its water is species {} \"{}\"", species, s_data.names[species]);
            return true;
        }

        static bool HandleStats(ChatHandler* handler, Optional<PlayerIdentifier> target)
        {
            if (!target)
                target = PlayerIdentifier::FromTargetOrSelf(handler);
            Player* player = target ? target->GetConnectedPlayer() : nullptr;
            if (!player)
            {
                handler->SendSysMessage("EFFISH no such player online");
                return true;
            }
            std::lock_guard<std::mutex> guard(s_lock);
            F::Stats const& s = s_stats[Key(player)];
            handler->PSendSysMessage("EFFISH {}: {} fights since the server started, {} landed ({:.0f} %), {} rejected, mean landing {:.1f} s, "
                                     "{} this hour", player->GetName(), s.fights, s.landed, s.fights ? 100.0 * s.landed / s.fights : 0.0,
                                     s.rejected, s.landed ? s.landedMs / 1000.0 / s.landed : 0.0, s.hourFights);
            return true;
        }

        static bool HandleReload(ChatHandler* handler)
        {
            Load();
            std::lock_guard<std::mutex> guard(s_lock);
            handler->PSendSysMessage("EFFISH reloaded: {} waters, {} cells, {} species", s_data.waters.size(), s_data.cells.size(),
                                     s_data.species.size());
            return true;
        }
    };
}

// Ground-bait (99265): where it went in, for the legendary's bite (fishing.md §4.5, §7.4).
class spell_ef_fish_groundbait : public SpellScript
{
    PrepareSpellScript(spell_ef_fish_groundbait);

    void HandleAfterCast()
    {
        Player* player = GetCaster() ? GetCaster()->ToPlayer() : nullptr;
        if (!player)
            return;
        // in front of the angler, where a cast would land
        float const o = player->GetOrientation();
        GroundBait g;
        g.map = player->GetMapId();
        g.x = player->GetPositionX() + 8.0f * std::cos(o);
        g.y = player->GetPositionY() + 8.0f * std::sin(o);
        g.untilSec = NowSec() + kGroundBaitSeconds;
        std::lock_guard<std::mutex> guard(s_lock);
        s_groundBait[Key(player)] = g;
    }

    void Register() override { AfterCast += SpellCastFn(spell_ef_fish_groundbait::HandleAfterCast); }
};

void AddEmberfallFishingScripts()
{
    Emberfall::Encounters::RegisterKind(efenc::kFishing, &s_kind);
    new emberfall_fishing_world();
    new emberfall_fishing_player();
    new go_ef_fishing_bobber();
    new at_ef_fish_watch();
    new emberfall_fishing_weather();
    new emberfall_fishing_commands();
    RegisterSpellScript(spell_ef_fish_groundbait);
}
