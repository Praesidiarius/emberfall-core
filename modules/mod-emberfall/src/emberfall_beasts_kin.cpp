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

// The Hunter's beasts, part 2 (Emberfall docs/design/hunter-beasts.md §4-§5; `Emberfall.Beasts.Enable`, default 1): the
// archetypes' passives on the hunter and the beast families' special attacks, from the world's ef_pet_archetype and
// ef_pet_family_spell (content/data/pets.yaml archetypes, taming/families.yaml; pet_kin.py) and a companion's beast
// family by its creature (ef_tame_beast, the taming's rows).
//
//   - Kinship (spell_ef_pet_kin, every companion's passive from its kind's skill line): once a second the companion
//     knows its family's special and no other family's (they persist in pet_spell, so a family that changes takes its
//     old one away), its hunter has its archetype's passive and no other archetype's, and the rule's helpers hold
//     (Packrunner: the pace within range; Heavyhide: the companion's hold; Skywing: the shots' reach); the client hears
//     the family (EFB KIN). When Kinship goes (the companion is put away, dismissed, dies away) the passives go. The
//     passive's effect values are its numbers (spells/pet_archetypes.yaml): this file only reads them.
//   - The blows (UnitScript): the hunter's damage on the companion's target (Packrunner), on an unhurt enemy (Prowler),
//     on an enemy with the companion's venom (Venomkin); the damage the hunter takes near the companion (Heavyhide); the
//     companion's swings' crits early in a fight (Prowler); the companion's blows leave the venom (Venomkin) and mark
//     their enemy for the hunter (Skywing); the hunter's ward under the threshold, once a minute (Shellback); a
//     special's rule (more on a busy, slowed or wet enemy, through the armour). The rules that hold a special back (a
//     caster only, never a boss) are the priority's (SpecialAllowedNow, emberfall_beasts.cpp).
//   - `.efpet make <family>` (the spec bench's companion, emberfall_petsize.cpp's command table): the hunter's companion
//     out or waiting goes for good and a new one of that beast family comes out at his level; `.efpet kin`.

#include "Chat.h"
#include "Config.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "EmberfallOwnData.h"
#include "GameTime.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Pet.h"
#include "Player.h"
#include "PlayerScript.h"
#include "ScriptMgr.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"
#include "UnitScript.h"
#include "WorldPacket.h"
#include "WorldScript.h"
#include "WorldSession.h"
#include "beasts/EfBeasts.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace B = Emberfall::Beasts;

namespace
{
    bool s_enable = true;
    // read-only after the load (the world's tables at startup)
    std::unordered_map<uint32, uint32> s_beastFamily;         // creature entry -> beast family (ef_tame_beast)
    std::unordered_map<uint32, B::FamilyKin> s_family;        // beast family -> its archetype and special
    std::map<std::string, uint32> s_familyByName;             // the family's key -> its id (.efpet make)
    std::unordered_map<uint8, B::ArchetypeSpells> s_archetype;  // by Kind
    std::vector<uint32> s_specials;                           // every family's special
    std::vector<uint32> s_passives;                           // every archetype's passive and the hunter's helpers

    std::mutex s_lock;
    std::unordered_map<uint32, uint64> s_lastWard;            // Shellback: the hunter's last ward (game ms), by GUID counter

    uint64 NowMs() { return uint64(GameTime::GetGameTimeMS().count()); }

