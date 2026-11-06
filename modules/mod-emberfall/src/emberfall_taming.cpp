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

// Emberfall hunter taming: Beastcraft, Drowse and Trust (Emberfall docs/design/hunter-taming.md, §14 the build
// contract; feedback #372). The second user of the mini-encounter framework (emberfall_encounters.cpp), and its first
// server-driven kind ('T'): the server keeps every beast's state, ticks it, and sends the bars and slots.
//
//   - Beastcraft (skill 900): every hunter of 10 or more is taught its Apprentice rank (99520) and Drowse Shot (99524)
//     at level-up and login; the ranks follow level and skill (Journeyman 20 / 50, Craftsman 30 / 125, Master 40 / 200)
//     at level-up, login, every skill-up and every tame; recipes come with the skill value (the core's
//     learnSkillRewardedSpells). Gentling (93120, retired) leaves the action bars at login.
//   - Drowse Shot (spell_ef_drowse_shot): the old Gentling checks (level, tameable, not owned, room in the stable when
//     a companion is out), someone else's quarry refused; the hit uses the strongest drowse arrow the bags hold that
//     the beast's level allows, adds Drowse, renews the beast's leash timer and sends BARS (id 0) to the group.
//   - Out Cold (99525, spell_ef_out_cold): Drowse full; the beast's combat stops, it lies down, immune, for 60 s.
//     The claimant's client asks to open it (EFN OPEN); PUT and TAKE move food and draughts between the bags and its
//     six slots; it eats every 4 s, drinks a draught under 15 s of sleep; Trust full: the tame.
//   - The tame (from the claimant's own update, never inside the beast's): an out companion is stabled first, Out Cold
//     comes off, the hunter casts gentling_tame (93121: TAME_CREATURE and the class quests' credit), the companion
//     starts Defensive, Beastcraft gains 1 / 3 / 5. Sleep out: the beast wakes, attacks a claimant within 30 yd or
//     goes home; uneaten items go back to him within 40 yd.
//   - Kept in memory only (one State per beast, by its GUID); freed on death, evade, tame, wake, and a sweep drops
//     states no creature has ticked for 30 s (grid unload, despawn). The Out Cold aura has its own duration, so a beast
//     the module forgets wakes anyway.
//   - Every tameable beast has a beast family (§15, ef_tame_beast: wolves, bears, herons ...), and its tastes are its
//     family's (ef_tame_family, ef_tame_like); its CreatureFamily is only what it becomes (the companion).
//   - The compendium's food lines (§16): every item a hunter feeds (or offers) a family is kept in the characters DB
//     (ef_character_tame_known) and sent with the taste after the client's EFN HELLO (KNOWN), so they follow him to any
//     machine.
//   - `.eftame state` (the targeted beast), `.eftame force` (Drowse or Trust full), `.eftame reload`.
// Config: Emberfall.Taming.Enable (1).

#include "AllCreatureScript.h"
#include "Bag.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "CommandScript.h"
#include "Config.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Group.h"
#include "GroupReference.h"
#include "Item.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Pet.h"
#include "Player.h"
#include "PlayerScript.h"
#include "ScriptMgr.h"
#include "SpellAuras.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"
#include "UnitScript.h"
#include "WorldScript.h"
#include "encounters/EfEncounterMgr.h"
#include "taming/EfTameLogic.h"
#include "taming/EfTameWire.h"
#include <atomic>
#include <iterator>
#include <mutex>
#include <set>
#include <unordered_map>
#include <utility>

using namespace Acore::ChatCommands;

namespace
{
    namespace E = Emberfall::Encounters;
    namespace T = Emberfall::Taming;
    namespace W = eftame::wire;

    constexpr uint32 kSkillBeastcraft = 900;
    constexpr uint32 kSpellApprentice = 99520;
    constexpr uint32 kSpellJourneyman = 99521;
    constexpr uint32 kSpellCraftsman = 99522;
    constexpr uint32 kSpellMaster = 99523;
    constexpr uint32 kSpellDrowseShot = 99524;
    constexpr uint32 kSpellOutCold = 99525;
    constexpr uint32 kSpellGentling = 93120;       // retired
    constexpr uint32 kSpellGentlingTame = 93121;
    constexpr uint32 kLearnLevel = 10;
    constexpr char const* kFoodGroup = "tame_food";

    constexpr float kFrameRange = 6.0f;            // 5 yd and a yard for the latency
    constexpr float kTameRange = 40.0f;
    constexpr float kAttackRange = 30.0f;
    constexpr uint32 kTickMs = 500;
    constexpr uint32 kDrowseBarsMs = 1000;
    constexpr uint64 kStaleMs = 30000;
    constexpr uint64 kIdleDrowsingMs = 30000;      // an empty Drowse bar this long without a hit: forgotten

    // the ranks: the rank spell, the level and the skill it needs
    struct RankStep
    {
        uint32 spell, level, skill;
    };
    constexpr RankStep kRanks[] = {{kSpellJourneyman, 20, 50}, {kSpellCraftsman, 30, 125}, {kSpellMaster, 40, 200}};

    struct Entry
    {
        T::State s;
        uint64 lastTickMs = 0;
        uint64 lastBarsMs = 0;
        uint32 lastBarsDrowse = 0;
        bool slotsDirty = false;
        bool tameDue = false;
        uint64 openFor = 0;        // the claimant's open frame: its encounter id (0: none)
    };

    bool s_enable = true;
    // the rule, tastes, arrows, draughts and states; recursive: Out Cold's aura script takes it, and the module removes
    // that aura while holding it
    std::recursive_mutex s_lock;
    T::Rule s_rule;
    T::Tastes s_tastes;
    std::vector<T::Arrow> s_arrows;
    std::unordered_map<uint32, T::Draught> s_draughts;
    std::unordered_map<uint64, Entry> s_states;      // by the beast's raw GUID
    std::unordered_map<uint32, uint32> s_beasts;     // creature entry -> its beast family (ef_tame_beast, §15)
    std::atomic<uint32> s_count{0};

    // What each hunter online has fed each family (ef_character_tame_known, §16): by the player's GUID counter, the
    // (family, item) pairs; loaded at login, added at every bite and sniff, dropped at logout. Under s_lock.
    std::unordered_map<uint32, std::set<std::pair<uint32, uint32>>> s_known;

    uint64 NowMs() { return uint64(GameTime::GetGameTimeMS().count()); }

    void CountStates() { s_count.store(uint32(s_states.size())); }

    bool IsDraught(uint32 item) { return s_draughts.count(item) != 0; }

