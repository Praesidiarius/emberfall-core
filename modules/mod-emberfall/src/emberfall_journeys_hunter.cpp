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

// The Hunter's Class Journey, its field abilities (Emberfall docs/design/class-journeys/hunter.md §3; the kit's
// learn: journey abilities, classes/hunter.yaml):
//
//   - Beast at Heel (spell_ef_journey_heel): a stabled beast walks beside the hunter as a non-combat follower (a
//     template per companion kind, npc_ef_journey_heel, wearing the stabled beast's own look: character_pet.modelid).
//     Not attackable, no spells, passive; at the stabled beast's own size and growth stage (hunter-taming.md §18). It
//     hides while the hunter fights, has his companion out or is in a dungeon or battleground, and comes back after; a map change brings it along; cast again, it goes back. The next cast takes
//     the next stall (EfJourneyLogic.h NextStall).
//   - Call from the Stable (spell_ef_journey_call, also the step's quest items: call_lesson; every hunter's from 10 since
//     2026-11-25, docs/design/hunter-beasts.md §2.2): its button opens the stable list in field mode (emberfall_beasts.cpp,
//     EFB); the list's Call casts it with the picked beast (3 s), then that beast (the whistle: the next stabled one)
//     takes the companion's place (the stable swap of a stable keeper, NPCHandler.cpp HandleStableSwapPet's, with no
//     keeper; beasts/EfBeasts.h SwapWith); with no companion out it walks at heel instead (no fighting companion for the
//     Ranger and the Spirit Caller). Not in dungeons. Quick Call (step 4) shortens its cooldown (a spell modifier).
//   - Stable at Hand (spell_ef_journey_stable, also stable_lesson): the open-stable aura (SPELL_AURA_OPEN_STABLE) the
//     core sends the stable list for, and whose holder may stable, unstable and swap with his own guid as the keeper
//     (PetHandler.cpp CheckStableMaster); refused in dungeons.

#include "CreatureAI.h"
#include "CreatureScript.h"
#include "DatabaseEnv.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Pet.h"
#include "Player.h"
#include "PlayerScript.h"
#include "ScriptMgr.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"
#include "TemporarySummon.h"
#include "beasts/EfBeasts.h"
#include "journeys/EfJourneyLogic.h"
#include "petsize/EfPetSize.h"