    void Load()
    {
        s_beastFamily.clear();
        s_family.clear();
        s_familyByName.clear();
        s_archetype.clear();
        s_specials.clear();
        s_passives.clear();
        if (QueryResult r = WorldDatabase.Query("SELECT `entry`, `family` FROM `ef_tame_beast`"))
            do
            {
                s_beastFamily[r->Fetch()[0].Get<uint32>()] = r->Fetch()[1].Get<uint32>();
            } while (r->NextRow());
        if (QueryResult r = WorldDatabase.Query("SELECT `id`, `passive`, `helper1`, `helper2` FROM `ef_pet_archetype`"))
            do
            {
                Field* f = r->Fetch();
                B::Kind const kind = B::KindOf(f[0].Get<uint8>());
                if (kind == B::Kind::None)
                    continue;
                s_archetype[uint8(kind)] = {f[1].Get<uint32>(), f[2].Get<uint32>(), f[3].Get<uint32>()};
            } while (r->NextRow());
        if (QueryResult r = WorldDatabase.Query("SELECT `family`, `archetype`, `special`, `rule`, `rule_value`, `name` FROM `ef_pet_family_spell`"))
            do
            {
                Field* f = r->Fetch();
                B::FamilyKin k;
                k.archetype = B::KindOf(f[1].Get<uint8>());
                k.special = f[2].Get<uint32>();
                k.rule = B::RuleOf(f[3].Get<uint8>());
                k.ruleValue = f[4].Get<uint32>();
                s_family[f[0].Get<uint32>()] = k;
                s_familyByName[f[5].Get<std::string>()] = f[0].Get<uint32>();
                if (k.special)
                    s_specials.push_back(k.special);
            } while (r->NextRow());
        for (auto const& [kind, a] : s_archetype)
        {
            s_passives.push_back(a.passive);
            B::Kind const k = B::Kind(kind);
            // the hunter's own helpers (the companion's hold, the venom, Marked and the ward are not on the hunter)
            if (k == B::Kind::Packrunner && a.helper1)
                s_passives.push_back(a.helper1);
            if (k == B::Kind::Skywing && a.helper2)
                s_passives.push_back(a.helper2);
        }
        LOG_INFO("server.loading", ">> Emberfall beasts: {} archetypes, {} families with a special, {} beasts", s_archetype.size(),
                 s_family.size(), s_beastFamily.size());
    }

    bool Ready() { return s_enable && !s_family.empty() && Emberfall::OwnData(); }