    void Load()
    {
        std::lock_guard<std::recursive_mutex> guard(s_lock);
        s_rule = T::Rule{};
        if (QueryResult r = WorldDatabase.Query(
                "SELECT `drowse_base`, `drowse_per_level`, `trust_base`, `trust_per_level`, `drowse_elite`, `drowse_rare`, "
                "`drowse_rare_elite`, `trust_elite`, `trust_rare`, `trust_rare_elite`, `out_cold_ms`, `eat_every_ms`, `decay_after_ms`, "
                "`decay_permil`, `draught_under_ms`, `sleep_cap_ms`, `above_level_permil`, `band_step_permil` FROM `ef_tame_rule` WHERE `id` = 1"))
        {
            Field* f = r->Fetch();
            T::Rule& x = s_rule;
            uint32* fields[] = {&x.drowseBase, &x.drowsePerLevel, &x.trustBase, &x.trustPerLevel, &x.drowseElite, &x.drowseRare,
                                &x.drowseRareElite, &x.trustElite, &x.trustRare, &x.trustRareElite, &x.outColdMs, &x.eatEveryMs,
                                &x.decayAfterMs, &x.decayPermil, &x.draughtUnderMs, &x.sleepCapMs, &x.aboveLevelPermil, &x.bandStepPermil};
            for (std::size_t i = 0; i < std::size(fields); ++i)
                *fields[i] = f[i].Get<uint32>();
        }
        s_tastes.Clear();
        if (QueryResult r = WorldDatabase.Query("SELECT `family`, `other_permil` FROM `ef_tame_family`"))
            do
            {
                s_tastes.SetFamily(r->Fetch()[0].Get<uint32>(), r->Fetch()[1].Get<uint32>());
            } while (r->NextRow());
        if (QueryResult r = WorldDatabase.Query("SELECT `family`, `group_name`, `permil` FROM `ef_tame_like` ORDER BY `family`, `ord`"))
            do
            {
                Field* f = r->Fetch();
                s_tastes.AddLike(f[0].Get<uint32>(), f[1].Get<std::string>(), f[2].Get<uint32>());
            } while (r->NextRow());
        s_beasts.clear();
        if (QueryResult r = WorldDatabase.Query("SELECT `entry`, `family` FROM `ef_tame_beast`"))
            do
            {
                s_beasts[r->Fetch()[0].Get<uint32>()] = r->Fetch()[1].Get<uint32>();
            } while (r->NextRow());
        s_arrows.clear();
        if (QueryResult r = WorldDatabase.Query("SELECT `item`, `drowse`, `max_level`, `skill` FROM `ef_tame_arrow`"))
            do
            {
                Field* f = r->Fetch();
                s_arrows.push_back({f[0].Get<uint32>(), f[1].Get<uint32>(), f[2].Get<uint32>(), f[3].Get<uint32>()});
            } while (r->NextRow());
        s_draughts.clear();
        if (QueryResult r = WorldDatabase.Query("SELECT `item`, `sleep_ms`, `max_level` FROM `ef_tame_draught`"))
            do
            {
                Field* f = r->Fetch();
                s_draughts[f[0].Get<uint32>()] = {f[0].Get<uint32>(), f[1].Get<uint32>(), f[2].Get<uint32>()};
            } while (r->NextRow());
        LOG_INFO("server.loading", ">> Emberfall taming: {} ({} families, {} beasts, {} arrows, {} draughts)", s_enable ? "on" : "off",
                 s_tastes.Size(), s_beasts.size(), s_arrows.size(), s_draughts.size());
    }

    // A beast's family (§15): its ef_tame_beast row; a creature the content build gave none (an older world DB) falls
    // back to its CreatureFamily, which then has no tastes (everything refused but the draughts).
    uint32 FamilyOf(Creature const* beast)
    {
        auto it = s_beasts.find(beast->GetEntry());
        return it != s_beasts.end() ? it->second : beast->GetCreatureTemplate()->family;
    }

    // ---- the compendium's food lines (§16) --------------------------------------------------------------------------

    void LoadKnown(Player* player)
    {
        std::set<std::pair<uint32, uint32>> known;
        if (QueryResult r = CharacterDatabase.Query("SELECT `family`, `item` FROM `ef_character_tame_known` WHERE `guid` = {}",
                                                    player->GetGUID().GetCounter()))
            do
            {
                known.emplace(r->Fetch()[0].Get<uint32>(), r->Fetch()[1].Get<uint32>());
            } while (r->NextRow());
        std::lock_guard<std::recursive_mutex> guard(s_lock);
        s_known[player->GetGUID().GetCounter()] = std::move(known);
    }

    // The hunter fed (or offered) `item` to a beast of `family`: remembered once. Under s_lock.
    void Learn(Player* player, uint32 family, uint32 item)
    {
        if (!player || !family || !item)
            return;
        uint32 const guid = player->GetGUID().GetCounter();
        if (!s_known[guid].emplace(family, item).second)
            return;
        CharacterDatabase.Execute("INSERT IGNORE INTO `ef_character_tame_known` (`guid`, `family`, `item`) VALUES ({}, {}, {})",
                                  guid, family, item);
    }

    // KNOWN after the HELLO's OK: every family's fed items with what it made of them (the world data's taste now).
    void SendKnown(Player* player)
    {
        std::lock_guard<std::recursive_mutex> guard(s_lock);
        auto it = s_known.find(player->GetGUID().GetCounter());
        if (it == s_known.end() || it->second.empty())
            return;
        E::ItemGroups const& groups = E::Groups();
        W::Known family;
        auto flush = [&]() {
            for (W::Known const& part : W::SplitKnown(family))
                E::Send(player, W::EncodeKnown(E::NextSeq(player), part));
            family.items.clear();
        };
        for (auto const& [fam, item] : it->second)   // ordered by family
        {
            if (fam != family.family)
            {
                flush();
                family.family = fam;
            }
            if (!IsDraught(item))
                family.items.emplace_back(item, T::TasteLetter(s_tastes.Resolve(fam, item, groups).permil));
        }
        flush();
    }

    // ---- the hunter's side ------------------------------------------------------------------------------------------