#include <cmath>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace
{
    namespace J = Emberfall::Journeys;

    // the beasts at heel, by the companion's CreatureFamily (content/data/journeys/creatures.yaml 90890-90894)
    uint32 HeelEntryOf(uint32 family)
    {
        switch (family)
        {
            case 2: return 90891;     // lynx
            case 5: return 90892;     // boar
            case 7: return 90893;     // hawk
            case 11: return 90894;    // lizard
            default: return 90890;    // hound (and any other)
        }
    }

    constexpr uint32 kHeelCheckMs = 500;
    constexpr float kHeelDist = 2.5f;
    constexpr float kHeelAngle = float(M_PI) * 0.75f;

    struct Heel
    {
        uint32 petNumber = 0;      // the stabled beast at heel (0: none)
        ObjectGuid summon;
        int lastStall = -1;        // the stall the last call or heel took
    };

    std::mutex s_lock;
    std::unordered_map<uint32, Heel> s_heel;   // by the hunter's GUID counter

    bool NoBeastsHere(Player* player)
    {
        Map* map = player->GetMap();
        return !map || map->IsDungeon() || map->IsBattlegroundOrArena();
    }

    std::vector<bool> StallsUsed(PetStable const* stable)
    {
        std::vector<bool> used;
        if (!stable)
            return used;
        for (uint32 i = 0; i < stable->MaxStabledPets && i < MAX_PET_STABLES; ++i)
            used.push_back(bool(stable->StabledPets[i]));
        return used;
    }

    int NextStall(Player* player)
    {
        std::vector<bool> const used = StallsUsed(player->GetPetStable());
        std::lock_guard<std::mutex> guard(s_lock);
        Heel& h = s_heel[player->GetGUID().GetCounter()];
        int const next = J::NextStall(used, h.lastStall);
        if (next >= 0)
            h.lastStall = next;
        return next;
    }

    Creature* HeelCreature(Player* player)
    {
        ObjectGuid g;
        {
            std::lock_guard<std::mutex> guard(s_lock);
            auto it = s_heel.find(player->GetGUID().GetCounter());
            if (it == s_heel.end())
                return nullptr;
            g = it->second.summon;
        }
        return g ? ObjectAccessor::GetCreature(*player, g) : nullptr;
    }

    void Dismiss(Player* player, bool forget)
    {
        if (Creature* c = HeelCreature(player))
            c->DespawnOrUnsummon();
        std::lock_guard<std::mutex> guard(s_lock);
        auto it = s_heel.find(player->GetGUID().GetCounter());
        if (it == s_heel.end())
            return;
        it->second.summon.Clear();
        if (forget)
            it->second.petNumber = 0;
    }

    // The stabled beast of this pet number at heel (its own look), replacing one there.
    bool WalkAtHeel(Player* player, uint32 petNumber)
    {
        PetStable const* stable = player->GetPetStable();
        if (!stable)
            return false;
        PetStable::PetInfo const* info = nullptr;
        for (auto const& p : stable->StabledPets)
            if (p && p->PetNumber == petNumber)
                info = &*p;
        if (!info)
            return false;
        CreatureTemplate const* t = sObjectMgr->GetCreatureTemplate(info->CreatureId);
        uint32 const entry = HeelEntryOf(t ? t->family : 0);
        uint32 const display = info->DisplayId;
        Dismiss(player, false);
        Position pos = player->GetNearPosition(kHeelDist, kHeelAngle);
        TempSummon* summon = player->SummonCreature(entry, pos, TEMPSUMMON_MANUAL_DESPAWN);
        if (!summon)
            return false;
        summon->SetFaction(player->GetFaction());
        summon->SetLevel(player->GetLevel());
        // at its own size and stage, as it stands as the companion (hunter-taming.md §18)
        if (float scale = 1.0f; display && Emberfall::PetSize::ScaleForPet(t ? t->family : 0, display, petNumber, scale))
            summon->SetDisplayId(display, scale);
        else if (display)
            summon->SetDisplayId(display);
        summon->SetReactState(REACT_PASSIVE);
        summon->GetMotionMaster()->MoveFollow(player, kHeelDist, kHeelAngle);
        std::lock_guard<std::mutex> guard(s_lock);
        Heel& h = s_heel[player->GetGUID().GetCounter()];
        h.petNumber = petNumber;
        h.summon = summon->GetGUID();
        return true;
    }

    uint32 PetNumberAt(Player* player, int stall)
    {
        PetStable const* stable = player->GetPetStable();
        if (!stable || stall < 0 || stall >= int(MAX_PET_STABLES) || !stable->StabledPets[stall])
            return 0;
        return stable->StabledPets[stall]->PetNumber;
    }

    // The stable keeper's swap without a keeper: the shared helper (beasts/EfBeasts.h).
    bool SwapIn(Player* player, int stall) { return Emberfall::Beasts::SwapWith(player, stall); }

    SpellCastResult FieldCheck(Player* player, bool needStabled)
    {
        if (!player)
            return SPELL_FAILED_DONT_REPORT;
        if (NoBeastsHere(player))
            return SPELL_FAILED_NOT_HERE;
        if (needStabled && J::NextStall(StallsUsed(player->GetPetStable()), -1) < 0)
            return SPELL_FAILED_NO_PET;
        return SPELL_CAST_OK;
    }
}

// The beast at heel: follows its hunter, never fights, hides while he fights or has his companion out.
class npc_ef_journey_heel : public CreatureScript
{
public:
    npc_ef_journey_heel() : CreatureScript("npc_ef_journey_heel") { }

    struct HeelAI : public CreatureAI
    {
        explicit HeelAI(Creature* c) : CreatureAI(c) { }

        void MoveInLineOfSight(Unit* /*who*/) override { }
        void AttackStart(Unit* /*who*/) override { }
        void EnterEvadeMode(EvadeReason /*why*/) override { }

        void UpdateAI(uint32 diff) override
        {
            _check += diff;
            if (_check < kHeelCheckMs)
                return;
            _check = 0;
            TempSummon* self = me->ToTempSummon();
            Player* owner = self ? ObjectAccessor::GetPlayer(*me, self->GetSummonerGUID()) : nullptr;
            if (!owner || owner->GetMap() != me->GetMap())
            {
                me->DespawnOrUnsummon();
                return;
            }
            bool const hide = owner->IsInCombat() || owner->GetPet() || NoBeastsHere(owner) || !owner->IsAlive();
            if (hide != _hidden)
            {
                _hidden = hide;
                me->SetVisible(!hide);
                if (!hide)
                {
                    Position pos = owner->GetNearPosition(kHeelDist, kHeelAngle);
                    me->NearTeleportTo(pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ(), owner->GetOrientation());
                    me->GetMotionMaster()->MoveFollow(owner, kHeelDist, kHeelAngle);
                }
            }
            // the follow keeps the hunter's walk and speed (MoveFollow's inheritWalkState, inheritSpeed)
            if (!_hidden && me->GetMotionMaster()->GetCurrentMovementGeneratorType() != FOLLOW_MOTION_TYPE)
                me->GetMotionMaster()->MoveFollow(owner, kHeelDist, kHeelAngle);
        }

    private:
        uint32 _check = 0;
        bool _hidden = false;
    };

    CreatureAI* GetAI(Creature* creature) const override { return new HeelAI(creature); }
};