    void Send(Player* player, std::string const& body)
    {
        std::string const text = std::string(B::kPrefix) + body;
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player, player, text);
        player->SendDirectMessage(&data);
    }

    uint32 FamilyOfEntry(uint32 entry)
    {
        auto it = s_beastFamily.find(entry);
        return it == s_beastFamily.end() ? 0 : it->second;
    }

    B::FamilyKin const* KinOfPet(Pet const* pet)
    {
        if (!pet || pet->getPetType() != HUNTER_PET)
            return nullptr;
        auto it = s_family.find(FamilyOfEntry(pet->GetEntry()));
        return it == s_family.end() ? nullptr : &it->second;
    }

    // The hunter's companion out and its kin (nothing for anyone else).
    Pet* CompanionOf(Unit const* unit)
    {
        if (!unit || !unit->IsPlayer())
            return nullptr;
        Player const* player = unit->ToPlayer();
        if (player->getClass() != CLASS_HUNTER)
            return nullptr;
        Pet* pet = player->GetPet();
        return pet && pet->IsInWorld() && pet->getPetType() == HUNTER_PET ? pet : nullptr;
    }

    // A unit that is a hunter's companion, and its hunter.
    Player* HunterOfPet(Unit const* unit, Pet const*& pet)
    {
        pet = nullptr;
        if (!unit || !unit->IsPet())
            return nullptr;
        Pet const* p = static_cast<Pet const*>(unit);
        if (p->getPetType() != HUNTER_PET)
            return nullptr;
        Player* owner = p->GetOwner();
        if (!owner || owner->getClass() != CLASS_HUNTER)
            return nullptr;
        pet = p;
        return owner;
    }

    // The archetype's passive on the hunter, its effect values (0 when not there).
    struct Passive
    {
        B::Kind kind = B::Kind::None;
        int32 v[3] = {0, 0, 0};
        B::ArchetypeSpells const* spells = nullptr;
    };

    Passive PassiveOf(Unit const* hunter, Pet const* pet)
    {
        Passive p;
        B::FamilyKin const* kin = KinOfPet(pet);
        if (!kin || !hunter)
            return p;
        auto a = s_archetype.find(uint8(kin->archetype));
        if (a == s_archetype.end())
            return p;
        Aura const* aura = hunter->GetAura(a->second.passive);
        if (!aura)
            return p;
        p.kind = kin->archetype;
        p.spells = &a->second;
        for (uint8 i = 0; i < 3; ++i)
            if (AuraEffect const* e = aura->GetEffect(i))
                p.v[i] = e->GetAmount();
        return p;
    }

    // Every archetype's passive and hunter helper off the hunter, but those of `keep` (its archetype's: they stay on, so
    // the pace and the reach do not flicker each second).
    void StripHunter(Unit* hunter, B::ArchetypeSpells const* keep = nullptr)
    {
        if (!hunter)
            return;
        for (uint32 spell : s_passives)
            if (!keep || (spell != keep->passive && spell != keep->helper1 && spell != keep->helper2))
                hunter->RemoveAurasDueToSpell(spell);
    }

    // ---- Kinship: the companion's once-a-second check -----------------------------------------------------------

    void Tick(Pet* pet, uint32& sentFamily)
    {
        Player* hunter = pet->GetOwner();
        if (!hunter || !Ready() || pet->getPetType() != HUNTER_PET)
            return;
        uint32 const family = FamilyOfEntry(pet->GetEntry());
        if (family != sentFamily)
        {
            sentFamily = family;
            Send(hunter, B::Kin(family));
        }
        auto it = s_family.find(family);
        if (it == s_family.end())
        {
            StripHunter(hunter);
            return;
        }
        B::FamilyKin const& kin = it->second;
        // the special: its own, no other family's
        if (kin.special && !pet->HasSpell(kin.special) && sSpellMgr->GetSpellInfo(kin.special))
            pet->learnSpell(kin.special);
        for (uint32 old : B::SpecialsToForget(s_specials, kin.special, [pet](uint32 s) { return pet->HasSpell(s); }))
            pet->unlearnSpell(old, false);
        // the passive: its archetype's, no other
        auto a = s_archetype.find(uint8(kin.archetype));
        if (a == s_archetype.end())
        {
            StripHunter(hunter);
            return;
        }
        B::ArchetypeSpells const& spells = a->second;
        StripHunter(hunter, &spells);
        if (!hunter->HasAura(spells.passive) && hunter->IsAlive())
            hunter->CastSpell(hunter, spells.passive, true);
        // a companion whose family moved to another archetype keeps no Heavyhide hold
        if (kin.archetype != B::Kind::Heavyhide)
        {
            auto hh = s_archetype.find(uint8(B::Kind::Heavyhide));
            if (hh != s_archetype.end() && hh->second.helper1)
                pet->RemoveAurasDueToSpell(hh->second.helper1);
        }
        Passive const p = PassiveOf(hunter, pet);
        float const yards = pet->GetExactDist(hunter);
        switch (kin.archetype)
        {
            case B::Kind::Packrunner:
                if (spells.helper1)
                {
                    bool const holds = B::PaceHolds(p.kind, p.v[1], yards);
                    if (holds && !hunter->HasAura(spells.helper1))
                        hunter->CastSpell(hunter, spells.helper1, true);
                    else if (!holds)
                        hunter->RemoveAurasDueToSpell(spells.helper1);
                }
                break;
            case B::Kind::Heavyhide:
                if (spells.helper1 && !pet->HasAura(spells.helper1))
                    pet->AddAura(spells.helper1, pet);
                break;
            case B::Kind::Skywing:
                if (spells.helper2 && !hunter->HasAura(spells.helper2) && hunter->IsAlive())
                    hunter->AddAura(spells.helper2, hunter);
                break;
            default:
                break;
        }
    }

    class spell_ef_pet_kin : public AuraScript
    {
        PrepareAuraScript(spell_ef_pet_kin);

        uint32 _sent = ~0u;

        void CalcPeriodic(AuraEffect const* /*aurEff*/, bool& isPeriodic, int32& amplitude)
        {
            isPeriodic = true;
            amplitude = 1 * IN_MILLISECONDS;
        }

        void HandlePeriodic(AuraEffect const* /*aurEff*/)
        {
            PreventDefaultAction();
            if (Unit* owner = GetUnitOwner())
                if (owner->IsPet())
                    Tick(static_cast<Pet*>(owner), _sent);
        }

        void Gone(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
        {
            Unit* owner = GetUnitOwner();
            if (!owner || !owner->IsPet())
                return;
            Player* hunter = static_cast<Pet*>(owner)->GetOwner();
            if (!hunter)
                return;
            StripHunter(hunter);
            if (hunter->IsInWorld())
                Send(hunter, B::Kin(0));
        }

        void Register() override
        {
            DoEffectCalcPeriodic += AuraEffectCalcPeriodicFn(spell_ef_pet_kin::CalcPeriodic, EFFECT_0, SPELL_AURA_DUMMY);
            OnEffectPeriodic += AuraEffectPeriodicFn(spell_ef_pet_kin::HandlePeriodic, EFFECT_0, SPELL_AURA_DUMMY);
            AfterEffectRemove += AuraEffectRemoveFn(spell_ef_pet_kin::Gone, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        }
    };

    // ---- the blows ----------------------------------------------------------------------------------------------

    uint32 VenomStacks(Unit const* victim, Pet const* pet, B::ArchetypeSpells const* spells)
    {
        if (!spells || !spells->helper1 || !victim || !pet)
            return 0;
        Aura const* venom = victim->GetAura(spells->helper1, pet->GetGUID());
        return venom ? venom->GetStackAmount() : 0;
    }

    // The hunter's damage factor (Packrunner, Prowler, Venomkin).
    float HunterFactor(Unit* attacker, Unit* victim, bool direct)
    {
        Pet* pet = CompanionOf(attacker);
        if (!pet || !victim || victim == pet)
            return 1.0f;
        Passive const p = PassiveOf(attacker, pet);
        if (p.kind == B::Kind::None)
            return 1.0f;
        B::HunterHit h;
        h.kind = p.kind;
        h.v1 = p.v[0];
        h.v2 = p.v[1];
        h.v3 = p.v[2];
        h.direct = direct;
        h.onCompanionTarget = pet->GetVictim() == victim;
        h.victimUnhurt = victim->IsFullHealth();
        if (p.kind == B::Kind::Venomkin)
            h.venomStacks = VenomStacks(victim, pet, p.spells);
        return B::HunterDamageFactor(h);
    }

    // The damage factor of a blow the hunter takes (Heavyhide).
    float TakenFactor(Unit* victim)
    {
        Pet* pet = CompanionOf(victim);
        if (!pet)
            return 1.0f;
        Passive const p = PassiveOf(victim, pet);
        return B::HunterTakenFactor(p.kind, p.v[0], p.v[1], pet->GetExactDist(victim));
    }

    // A companion's special and its rule's factor at the hit.
    float SpecialFactor(Unit* attacker, Unit* victim, SpellInfo const* spellInfo)
    {
        if (!spellInfo || !victim)
            return 1.0f;
        Pet const* pet = nullptr;
        if (!HunterOfPet(attacker, pet))
            return 1.0f;
        B::FamilyKin const* kin = KinOfPet(pet);
        if (!kin || kin->special != spellInfo->Id || kin->rule == B::Rule::None)
            return 1.0f;
        if (kin->rule == B::Rule::Pierce)
            return B::PierceFactor(float(victim->GetArmor()), pet->GetLevel(), kin->ruleValue);
        bool const busy = victim->GetVictim() && victim->GetVictim() != attacker;
        return B::SpecialBonus(kin->rule, kin->ruleValue, busy, victim->HasAuraType(SPELL_AURA_MOD_DECREASE_SPEED),
                               victim->IsInWater() || attacker->IsInWater());
    }

    template <class T>
    void Scale(T& damage, float factor)
    {
        if (factor != 1.0f && damage > 0)
            damage = T(std::max(0.0f, float(damage) * factor + 0.5f));
    }

    class emberfall_beasts_kin_unit : public UnitScript
    {
    public:
        emberfall_beasts_kin_unit() : UnitScript("emberfall_beasts_kin_unit", true,
            { UNITHOOK_MODIFY_MELEE_DAMAGE, UNITHOOK_MODIFY_SPELL_DAMAGE_TAKEN, UNITHOOK_MODIFY_PERIODIC_DAMAGE_AURAS_TICK,
              UNITHOOK_ON_BEFORE_ROLL_MELEE_OUTCOME_AGAINST, UNITHOOK_ON_DAMAGE }) { }

        void ModifyMeleeDamage(Unit* target, Unit* attacker, uint32& damage) override
        {
            if (!Ready() || !attacker || !target)
                return;
            Scale(damage, HunterFactor(attacker, target, true) * TakenFactor(target));
        }

        void ModifySpellDamageTaken(Unit* target, Unit* attacker, int32& damage, SpellInfo const* spellInfo) override
        {
            if (!Ready() || !attacker || !target)
                return;
            Scale(damage, HunterFactor(attacker, target, true) * TakenFactor(target) * SpecialFactor(attacker, target, spellInfo));
        }

        void ModifyPeriodicDamageAurasTick(Unit* target, Unit* attacker, uint32& damage, SpellInfo const* /*spellInfo*/) override
        {
            if (!Ready() || !attacker || !target)
                return;
            Scale(damage, HunterFactor(attacker, target, false) * TakenFactor(target));
        }

        void OnBeforeRollMeleeOutcomeAgainst(Unit const* attacker, Unit const* victim, WeaponAttackType /*attType*/, int32& /*a*/,
                                             int32& /*b*/, int32& /*c*/, int32& /*d*/, int32& crit_chance, int32& /*miss*/,
                                             int32& /*dodge*/, int32& /*parry*/, int32& /*block*/) override
        {
            if (!Ready() || !victim)
                return;
            Pet const* pet = nullptr;
            Player* hunter = HunterOfPet(attacker, pet);
            if (!hunter)
                return;
            Passive const p = PassiveOf(hunter, pet);
            crit_chance += B::CompanionCritBonus(p.kind, p.v[1], p.v[2], victim->GetHealthPct());
        }

        void OnDamage(Unit* attacker, Unit* victim, uint32& damage) override
        {
            if (!Ready() || !victim || !damage)
                return;
            // the companion's blow: the venom (Venomkin), the mark (Skywing)
            Pet const* pet = nullptr;
            if (Player* hunter = HunterOfPet(attacker, pet))
            {
                if (victim != hunter && victim->IsAlive() && damage < victim->GetHealth() && hunter->IsAlive())
                {
                    Passive const p = PassiveOf(hunter, pet);
                    if (p.kind == B::Kind::Venomkin && p.spells->helper1)
                        attacker->CastSpell(victim, p.spells->helper1, true);
                    else if (p.kind == B::Kind::Skywing && p.spells->helper1)
                        hunter->CastSpell(victim, p.spells->helper1, true);
                }
                return;
            }
            // the hunter's fall (Shellback)
            Pet* out = CompanionOf(victim);
            if (!out || !victim->IsAlive() || damage >= victim->GetHealth())
                return;
            Passive const p = PassiveOf(victim, out);
            if (p.kind != B::Kind::Shellback || !p.spells->helper1)
                return;
            float const max = float(std::max<uint32>(1, victim->GetMaxHealth()));
            float const before = 100.0f * float(victim->GetHealth()) / max;
            float const after = 100.0f * float(victim->GetHealth() - damage) / max;
            uint32 const guid = victim->GetGUID().GetCounter();
            uint64 const now = NowMs();
            {
                std::lock_guard<std::mutex> guard(s_lock);
                auto it = s_lastWard.find(guid);
                if (!B::WardFires(before, after, p.v[1], now, it == s_lastWard.end() ? 0 : it->second, B::kWardCooldownMs))
                    return;
                s_lastWard[guid] = now;
            }
            int32 const absorb = B::WardAbsorb(victim->GetMaxHealth(), p.v[2]);
            victim->CastCustomSpell(victim, p.spells->helper1, &absorb, nullptr, nullptr, true);
        }
    };

    class emberfall_beasts_kin_world : public WorldScript
    {
    public:
        emberfall_beasts_kin_world()
            : WorldScript("emberfall_beasts_kin_world", { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_LOAD_CUSTOM_DATABASE_TABLE }) { }

        void OnAfterConfigLoad(bool /*reload*/) override
        {
            s_enable = sConfigMgr->GetOption<bool>("Emberfall.Beasts.Enable", true, false);
        }

        void OnLoadCustomDatabaseTable() override { Load(); }
    };

    class emberfall_beasts_kin_player : public PlayerScript
    {
    public:
        emberfall_beasts_kin_player()
            : PlayerScript("emberfall_beasts_kin_player", { PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_LOGOUT }) { }

        // a passive saved at logout comes back without its companion: it goes, and Kinship puts it on again
        void OnPlayerLogin(Player* player) override
        {
            if (player->getClass() == CLASS_HUNTER)
                StripHunter(player);
        }

        void OnPlayerLogout(Player* player) override
        {
            std::lock_guard<std::mutex> guard(s_lock);
            s_lastWard.erase(player->GetGUID().GetCounter());
        }
    };
}

namespace Emberfall::Beasts
{
    bool SpecialAllowedNow(Pet const* pet, unsigned spell)
    {
        if (!Ready())
            return true;
        FamilyKin const* kin = KinOfPet(pet);
        if (!kin || kin->special != spell || kin->rule == Rule::None)
            return true;
        Unit const* victim = pet->GetVictim();
        if (!victim)
            return true;
        Creature const* c = victim->ToCreature();
        bool const boss = c && (c->isWorldBoss() || c->IsDungeonBoss());
        return SpecialAllowed(kin->rule, victim->IsNonMeleeSpellCast(false), boss);
    }

    unsigned CompanionFamily(Pet const* pet) { return pet ? FamilyOfEntry(pet->GetEntry()) : 0; }

    bool MakeCompanion(Player* player, std::string const& name, std::string& say)
    {
        if (!player || player->getClass() != CLASS_HUNTER)
        {
            say = "Only a hunter keeps a companion.";
            return false;
        }
        uint32 family = 0;
        auto byName = s_familyByName.find(name);
        if (byName != s_familyByName.end())
            family = byName->second;
        else if (!name.empty() && std::all_of(name.begin(), name.end(), [](char c) { return c >= '0' && c <= '9'; }) && name.size() < 9)
            family = uint32(std::stoul(name));
        if (!s_family.count(family))
        {
            say = "No beast family '" + name + "' (a taming/families.yaml key such as wolf, or its id).";
            return false;
        }
        // a beast of that family that a hunter can tame, the lowest entry (any: the companion takes the hunter's level)
        uint32 entry = 0;
        for (auto const& [e, f] : s_beastFamily)
            if (f == family && (!entry || e < entry))
                if (CreatureTemplate const* t = sObjectMgr->GetCreatureTemplate(e))
                    if (t->family && t->IsTameable(true))
                        entry = e;
        if (!entry)
        {
            say = "No tameable beast of that family in the world.";
            return false;
        }
        // the companion out or waiting goes for good (the stable's stalls are left alone)
        if (Pet* pet = player->GetPet())
            player->RemovePet(pet, PET_SAVE_AS_DELETED);
        if (PetStable* stable = player->GetPetStable())
        {
            if (stable->CurrentPet)
            {
                Pet::DeleteFromDB(stable->CurrentPet->PetNumber);
                stable->CurrentPet.reset();
            }
            for (auto it = stable->UnslottedPets.begin(); it != stable->UnslottedPets.end();)
                if (it->Type == HUNTER_PET)
                {
                    Pet::DeleteFromDB(it->PetNumber);
                    it = stable->UnslottedPets.erase(it);
                }
                else
                    ++it;
        }
        Pet* pet = player->CreatePet(entry);
        if (!pet)
        {
            say = "Could not make the companion.";
            return false;
        }
        say = "A " + name + " companion (creature " + std::to_string(entry) + ", family " + std::to_string(family) + ").";
        return true;
    }

    std::string KinInfo(Player* player)
    {
        Pet* pet = CompanionOf(player);
        if (!pet)
            return "No companion out.";
        uint32 const family = FamilyOfEntry(pet->GetEntry());
        FamilyKin const* kin = KinOfPet(pet);
        Passive const p = PassiveOf(player, pet);
        return "companion " + std::to_string(pet->GetEntry()) + ", family " + std::to_string(family) + ", archetype " +
               std::to_string(kin ? uint32(kin->archetype) : 0) + " (passive on the hunter: " + (p.kind != Kind::None ? "yes" : "no") +
               "), special " + std::to_string(kin ? kin->special : 0) + " (" + (kin && pet->HasSpell(kin->special) ? "known" : "not known") +
               "), rule " + std::to_string(kin ? uint32(kin->rule) : 0);
    }
}

void AddEmberfallBeastsKinScripts()
{
    new emberfall_beasts_kin_world();
    new emberfall_beasts_kin_unit();
    new emberfall_beasts_kin_player();
    RegisterSpellScript(spell_ef_pet_kin);
}