    // Beastcraft, Drowse Shot and the ranks for a hunter of 10 or more; Gentling off the bars.
    void Reconcile(Player* player)
    {
        if (!s_enable || player->getClass() != CLASS_HUNTER)
            return;
        for (uint8 b = 0; b < MAX_ACTION_BUTTONS; ++b)
            if (ActionButton const* ab = player->GetActionButton(b))
                if (ab->GetType() == ACTION_BUTTON_SPELL && ab->GetAction() == kSpellGentling)
                    player->removeActionButton(b);
        if (player->GetLevel() < kLearnLevel)
            return;
        if (!player->HasSkill(kSkillBeastcraft) && sSpellMgr->GetSpellInfo(kSpellApprentice))
            player->learnSpell(kSpellApprentice);
        if (!player->HasSpell(kSpellDrowseShot) && sSpellMgr->GetSpellInfo(kSpellDrowseShot))
            player->learnSpell(kSpellDrowseShot);
        if (!player->HasSkill(kSkillBeastcraft))
            return;
        uint32 const skill = player->GetPureSkillValue(kSkillBeastcraft);
        for (RankStep const& r : kRanks)
            if (player->GetLevel() >= r.level && skill >= r.skill && !player->HasSpell(r.spell) && sSpellMgr->GetSpellInfo(r.spell))
                player->learnSpell(r.spell);
        player->learnSkillRewardedSpells(kSkillBeastcraft, skill);
    }

    // Gentling's checks (spell_ef_kit_gentling until 2026-11-05), for Drowse Shot: SPELL_CAST_OK or the refusal (the
    // tame failure already sent).
    SpellCastResult TameRefusal(Player* player, Creature* target)
    {
        if (!target || target->IsPet() || target->GetOwnerGUID())
        {
            player->SendTameFailure(target && target->GetOwnerGUID() ? PET_TAME_CREATURE_ALREADY_OWNED : PET_TAME_INVALID_CREATURE);
            return SPELL_FAILED_DONT_REPORT;
        }
        if (target->GetLevel() > player->GetLevel())
        {
            player->SendTameFailure(PET_TAME_TOO_HIGHLEVEL);
            return SPELL_FAILED_DONT_REPORT;
        }
        if (!target->GetCreatureTemplate()->IsTameable(player->CanTameExoticPets()))
        {
            player->SendTameFailure(PET_TAME_NOT_TAMEABLE);
            return SPELL_FAILED_DONT_REPORT;
        }
        // a companion out (or waiting) needs a free stall for the tame (§4.3)
        if (PetStable const* stable = player->GetPetStable(); stable && (stable->CurrentPet || !stable->UnslottedPets.empty()))
        {
            bool room = false;
            for (uint32 i = 0; i < stable->MaxStabledPets && i < MAX_PET_STABLES; ++i)
                room = room || !stable->StabledPets[i];
            if (!room)
            {
                player->SendTameFailure(PET_TAME_TOO_MANY);
                return SPELL_FAILED_DONT_REPORT;
            }
        }
        return SPELL_CAST_OK;
    }

