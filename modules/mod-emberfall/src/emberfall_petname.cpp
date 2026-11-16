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

// A new companion's default name (Emberfall docs/design/hunter-taming.md §17, feedback #579: "the fell goat name after
// taming is boar"). The core names a tamed beast after its CreatureFamily (Pet::CreateBaseAtCreature: Hound, Lynx, Boar,
// Hawk, Lizard, the companion kinds), so every goat, deer, bear, ram or pony filed under Boar was called "Boar". Here a
// hunter's companion that still carries a kind's name and was never renamed by its hunter takes the creature's own name,
// in the hunter's language (src/petname/EfPetNameLogic.h: "fell goat" -> "Fell Goat"):
//
//   - at a tame (its stats set, before the core saves it) and whenever a companion comes into the world;
//   - at login for every companion the hunter keeps (out, stabled, sent off: the stable's list shows the new name).
//
// The kind (its abilities, its CreatureFamily) stays; only the name changes. Any data: a stock world's family names are
// left alone unless a pet carries one (then it is named after its creature, as here).

#include "Chat.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "GameTime.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Pet.h"
#include "PetScript.h"
#include "Player.h"
#include "PlayerScript.h"
#include "ScriptMgr.h"
#include "WorldSession.h"
#include "petname/EfPetNameLogic.h"

namespace
{
    namespace PN = Emberfall::PetName;

    // Whether `name` is a companion kind's name in any language (the core's default), or the core's fallback.
    bool IsKindName(std::string const& name)
    {
        if (name.empty() || name == "Pet")
            return true;
        for (uint32 i = 0; i < sCreatureFamilyStore.GetNumRows(); ++i)
            if (CreatureFamilyEntry const* f = sCreatureFamilyStore.LookupEntry(i))
                for (char const* n : f->Name)
                    if (n && *n && name == n)
                        return true;
        return false;
    }

    // The creature's name in the hunter's language.
    std::string CreatureNameFor(Player* owner, uint32 entry)
    {
        CreatureTemplate const* t = sObjectMgr->GetCreatureTemplate(entry);
        if (!t)
            return {};
        std::string name = t->Name;
        LocaleConstant const loc = owner && owner->GetSession() ? owner->GetSession()->GetSessionDbLocaleIndex() : LOCALE_enUS;
        if (loc != LOCALE_enUS)
            if (CreatureLocale const* cl = sObjectMgr->GetCreatureLocale(entry))
                ObjectMgr::GetLocaleString(cl->Name, loc, name);
        return name;
    }

    void Store(uint32 petNumber, std::string const& name)
    {
        std::string escaped = name;
        CharacterDatabase.EscapeString(escaped);
        CharacterDatabase.Execute("UPDATE `character_pet` SET `name` = '{}' WHERE `id` = {} AND `renamed` = 0", escaped, petNumber);
    }

    // The companion takes its creature's name if it still carries a kind's and was never renamed.
    void Rename(Pet* pet)
    {
        if (!pet || pet->getPetType() != HUNTER_PET || !pet->HasByteFlag(UNIT_FIELD_BYTES_2, 2, UNIT_CAN_BE_RENAMED))
            return;
        if (!IsKindName(pet->GetName()))
            return;
        Player* owner = pet->GetOwner();
        std::string const name = PN::DefaultName(CreatureNameFor(owner, pet->GetEntry()));
        if (name.empty() || name == pet->GetName())
            return;
        LOG_INFO("module", "Emberfall pet name: {}'s companion {} (pet {}, creature {}) is {} now", owner ? owner->GetName() : "?",
                 pet->GetName(), pet->GetUInt32Value(UNIT_FIELD_PETNUMBER), pet->GetEntry(), name);
        pet->SetName(name);
        pet->SetUInt32Value(UNIT_FIELD_PET_NAME_TIMESTAMP, uint32(GameTime::GetGameTime().count()));   // clients ask again
        if (uint32 const number = pet->GetUInt32Value(UNIT_FIELD_PETNUMBER))
        {
            Store(number, name);
            if (PetStable* stable = owner ? owner->GetPetStable() : nullptr)
                if (stable->CurrentPet && stable->CurrentPet->PetNumber == number)
                    stable->CurrentPet->Name = name;
        }
    }

    // A companion called or loaded (its owner known: the core calls this hook only then).
    class emberfall_petname_pet : public PetScript
    {
    public:
        emberfall_petname_pet() : PetScript("emberfall_petname_pet", { PETHOOK_ON_PET_ADD_TO_WORLD }) { }

        void OnPetAddToWorld(Pet* pet) override { Rename(pet); }
    };

    class emberfall_petname_player : public PlayerScript
    {
    public:
        emberfall_petname_player() : PlayerScript("emberfall_petname_player", { PLAYERHOOK_ON_LOGIN,
            PLAYERHOOK_ON_AFTER_GUARDIAN_INIT_STATS_FOR_LEVEL }) { }

        // A tame: the new pet's stats are set (Unit::InitTamedPet) after the core named it, before it enters the world (with
        // no owner GUID yet, so OnPetAddToWorld does not come) and before the core saves it.
        void OnPlayerAfterGuardianInitStatsForLevel(Player* /*player*/, Guardian* guardian) override
        {
            if (guardian && guardian->IsPet())
                Rename(guardian->ToPet());
        }

        // Every companion the hunter keeps and never renamed, still called after its kind: its creature's name.
        void OnPlayerLogin(Player* player) override
        {
            if (player->getClass() != CLASS_HUNTER)
                return;
            PetStable* stable = player->GetPetStable();
            if (!stable)
                return;
            auto fix = [&](PetStable::PetInfo& p) {
                if (p.Type != HUNTER_PET || p.WasRenamed || !IsKindName(p.Name))
                    return;
                std::string const name = PN::DefaultName(CreatureNameFor(player, p.CreatureId));
                if (name.empty() || name == p.Name)
                    return;
                LOG_INFO("module", "Emberfall pet name: {}'s companion {} (pet {}, creature {}) is {} now", player->GetName(), p.Name,
                         p.PetNumber, p.CreatureId, name);
                p.Name = name;
                Store(p.PetNumber, name);
            };
            if (stable->CurrentPet)
                fix(*stable->CurrentPet);
            for (auto& p : stable->StabledPets)
                if (p)
                    fix(*p);
            for (auto& p : stable->UnslottedPets)
                fix(p);
        }
    };
}

void AddEmberfallPetNameScripts()
{
    new emberfall_petname_pet();
    new emberfall_petname_player();
}