// Beast at Heel (103801): the next stabled beast walks at heel; again, it goes back.
class spell_ef_journey_heel : public SpellScript
{
    PrepareSpellScript(spell_ef_journey_heel);

    SpellCastResult Check()
    {
        Player* player = GetCaster() ? GetCaster()->ToPlayer() : nullptr;
        if (player && HeelCreature(player))
            return SPELL_CAST_OK;   // sending it back
        if (SpellCastResult r = FieldCheck(player, true); r != SPELL_CAST_OK)
            return r;
        if (player->GetPet())
            return SPELL_FAILED_ALREADY_HAVE_SUMMON;
        return SPELL_CAST_OK;
    }

    void HandleHit(SpellEffIndex /*effIndex*/)
    {
        Player* player = GetCaster() ? GetCaster()->ToPlayer() : nullptr;
        if (!player)
            return;
        if (HeelCreature(player))
        {
            Dismiss(player, true);
            return;
        }
        int const stall = NextStall(player);
        if (stall >= 0)
            WalkAtHeel(player, PetNumberAt(player, stall));
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_ef_journey_heel::Check);
        OnEffectHitTarget += SpellEffectFn(spell_ef_journey_heel::HandleHit, EFFECT_0, SPELL_EFFECT_DUMMY);
    }
};

// Call from the Stable (103803) and its quest item's lesson (103807): the next stabled beast takes the companion's
// place, or walks at heel with no companion out.
class spell_ef_journey_call : public SpellScript
{
    PrepareSpellScript(spell_ef_journey_call);

    SpellCastResult Check()
    {
        Player* player = GetCaster() ? GetCaster()->ToPlayer() : nullptr;
        SpellCastResult const r = FieldCheck(player, true);
        if (r != SPELL_CAST_OK)
            return r;
        // the button (not the quest's whistle) opens the stable list first; its Call casts it with the pick
        // (hunter-beasts.md §2.2)
        if (GetSpellInfo()->Id == Emberfall::Beasts::kSpellCallFromTheStable && !Emberfall::Beasts::HasCallPick(player))
        {
            Emberfall::Beasts::OpenFieldList(player);
            return SPELL_FAILED_DONT_REPORT;
        }
        return SPELL_CAST_OK;
    }

    void HandleHit(SpellEffIndex /*effIndex*/)
    {
        Player* player = GetCaster() ? GetCaster()->ToPlayer() : nullptr;
        if (!player)
            return;
        // the beast picked in the field list (EFB CALL, hunter-beasts.md §2.2), else the next stall's
        int stall = Emberfall::Beasts::TakeCallPick(player);
        if (stall < 0)
            stall = NextStall(player);
        if (stall < 0)
            return;
        if (Pet* pet = player->GetPet(); pet && pet->IsHunterPet() && pet->IsAlive())
        {
            Dismiss(player, true);
            SwapIn(player, stall);
            return;
        }
        WalkAtHeel(player, PetNumberAt(player, stall));
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_ef_journey_call::Check);
        OnEffectHitTarget += SpellEffectFn(spell_ef_journey_call::HandleHit, EFFECT_0, SPELL_EFFECT_DUMMY);
    }
};

// Stable at Hand (103806) and its quest item's lesson (103808): the open-stable aura, refused in dungeons.
class spell_ef_journey_stable : public SpellScript
{
    PrepareSpellScript(spell_ef_journey_stable);

    SpellCastResult Check() { return FieldCheck(GetCaster() ? GetCaster()->ToPlayer() : nullptr, false); }

    void Register() override { OnCheckCast += SpellCheckCastFn(spell_ef_journey_stable::Check); }
};

// The beast at heel follows its hunter to another map and goes with his logout.
class emberfall_journeys_hunter_player : public PlayerScript
{
public:
    emberfall_journeys_hunter_player()
        : PlayerScript("emberfall_journeys_hunter_player", { PLAYERHOOK_ON_MAP_CHANGED, PLAYERHOOK_ON_LOGOUT }) { }

    void OnPlayerMapChanged(Player* player) override
    {
        uint32 pet = 0;
        {
            std::lock_guard<std::mutex> guard(s_lock);
            auto it = s_heel.find(player->GetGUID().GetCounter());
            if (it == s_heel.end() || !it->second.petNumber)
                return;
            pet = it->second.petNumber;
        }
        if (!HeelCreature(player))
            WalkAtHeel(player, pet);
    }

    void OnPlayerLogout(Player* player) override
    {
        Dismiss(player, true);
        std::lock_guard<std::mutex> guard(s_lock);
        s_heel.erase(player->GetGUID().GetCounter());
    }
};

void AddEmberfallJourneysHunterScripts()
{
    new npc_ef_journey_heel();
    new emberfall_journeys_hunter_player();
    RegisterSpellScript(spell_ef_journey_heel);
    RegisterSpellScript(spell_ef_journey_call);
    RegisterSpellScript(spell_ef_journey_stable);
}