    // Stables the out (or waiting) companion into a free stall; false: no stall free.
    bool StableOut(Player* player)
    {
        PetStable* stable = player->GetPetStable();
        if (!stable || (!stable->CurrentPet && stable->UnslottedPets.empty() && !player->GetPet()))
            return true;
        uint32 slot = MAX_PET_STABLES;
        for (uint32 i = 0; i < stable->MaxStabledPets && i < MAX_PET_STABLES; ++i)
            if (!stable->StabledPets[i])
            {
                slot = i;
                break;
            }
        if (slot == MAX_PET_STABLES)
            return false;
        if (Pet* pet = player->GetPet())
        {
            if (pet->getPetType() != HUNTER_PET)
                return false;
            player->RemovePet(pet, PetSaveMode(PET_SAVE_FIRST_STABLE_SLOT + slot));
            std::swap(stable->StabledPets[slot], stable->CurrentPet);
            return true;
        }
        PetStable::PetInfo info;
        if (stable->CurrentPet)
        {
            info = *stable->CurrentPet;
            stable->CurrentPet.reset();
        }
        else
        {
            info = stable->UnslottedPets.back();
            stable->UnslottedPets.pop_back();
        }
        CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_UPD_CHAR_PET_SLOT_BY_ID);
        stmt->SetData(0, uint8(PET_SAVE_FIRST_STABLE_SLOT + slot));
        stmt->SetData(1, player->GetGUID().GetCounter());
        stmt->SetData(2, info.PetNumber);
        CharacterDatabase.Execute(stmt);
        stable->StabledPets[slot] = std::move(info);
        return true;
    }

    // ---- messages ---------------------------------------------------------------------------------------------------

    std::vector<efenc::BarValue> BarsOf(T::State const& s, uint64 now)
    {
        if (s.phase == T::Phase::Drowsing)
            return {{s.drowse, s.drowseMax}};
        return {{T::SleepLeft(s, now), s.sleepLenMs}, {s.trust, s.trustMax}};
    }

    // The claimant and the members of his group on the beast's map.
    std::vector<Player*> Watchers(Creature* beast, ObjectGuid claimant)
    {
        std::vector<Player*> out;
        Player* lead = ObjectAccessor::GetPlayer(*beast, claimant);
        if (!lead)
            return out;
        if (Group* g = lead->GetGroup())
        {
            for (GroupReference* r = g->GetFirstMember(); r; r = r->next())
                if (Player* m = r->GetSource(); m && m->IsInWorld() && m->GetMap() == beast->GetMap())
                    out.push_back(m);
        }
        else
            out.push_back(lead);
        return out;
    }

    ObjectGuid ClaimantOf(T::State const& s) { return ObjectGuid(T::Asleep(s) ? s.claimant : T::Claimant(s)); }

    void SendBars(Creature* beast, Entry& e, uint64 now, bool over = false)
    {
        ObjectGuid const claimant = ClaimantOf(e.s);
        for (Player* p : Watchers(beast, claimant))
        {
            efenc::Bars b;
            bool const mine = p->GetGUID() == claimant;
            b.id = mine ? uint32(e.openFor) : 0;
            b.subject = e.s.beast;
            b.phase = over ? W::kOver : uint32(e.s.phase);
            b.leftMs = over || e.s.phase == T::Phase::Drowsing ? 0 : T::SleepLeft(e.s, now);
            b.flags = mine ? W::kMine : 0;
            if (!over)
                b.bars = BarsOf(e.s, now);
            E::Send(p, efenc::EncodeBars(E::NextSeq(p), efenc::kTaming, b));
        }
        e.lastBarsMs = now;
        e.lastBarsDrowse = e.s.drowse;
    }

    void SendSlots(Player* player, Entry& e)
    {
        efenc::Slots s;
        s.id = uint32(e.openFor);
        for (T::Slot const& slot : e.s.slots)
            s.slots.push_back({slot.item, slot.count});
        E::Send(player, efenc::EncodeSlots(E::NextSeq(player), efenc::kTaming, s));
        e.slotsDirty = false;
    }

    void SendResult(Player* player, Entry const& e, char outcome)
    {
        if (player)
            E::Send(player, W::EncodeResult(E::NextSeq(player), {uint32(e.openFor), e.s.beast, outcome}));
    }

    // Ends the claimant's frame on this beast (the encounter in the book), with a CLOSE when `reason` is given.
    void CloseFrame(Player* player, Entry& e, char const* reason)
    {
        if (!e.openFor)
            return;
        if (player)
        {
            if (reason)
                E::Send(player, W::EncodeClose(E::NextSeq(player), {uint32(e.openFor), reason}));
            E::Finish(player, uint32(e.openFor));
        }
        e.openFor = 0;
    }

    // ---- the beast's side -------------------------------------------------------------------------------------------

    void LieDown(Creature* beast, Entry& e, uint64 now)
    {
        T::StartOutCold(s_rule, e.s, now);
        beast->CombatStop(true);
        beast->GetThreatMgr().ClearAllThreat();
        beast->AttackStop();
        beast->GetMotionMaster()->Clear();
        beast->GetMotionMaster()->MoveIdle();
        beast->StopMoving();
        if (Aura* aura = beast->AddAura(kSpellOutCold, beast))
        {
            aura->SetMaxDuration(int32(s_rule.outColdMs));
            aura->SetDuration(int32(s_rule.outColdMs));
        }
        beast->SetStandState(UNIT_STAND_STATE_SLEEP);
        LOG_DEBUG("module", "Emberfall taming: {} ({}) out cold, claimed by {}", beast->GetName(), beast->GetEntry(),
                  ObjectGuid(e.s.claimant).ToString());
    }

    // The sleep's end on the aura (a draught renews it).
    void RenewAura(Creature* beast, Entry const& e, uint64 now)
    {
        if (Aura* aura = beast->GetAura(kSpellOutCold))
        {
            int32 const left = int32(T::SleepLeft(e.s, now));
            aura->SetMaxDuration(std::max(aura->GetMaxDuration(), left));
            aura->SetDuration(left);
        }
    }

    void StandUp(Creature* beast)
    {
        beast->RemoveAurasDueToSpell(kSpellOutCold);
        beast->SetStandState(UNIT_STAND_STATE_STAND);
    }

    // Uneaten items back to the claimant (within 40 yd, room in the bags), else lost.
    uint32 ReturnItems(Player* claimant, Creature* beast, Entry& e)
    {
        uint32 lost = 0;
        for (T::Slot& slot : e.s.slots)
        {
            if (!slot.item || !slot.count)
                continue;
            bool const back = claimant && claimant->IsInWorld() && claimant->IsWithinDistInMap(beast, kTameRange) &&
                              claimant->AddItem(slot.item, slot.count);
            if (!back)
                lost += slot.count;
            slot = {};
        }
        return lost;
    }

    // Sleep out (§4.3): the beast wakes; returns nothing (the entry is erased by the caller).
    void Wake(Creature* beast, Entry& e, uint64 now)
    {
        Player* claimant = ObjectAccessor::GetPlayer(*beast, ObjectGuid(e.s.claimant));
        StandUp(beast);
        uint32 const lost = ReturnItems(claimant, beast, e);
        SendResult(claimant, e, W::kWoke);
        SendBars(beast, e, now, true);
        CloseFrame(claimant, e, nullptr);
        LOG_INFO("module", "Emberfall taming: {} ({}, level {}) woke after {} s: trust {}/{}, {} eaten, {} item(s) lost", beast->GetName(),
                 beast->GetEntry(), e.s.level, (now - e.s.startMs) / 1000, e.s.trust / 1000, e.s.trustMax / 1000, e.s.eaten, lost);
        if (claimant && claimant->IsAlive() && claimant->IsWithinDistInMap(beast, kAttackRange) && beast->IsAlive() && beast->AI())
            beast->AI()->AttackStart(claimant);
        else if (beast->AI())
            beast->AI()->EnterEvadeMode();
    }

    // The sleeping beast's 500-ms step: draughts, eating, the frame's range, the bars. Under s_lock.
    void TickAsleep(Creature* beast, Entry& e, uint64 now)
    {
        T::State& s = e.s;
        Player* claimant = ObjectAccessor::GetPlayer(*beast, ObjectGuid(s.claimant));
        // a draught when the sleep runs low
        if (T::SleepLeft(s, now) < s_rule.draughtUnderMs)
        {
            int const d = T::PickDraught(s, [](uint32 item) {
                auto it = s_draughts.find(item);
                return it == s_draughts.end() ? 0u : it->second.sleepMs;
            });
            if (d >= 0)
            {
                uint32 const item = s.slots[d].item;
                T::Consume(s, uint32(d));
                uint32 const added = T::AddSleep(s_rule, s, T::DraughtMs(s_rule, s_draughts[item], s.level), now);
                RenewAura(beast, e, now);
                e.slotsDirty = true;
                if (claimant && e.openFor)
                    E::Send(claimant, W::EncodeEat(E::NextSeq(claimant), {uint32(e.openFor), item, W::kDrank, std::max<uint32>(added, 1)}));
            }
        }
        // eating, one item every eat_every
        if (s.phase == T::Phase::OutCold && now >= s.nextEatMs)
        {
            s.nextEatMs = now + s_rule.eatEveryMs;
            E::ItemGroups const& groups = E::Groups();
            int const f = T::PickFood(s_rule, s, s_tastes, groups, IsDraught);
            if (f >= 0)
            {
                uint32 const item = s.slots[f].item;
                T::Taste const taste = s_tastes.Resolve(s.family, item, groups);
                uint32 const trust = T::FoodTrust(s_rule, taste, s.level);
                T::Consume(s, uint32(f));
                T::AddTrust(s, trust);
                ++s.eaten;
                e.slotsDirty = true;
                Learn(claimant, s.family, item);
                if (claimant && e.openFor)
                    E::Send(claimant, W::EncodeEat(E::NextSeq(claimant), {uint32(e.openFor), item, T::TasteLetter(taste.permil), std::max<uint32>(trust, 1)}));
            }
            else if (int const r = T::FirstRefused(s_rule, s, s_tastes, groups, IsDraught); r >= 0)
            {
                s.sniffed[r] = true;
                Learn(claimant, s.family, s.slots[r].item);
                if (claimant && e.openFor)
                    E::Send(claimant, W::EncodeEat(E::NextSeq(claimant), {uint32(e.openFor), s.slots[r].item, W::kRefused, 1}));
            }
        }
        if (T::TrustFull(s))
            e.tameDue = true;
        // the frame closes beyond 5 yd (the encounter ends; the beast sleeps on)
        if (e.openFor && (!claimant || !claimant->IsWithinDistInMap(beast, kFrameRange)))
            CloseFrame(claimant, e, "RANGE");
        if (claimant && e.openFor && e.slotsDirty)
            SendSlots(claimant, e);
        SendBars(beast, e, now);
    }

    void Forget(uint64 beast)
    {
        s_states.erase(beast);
        CountStates();
    }

    // The 500-ms step of a beast with a state (OnAllCreatureUpdate).
    void Tick(Creature* beast)
    {
        if (!s_count.load())
            return;
        uint64 const key = beast->GetGUID().GetRawValue();
        std::lock_guard<std::recursive_mutex> guard(s_lock);
        auto it = s_states.find(key);
        if (it == s_states.end())
            return;
        Entry& e = it->second;
        uint64 const now = NowMs();
        if (now < e.lastTickMs + kTickMs)
            return;
        uint32 const elapsed = uint32(std::min<uint64>(now - e.lastTickMs, 5000));
        e.lastTickMs = now;
        if (!beast->IsAlive())
        {
            SendBars(beast, e, now, true);
            Forget(key);
            return;
        }
        if (e.s.phase == T::Phase::Drowsing)
        {
            // evaded or out of combat: the state is gone (§8)
            if (beast->IsInEvadeMode() || !beast->IsInCombat())
            {
                SendBars(beast, e, now, true);
                Forget(key);
                return;
            }
            bool const held = beast->HasUnitState(UNIT_STATE_STUNNED | UNIT_STATE_CONFUSED | UNIT_STATE_FLEEING | UNIT_STATE_ROOT);
            T::Decay(s_rule, e.s, now, elapsed, held);
            if (e.s.drowse == 0 && now > e.s.lastDrowseMs + kIdleDrowsingMs)
            {
                SendBars(beast, e, now, true);
                Forget(key);
                return;
            }
            if (e.s.drowse != e.lastBarsDrowse && now >= e.lastBarsMs + kDrowseBarsMs)
                SendBars(beast, e, now);
            return;
        }
        if (T::SleepLeft(e.s, now) == 0 || !beast->HasAura(kSpellOutCold))
        {
            Wake(beast, e, now);
            Forget(key);
            return;
        }
        TickAsleep(beast, e, now);
    }

    // ---- the tame (from the claimant's update) ----------------------------------------------------------------------

    void TryTame(Player* player)
    {
        if (!s_count.load())
            return;
        uint64 const me = player->GetGUID().GetRawValue();
        std::lock_guard<std::recursive_mutex> guard(s_lock);
        for (auto it = s_states.begin(); it != s_states.end(); ++it)
        {
            Entry& e = it->second;
            if (!e.tameDue || e.s.claimant != me)
                continue;
            Creature* beast = ObjectAccessor::GetCreature(*player, ObjectGuid(e.s.beast));
            if (!beast || !beast->IsAlive())
                continue;
            uint64 const now = NowMs();
            if (!player->IsAlive() || !player->IsWithinDistInMap(beast, kTameRange))
            {
                if (e.s.phase != T::Phase::WaitHunter)
                {
                    e.s.phase = T::Phase::WaitHunter;
                    SendBars(beast, e, now);
                }
                continue;
            }
            if (!StableOut(player))
            {
                if (e.s.phase != T::Phase::WaitStable)
                {
                    e.s.phase = T::Phase::WaitStable;
                    SendBars(beast, e, now);
                }
                continue;
            }
            uint32 const entry = beast->GetEntry();
            StandUp(beast);
            ReturnItems(player, beast, e);
            player->CastSpell(beast, kSpellGentlingTame, true);
            Pet* pet = player->GetPet();
            bool const tamed = pet && pet->getPetType() == HUNTER_PET && pet->GetEntry() == entry;
            if (tamed)
            {
                // a new companion starts Defensive (it joins what its hunter attacks), the pet bar told
                pet->SetReactState(REACT_DEFENSIVE);
                pet->SavePetToDB(PET_SAVE_AS_CURRENT);
                player->PetSpellInitialize();
                player->UpdateSkill(kSkillBeastcraft, T::TameSkillPoints(e.s.rank));
            }
            SendResult(player, e, tamed ? W::kTamed : W::kFailed);
            SendBars(beast, e, now, true);
            CloseFrame(player, e, nullptr);
            LOG_INFO("module", "Emberfall taming: {} (guid {}) {} {} ({}, level {}, rank {}) after {} s: {} arrow(s), {} eaten",
                     player->GetName(), player->GetGUID().GetCounter(), tamed ? "tamed" : "failed to tame", beast->GetName(), entry,
                     e.s.level, uint32(e.s.rank), (now - e.s.startMs) / 1000, e.s.arrows, e.s.eaten);
            if (!tamed)
            {
                if (beast->AI())
                    beast->AI()->EnterEvadeMode();
            }
            s_states.erase(it);
            CountStates();
            break;   // the iterator is gone; another due tame waits for the next update
        }
    }

    // ---- the kind ('T') ---------------------------------------------------------------------------------------------

    // Lua's bag numbering (0 the backpack, 1-4 the bags; slots from 1) to the core's.
    bool BagPos(uint32 bag, uint32 bagSlot, uint8& outBag, uint8& outSlot)
    {
        if (bagSlot == 0 || bag > 4)
            return false;
        if (bag == 0)
        {
            if (bagSlot > uint32(INVENTORY_SLOT_ITEM_END - INVENTORY_SLOT_ITEM_START))
                return false;
            outBag = INVENTORY_SLOT_BAG_0;
            outSlot = uint8(INVENTORY_SLOT_ITEM_START + bagSlot - 1);
            return true;
        }
        if (bagSlot > MAX_BAG_SIZE)
            return false;
        outBag = uint8(INVENTORY_SLOT_BAG_START + bag - 1);
        outSlot = uint8(bagSlot - 1);
        return true;
    }

    class TamingKind : public E::Kind
    {
    public:
        bool Enabled() const override { return s_enable; }

        void OnMessage(Player* player, efenc::Message const& msg) override
        {
            if (msg.cmd == "HELLO")
            {
                efenc::Message ok = efenc::MakeMessage(E::NextSeq(player), efenc::kTaming, "OK");
                E::Send(player, efenc::Encode(ok));
                if (player->getClass() == CLASS_HUNTER)
                    SendKnown(player);   // the compendium's food lines (§16)
            }
            else if (msg.cmd == "OPEN")
                HandleOpen(player, msg);
            else if (msg.cmd == "PUT")
                HandlePut(player, msg);
            else if (msg.cmd == "TAKE")
                HandleTake(player, msg);
            else if (msg.cmd == "CLOSE")
            {
                auto c = W::DecodeClose(msg);
                std::lock_guard<std::recursive_mutex> guard(s_lock);
                for (auto& [key, e] : s_states)
                    if (c && e.openFor == c->id && e.s.claimant == player->GetGUID().GetRawValue())
                        e.openFor = 0;
                if (c)
                    E::Finish(player, c->id);
            }
            else
                E::SendErr(player, msg.seq, efenc::kTaming, efenc::kErrDecode, msg.cmd);
        }

        void OnFree(Player* player, E::Open const& open, E::FreeReason reason) override
        {
            std::lock_guard<std::recursive_mutex> guard(s_lock);
            auto it = s_states.find(open.subject);
            if (it != s_states.end() && it->second.openFor == open.id)
                it->second.openFor = 0;
            if (reason != E::FreeReason::Logout)
                E::Send(player, W::EncodeClose(E::NextSeq(player), {open.id, reason == E::FreeReason::Died ? "DIED" : "GONE"}));
        }

    private:
        // The entry of the player's open frame for this message's id, the beast in range; nullptr after an ERR.
        Entry* Frame(Player* player, efenc::Message const& msg, uint32 id, Creature*& beast)
        {
            std::optional<E::Open> open = E::Current(player);
            if (!open || open->kind != efenc::kTaming || open->id != id)
            {
                E::SendErr(player, msg.seq, efenc::kTaming, efenc::kErrNoEnc);
                return nullptr;
            }
            auto it = s_states.find(open->subject);
            if (it == s_states.end() || !T::Asleep(it->second.s))
            {
                E::SendErr(player, msg.seq, efenc::kTaming, efenc::kErrNoEnc, "AWAKE");
                return nullptr;
            }
            if (it->second.s.claimant != player->GetGUID().GetRawValue())
            {
                E::SendErr(player, msg.seq, efenc::kTaming, efenc::kErrClaim);
                return nullptr;
            }
            beast = ObjectAccessor::GetCreature(*player, ObjectGuid(open->subject));
            if (!beast || !player->IsWithinDistInMap(beast, kFrameRange))
            {
                E::SendErr(player, msg.seq, efenc::kTaming, efenc::kErrRange);
                return nullptr;
            }
            return &it->second;
        }

        void HandleOpen(Player* player, efenc::Message const& msg)
        {
            std::optional<uint64> subject = W::DecodeOpenRequest(msg);
            if (!subject)
            {
                E::SendErr(player, msg.seq, efenc::kTaming, efenc::kErrDecode, "OPEN");
                return;
            }
            std::lock_guard<std::recursive_mutex> guard(s_lock);
            auto it = s_states.find(*subject);
            if (it == s_states.end() || !T::Asleep(it->second.s))
            {
                E::SendErr(player, msg.seq, efenc::kTaming, efenc::kErrNoEnc, "AWAKE");
                return;
            }
            Entry& e = it->second;
            if (e.s.claimant != player->GetGUID().GetRawValue())
            {
                E::SendErr(player, msg.seq, efenc::kTaming, efenc::kErrClaim);   // "It sleeps. Leave it be."
                return;
            }
            Creature* beast = ObjectAccessor::GetCreature(*player, ObjectGuid(*subject));
            if (!beast || !beast->IsAlive() || !player->IsAlive() || !player->IsWithinDistInMap(beast, kFrameRange))
            {
                E::SendErr(player, msg.seq, efenc::kTaming, efenc::kErrRange);
                return;
            }
            std::optional<E::Open> open = E::Current(player);
            if (open && !(open->kind == efenc::kTaming && open->subject == *subject))
            {
                E::SendErr(player, msg.seq, efenc::kTaming, efenc::kErrNoEnc, "BUSY");
                return;
            }
            if (!open)
                open = E::Begin(player, efenc::kTaming, *subject);
            if (!open)
                return;
            e.openFor = open->id;
            W::Open o;
            o.id = open->id;
            o.beast = *subject;
            o.family = e.s.family;
            o.entry = beast->GetEntry();
            o.level = e.s.level;
            o.rank = beast->GetCreatureTemplate()->rank;
            o.slots = uint32(T::kSlots);
            E::Send(player, W::EncodeOpen(E::NextSeq(player), o));
            SendSlots(player, e);
            SendBars(beast, e, NowMs());
        }

        void HandlePut(Player* player, efenc::Message const& msg)
        {
            std::optional<efenc::Put> put = efenc::DecodePut(msg);
            if (!put)
            {
                E::SendErr(player, msg.seq, efenc::kTaming, efenc::kErrDecode, "PUT");
                return;
            }
            std::lock_guard<std::recursive_mutex> guard(s_lock);
            Creature* beast = nullptr;
            Entry* e = Frame(player, msg, put->id, beast);
            if (!e)
                return;
            uint8 bag = 0, slot = 0;
            Item* item = BagPos(put->bag, put->bagSlot, bag, slot) ? player->GetItemByPos(bag, slot) : nullptr;
            if (!item || put->slot >= T::kSlots)
            {
                E::SendErr(player, msg.seq, efenc::kTaming, efenc::kErrBounds);
                return;
            }
            uint32 const entry = item->GetEntry();
            if (!E::Groups().Has(kFoodGroup, entry))
            {
                E::SendErr(player, msg.seq, efenc::kTaming, efenc::kErrGroup);
                return;
            }
            uint32 count = std::min(put->count, item->GetCount());
            if (T::CanPut(e->s, put->slot, entry, count, item->GetTemplate()->GetMaxStackSize()) != T::PutError::None)
            {
                E::SendErr(player, msg.seq, efenc::kTaming, efenc::kErrBounds, "SLOT");
                return;
            }
            uint32 taken = count;
            player->DestroyItemCount(item, taken, true);
            T::Put(e->s, put->slot, entry, count);
            SendSlots(player, *e);
        }

        void HandleTake(Player* player, efenc::Message const& msg)
        {
            std::optional<efenc::Take> take = efenc::DecodeTake(msg);
            if (!take)
            {
                E::SendErr(player, msg.seq, efenc::kTaming, efenc::kErrDecode, "TAKE");
                return;
            }
            std::lock_guard<std::recursive_mutex> guard(s_lock);
            Creature* beast = nullptr;
            Entry* e = Frame(player, msg, take->id, beast);
            if (!e)
                return;
            if (take->slot >= T::kSlots || !e->s.slots[take->slot].item)
            {
                E::SendErr(player, msg.seq, efenc::kTaming, efenc::kErrBounds);
                return;
            }
            T::Slot const s = e->s.slots[take->slot];
            if (!player->AddItem(s.item, s.count))
            {
                E::SendErr(player, msg.seq, efenc::kTaming, efenc::kErrBounds, "FULL");
                return;
            }
            T::Take(e->s, take->slot);
            SendSlots(player, *e);
        }
    };
    TamingKind s_kind;

    // ---- the scripts ------------------------------------------------------------------------------------------------

    class emberfall_taming_world : public WorldScript
    {
    public:
        emberfall_taming_world()
            : WorldScript("emberfall_taming_world", { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_LOAD_CUSTOM_DATABASE_TABLE,
                WORLDHOOK_ON_UPDATE }) { }

        void OnAfterConfigLoad(bool /*reload*/) override
        {
            s_enable = sConfigMgr->GetOption<bool>("Emberfall.Taming.Enable", true, false);
        }

        void OnLoadCustomDatabaseTable() override { Load(); }

        // The sweep: states no creature ticked for 30 s (its grid unloaded, it despawned) are dropped.
        void OnUpdate(uint32 diff) override
        {
            m_sweep += diff;
            if (m_sweep < 10000 || !s_count.load())
                return;
            m_sweep = 0;
            uint64 const now = NowMs();
            std::lock_guard<std::recursive_mutex> guard(s_lock);
            for (auto it = s_states.begin(); it != s_states.end();)
                it = now > it->second.lastTickMs + kStaleMs ? s_states.erase(it) : std::next(it);
            CountStates();
        }

    private:
        uint32 m_sweep = 0;
    };

    class emberfall_taming_player : public PlayerScript
    {
    public:
        emberfall_taming_player()
            : PlayerScript("emberfall_taming_player", { PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_LEVEL_CHANGED, PLAYERHOOK_ON_UPDATE_SKILL,
                PLAYERHOOK_ON_UPDATE, PLAYERHOOK_ON_PLAYER_JUST_DIED, PLAYERHOOK_ON_LOGOUT, PLAYERHOOK_ON_DELETE_FROM_DB }) { }

        void OnPlayerLogin(Player* player) override
        {
            Reconcile(player);
            if (s_enable && player->getClass() == CLASS_HUNTER)
                LoadKnown(player);
        }

        void OnPlayerLogout(Player* player) override
        {
            std::lock_guard<std::recursive_mutex> guard(s_lock);
            s_known.erase(player->GetGUID().GetCounter());
        }

        void OnPlayerDeleteFromDB(CharacterDatabaseTransaction trans, uint32 guid) override
        {
            trans->Append("DELETE FROM `ef_character_tame_known` WHERE `guid` = {}", guid);
        }
        void OnPlayerLevelChanged(Player* player, uint8 /*oldLevel*/) override { Reconcile(player); }

        void OnPlayerUpdateSkill(Player* player, uint32 skillId, uint32 /*value*/, uint32 /*max*/, uint32 /*step*/, uint32 /*newValue*/) override
        {
            if (skillId == kSkillBeastcraft)
                Reconcile(player);
        }

        void OnPlayerUpdate(Player* player, uint32 /*diff*/) override
        {
            if (s_count.load())
                TryTame(player);
        }

        // The hunter dies while Drowsing: the beast's Drowse is dropped (§8); asleep, the claim holds.
        void OnPlayerJustDied(Player* player) override
        {
            if (!s_count.load())
                return;
            uint64 const me = player->GetGUID().GetRawValue();
            std::lock_guard<std::recursive_mutex> guard(s_lock);
            for (auto it = s_states.begin(); it != s_states.end();)
            {
                if (it->second.s.phase == T::Phase::Drowsing && T::Claimant(it->second.s) == me)
                {
                    efenc::Bars b{0, it->second.s.beast, W::kOver, 0, W::kMine, {}};
                    E::Send(player, efenc::EncodeBars(E::NextSeq(player), efenc::kTaming, b));
                    it = s_states.erase(it);
                }
                else
                    ++it;
            }
            CountStates();
        }
    };

    class emberfall_taming_creature : public AllCreatureScript
    {
    public:
        emberfall_taming_creature() : AllCreatureScript("emberfall_taming_creature") { }

        void OnAllCreatureUpdate(Creature* creature, uint32 /*diff*/) override { Tick(creature); }
    };

    class emberfall_taming_unit : public UnitScript
    {
    public:
        emberfall_taming_unit() : UnitScript("emberfall_taming_unit", true, { UNITHOOK_ON_UNIT_DEATH }) { }

        void OnUnitDeath(Unit* unit, Unit* /*killer*/) override
        {
            if (!s_count.load() || !unit->IsCreature())
                return;
            Creature* beast = unit->ToCreature();
            std::lock_guard<std::recursive_mutex> guard(s_lock);
            auto it = s_states.find(beast->GetGUID().GetRawValue());
            if (it == s_states.end())
                return;
            uint64 const now = NowMs();
            if (T::Asleep(it->second.s))
            {
                Player* claimant = ObjectAccessor::GetPlayer(*beast, ObjectGuid(it->second.s.claimant));
                SendResult(claimant, it->second, W::kDied);
                CloseFrame(claimant, it->second, nullptr);
            }
            SendBars(beast, it->second, now, true);
            Forget(it->first);
        }
    };

    class emberfall_taming_commands : public CommandScript
    {
    public:
        emberfall_taming_commands() : CommandScript("emberfall_taming_commands") { }

        ChatCommandTable GetCommands() const override
        {
            static ChatCommandTable tameTable =
            {
                { "state",  HandleState,  SEC_GAMEMASTER, Console::No },
                { "force",  HandleForce,  SEC_GAMEMASTER, Console::No },
                { "reload", HandleReload, SEC_GAMEMASTER, Console::Yes },
            };
            static ChatCommandTable commandTable =
            {
                { "eftame", tameTable },
            };
            return commandTable;
        }

        static Creature* Target(ChatHandler* handler)
        {
            Creature* c = handler->getSelectedCreature();
            if (!c)
                handler->SendSysMessage("EFTAME target a beast");
            return c;
        }

        static bool HandleState(ChatHandler* handler)
        {
            Creature* beast = Target(handler);
            if (!beast)
                return true;
            std::lock_guard<std::recursive_mutex> guard(s_lock);
            auto it = s_states.find(beast->GetGUID().GetRawValue());
            if (it == s_states.end())
            {
                handler->PSendSysMessage("EFTAME {} ({}): no state ({} on the realm)", beast->GetName(), beast->GetEntry(), s_states.size());
                return true;
            }
            T::State const& s = it->second.s;
            uint64 const now = NowMs();
            handler->PSendSysMessage("EFTAME {} ({}) family {} level {} rank {} phase {} for {} s", beast->GetName(), beast->GetEntry(), s.family,
                                     s.level, uint32(s.rank), uint32(s.phase), (now - s.startMs) / 1000);
            handler->PSendSysMessage("EFTAME drowse {}/{} ({} arrows, claim {}) sleep {} ms/{} trust {}/{} eaten {} frame {}", s.drowse / 1000,
                                     s.drowseMax / 1000, s.arrows, ObjectGuid(T::Asleep(s) ? s.claimant : T::Claimant(s)).ToString(),
                                     T::SleepLeft(s, now), s.sleepLenMs, s.trust / 1000, s.trustMax / 1000, s.eaten, it->second.openFor);
            for (std::size_t i = 0; i < T::kSlots; ++i)
                if (s.slots[i].item)
                    handler->PSendSysMessage("EFTAME slot {}: {} x{}", i + 1, s.slots[i].item, s.slots[i].count);
            return true;
        }

        // Drowsing: Drowse full (Out Cold); asleep: Trust full (the tame).
        static bool HandleForce(ChatHandler* handler)
        {
            Creature* beast = Target(handler);
            if (!beast)
                return true;
            std::lock_guard<std::recursive_mutex> guard(s_lock);
            auto it = s_states.find(beast->GetGUID().GetRawValue());
            if (it == s_states.end())
            {
                handler->SendSysMessage("EFTAME no state: hit it with a Drowse Shot first");
                return true;
            }
            Entry& e = it->second;
            uint64 const now = NowMs();
            if (e.s.phase == T::Phase::Drowsing)
            {
                e.s.drowse = e.s.drowseMax;
                LieDown(beast, e, now);
                SendBars(beast, e, now);
                handler->SendSysMessage("EFTAME out cold");
            }
            else
            {
                e.s.trust = e.s.trustMax;
                e.tameDue = true;
                handler->SendSysMessage("EFTAME trust full");
            }
            return true;
        }

        static bool HandleReload(ChatHandler* handler)
        {
            Load();
            handler->SendSysMessage("EFTAME reloaded");
            return true;
        }
    };
}

// Drowse Shot (99524): a drowse arrow at a tameable beast (hunter-taming.md §4.1).
class spell_ef_drowse_shot : public SpellScript
{
    PrepareSpellScript(spell_ef_drowse_shot);

    std::optional<T::Arrow> Arrow(Player* player, Creature* beast)
    {
        std::lock_guard<std::recursive_mutex> guard(s_lock);
        return T::PickArrow(s_arrows, beast->GetLevel(), [player](uint32 item) { return player->HasItemCount(item, 1); });
    }

    SpellCastResult Check()
    {
        Player* player = GetCaster() ? GetCaster()->ToPlayer() : nullptr;
        if (!player)
            return SPELL_FAILED_DONT_REPORT;
        if (!s_enable)
            return SPELL_FAILED_NOT_READY;
        Creature* beast = GetExplTargetUnit() ? GetExplTargetUnit()->ToCreature() : nullptr;
        if (SpellCastResult r = TameRefusal(player, beast); r != SPELL_CAST_OK)
            return r;
        if (beast->HasAura(kSpellOutCold))
            return SPELL_FAILED_BAD_TARGETS;
        // someone else's quarry (§7): tagged by a player outside your group
        if (beast->hasLootRecipient() && !beast->isTappedBy(player))
        {
            E::SendErr(player, 0, efenc::kTaming, efenc::kErrClaim, "TAPPED");
            return SPELL_FAILED_DONT_REPORT;
        }
        if (!Arrow(player, beast))
        {
            E::SendErr(player, 0, efenc::kTaming, efenc::kErrBounds, "ARROWS");
            return SPELL_FAILED_DONT_REPORT;
        }
        return SPELL_CAST_OK;
    }

    void HandleHit(SpellEffIndex /*effIndex*/)
    {
        Player* player = GetCaster() ? GetCaster()->ToPlayer() : nullptr;
        Creature* beast = GetHitUnit() ? GetHitUnit()->ToCreature() : nullptr;
        if (!player || !beast || !beast->IsAlive() || beast->HasAura(kSpellOutCold))
            return;
        std::optional<T::Arrow> arrow = Arrow(player, beast);
        if (!arrow)
            return;
        player->DestroyItemCount(arrow->item, 1, true);
        beast->UpdateLeashExtensionTime();
        if (!beast->hasLootRecipient())
            beast->SetLootRecipient(player);   // it tags the beast as any first hit does
        uint64 const now = NowMs();
        std::lock_guard<std::recursive_mutex> guard(s_lock);
        uint64 const key = beast->GetGUID().GetRawValue();
        auto it = s_states.find(key);
        if (it == s_states.end())
        {
            CreatureTemplate const* t = beast->GetCreatureTemplate();
            Entry e;
            e.s = T::Begin(s_rule, key, FamilyOf(beast), beast->GetLevel(), T::RankOf(t->rank), now);
            e.lastTickMs = now;
            it = s_states.emplace(key, e).first;
            CountStates();
        }
        Entry& e = it->second;
        if (e.s.phase != T::Phase::Drowsing)
            return;
        // another crowd control holds it: the arrow lands, the bar does not rise (§7)
        bool const held = beast->HasUnitState(UNIT_STATE_STUNNED | UNIT_STATE_CONFUSED | UNIT_STATE_FLEEING | UNIT_STATE_ROOT);
        uint32 const amount = held ? 0 : T::ArrowDrowse(s_rule, *arrow, e.s.level);
        if (T::AddDrowse(e.s, player->GetGUID().GetRawValue(), amount, now))
            LieDown(beast, e, now);
        SendBars(beast, e, now);
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_ef_drowse_shot::Check);
        OnEffectHitTarget += SpellEffectFn(spell_ef_drowse_shot::HandleHit, EFFECT_0, SPELL_EFFECT_DUMMY);
    }
};

// Out Cold (99525): the beast's sleep. The module wakes it at its time; this only stands a beast up whose state the
// module no longer has (a forgotten state, a GM's removal), so nothing lies about forever.
class spell_ef_out_cold : public AuraScript
{
    PrepareAuraScript(spell_ef_out_cold);

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* target = GetTarget();
        if (!target)
            return;
        {
            std::lock_guard<std::recursive_mutex> guard(s_lock);
            if (s_states.count(target->GetGUID().GetRawValue()))
                return;   // the module's tick wakes it
        }
        target->SetStandState(UNIT_STAND_STATE_STAND);
    }

    void Register() override
    {
        AfterEffectRemove += AuraEffectRemoveFn(spell_ef_out_cold::HandleRemove, EFFECT_0, SPELL_AURA_ANY, AURA_EFFECT_HANDLE_REAL);
    }
};

void AddEmberfallTamingScripts()
{
    Emberfall::Encounters::RegisterKind(efenc::kTaming, &s_kind);
    new emberfall_taming_world();
    new emberfall_taming_player();
    new emberfall_taming_creature();
    new emberfall_taming_unit();
    new emberfall_taming_commands();
    RegisterSpellScript(spell_ef_drowse_shot);
    RegisterSpellScript(spell_ef_out_cold);
}
