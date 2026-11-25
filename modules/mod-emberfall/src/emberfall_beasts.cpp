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

// The Hunter's beasts, part 1 (Emberfall docs/design/hunter-beasts.md §1-§3; `Emberfall.Beasts.Enable`, default 1):
//   - the stable: every hunter's 80 stalls (the core's MAX_PET_STABLES, PATCHES.md #46) open at login, for free;
//   - the stable helpers the taming and the Class Journey share (PutAway, SwapWith, FreeStall; EfBeasts.h);
//   - the tame choice: "When tamed" in the taming frame (EFB TAMETO), remembered (ef_character_beast_pref);
//   - the field list: EFB LIST sends the stock stable list with the hunter as his own keeper (the window's field mode,
//     no buttons but Call); EFB CALL picks the beast Call from the Stable brings out (emberfall_journeys_hunter.cpp);
//   - the companion's spells by priority: the core's #46 list function from the world's ef_pet_priority rows (by the
//     companion's CreatureFamily, best first; a taunt only while toggled on), and the hand casts of a companion's spell
//     refused (CMSG_PET_ACTION with a spell, CMSG_PET_CAST_SPELL), so no click or macro changes what it does.

#include "Chat.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "EmberfallOwnData.h"
#include "EmberfallPetPriority.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Pet.h"
#include "Player.h"
#include "PlayerScript.h"
#include "ScriptMgr.h"
#include "ServerScript.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "WorldPacket.h"
#include "WorldScript.h"
#include "WorldSession.h"
#include "beasts/EfBeasts.h"

#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace B = Emberfall::Beasts;

namespace
{
    bool s_enable = true;
    bool s_ordered = false;                                            // ef_pet_priority has rows
    std::unordered_map<uint32, std::vector<B::PriorityRow>> s_rows;    // by CreatureFamily; read-only after the load

    std::mutex s_lock;
    std::unordered_map<uint32, B::TameTo> s_pref;   // by the hunter's GUID counter
    std::unordered_map<uint32, uint32> s_pick;      // the field list's pick: pet number, by the hunter's GUID counter

    void Load()
    {
        s_rows.clear();
        if (QueryResult r = WorldDatabase.Query("SELECT `family`, `spell`, `reserve`, `taunt` FROM `ef_pet_priority` ORDER BY `family`, `ord`"))
            do
            {
                // the spells are not loaded yet at this hook: a row naming no spell is skipped by the pet AI (and the
                // content build checks every row against the kinds' lines, pet_combat.py)
                Field* f = r->Fetch();
                s_rows[f[0].Get<uint32>()].push_back({f[1].Get<uint32>(), f[2].Get<uint32>(), f[3].Get<uint8>() != 0});
            } while (r->NextRow());
        s_ordered = s_enable && !s_rows.empty();
        LOG_INFO("server.loading", ">> Emberfall beasts: companion priority {} ({} kinds)", s_ordered ? "on" : "off", s_rows.size());
    }

    Pet const* HunterCompanion(Unit const* unit)
    {
        if (!unit || !unit->IsPet())
            return nullptr;
        Pet const* pet = static_cast<Pet const*>(unit);
        if (pet->getPetType() != HUNTER_PET || !pet->GetOwnerGUID().IsPlayer())
            return nullptr;
        return pet;
    }

    // The core's #46 list function: the companion's kind's rows, the known ones, the taunt while toggled on.
    bool PriorityList(Unit const* unit, std::vector<uint32>& out)
    {
        if (!Emberfall::OwnData())
            return false;
        Pet const* pet = HunterCompanion(unit);
        if (!pet || !pet->GetCreatureTemplate())
            return false;
        auto it = s_rows.find(pet->GetCreatureTemplate()->family);
        if (it == s_rows.end())
            return false;
        B::Order(it->second, pet->GetPower(pet->getPowerType()), [pet](uint32 spell) {
            B::SpellState s;
            auto const sp = pet->m_spells.find(spell);
            if (sp == pet->m_spells.end() || sp->second.state == PETSPELL_REMOVED)
                return s;
            s.known = true;
            s.toggledOn = sp->second.active == ACT_ENABLED;
            if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spell))
                s.cost = info->ManaCost;
            return s;
        }, out);
        return true;
    }

    void Send(Player* player, std::string const& body)
    {
        std::string const text = std::string(B::kPrefix) + body;
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player, player, text);
        player->SendDirectMessage(&data);
    }

    B::TameTo PrefOf(uint32 guid)
    {
        std::lock_guard<std::mutex> guard(s_lock);
        auto it = s_pref.find(guid);
        return it == s_pref.end() ? B::TameTo::WithMe : it->second;
    }

    int StallOf(PetStable const* stable, uint32 petNumber)
    {
        if (!stable || !petNumber)
            return -1;
        for (uint32 i = 0; i < stable->MaxStabledPets && i < MAX_PET_STABLES; ++i)
            if (stable->StabledPets[i] && stable->StabledPets[i]->PetNumber == petNumber)
                return int(i);
        return -1;
    }

    void HandleMessage(Player* player, std::string_view body)
    {
        if (player->getClass() != CLASS_HUNTER)
            return;
        std::optional<B::Request> const req = B::Parse(body);
        if (!req)
            return;
        uint32 const guid = player->GetGUID().GetCounter();
        switch (req->kind)
        {
            case B::Request::Kind::Hello:
            {
                Send(player, B::Pref(PrefOf(guid)));
                std::vector<uint32> taunts;
                for (auto const& [family, rows] : s_rows)
                    for (B::PriorityRow const& r : rows)
                        if (r.taunt && std::find(taunts.begin(), taunts.end(), r.spell) == taunts.end())
                            taunts.push_back(r.spell);
                Send(player, B::Taunts(taunts, s_ordered && Emberfall::OwnData()));
                break;
            }
            case B::Request::Kind::TameTo:
            {
                {
                    std::lock_guard<std::mutex> guard(s_lock);
                    s_pref[guid] = req->to;
                }
                CharacterDatabase.Execute("REPLACE INTO `ef_character_beast_pref` (`guid`, `tame_to`) VALUES ({}, {})", guid,
                                          uint32(req->to));
                Send(player, B::Pref(req->to));
                break;
            }
            case B::Request::Kind::List:
                // the stock list with the hunter as his own keeper: the window opens in field mode (only Call); its keeper
                // buttons need a stable keeper or the open-stable aura (Stable at Hand), as the core checks
                player->GetSession()->SendStablePet(player->GetGUID());
                break;
            case B::Request::Kind::Call:
            {
                bool const ok = StallOf(player->GetPetStable(), req->pet) >= 0;
                {
                    std::lock_guard<std::mutex> guard(s_lock);
                    if (ok)
                        s_pick[guid] = req->pet;
                    else
                        s_pick.erase(guid);
                }
                Send(player, B::Called(ok ? req->pet : 0));
                // the cast itself (its 3 s, its cooldown, out of combat: the spell's own checks)
                if (ok && player->HasSpell(B::kSpellCallFromTheStable)
                    && player->CastSpell(player, B::kSpellCallFromTheStable, false) != SPELL_CAST_OK)
                {
                    // refused (a fight, the cooldown): the pick goes, so the next press opens the list again
                    std::lock_guard<std::mutex> guard(s_lock);
                    s_pick.erase(guid);
                }
                break;
            }
        }
    }

    // A hand cast of the companion's spell (a pet bar click, a macro's CastPetAction or CastSpellByName on the pet).
    bool IsRefusedHandCast(WorldSession* session, WorldPacket const& packet)
    {
        uint16 const op = packet.GetOpcode();
        if (op != CMSG_PET_ACTION && op != CMSG_PET_CAST_SPELL)
            return false;
        Player* player = session ? session->GetPlayer() : nullptr;
        if (!s_ordered || !player || player->getClass() != CLASS_HUNTER || packet.size() < 12)
            return false;
        ObjectGuid const petGuid(packet.read<uint64>(0));
        if (petGuid != player->GetPetGUID())
            return false;
        bool isSpell = false;
        if (op == CMSG_PET_ACTION)
            isSpell = B::IsSpellAct(B::ActState(packet.read<uint32>(8))) && B::ActSpell(packet.read<uint32>(8)) != 0;
        else
            isSpell = true;   // CMSG_PET_CAST_SPELL: guid, cast count, spell, ...
        Pet* pet = player->GetPet();
        return B::RefuseHandCast(Emberfall::OwnData(), pet && pet->getPetType() == HUNTER_PET, isSpell);
    }

    class emberfall_beasts_world : public WorldScript
    {
    public:
        emberfall_beasts_world()
            : WorldScript("emberfall_beasts_world", { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_LOAD_CUSTOM_DATABASE_TABLE }) { }

        void OnAfterConfigLoad(bool /*reload*/) override
        {
            s_enable = sConfigMgr->GetOption<bool>("Emberfall.Beasts.Enable", true, false);
        }

        void OnLoadCustomDatabaseTable() override
        {
            Load();
            Emberfall::PetPriority::Set(s_ordered ? &PriorityList : nullptr);
        }
    };

    class emberfall_beasts_server : public ServerScript
    {
    public:
        emberfall_beasts_server() : ServerScript("emberfall_beasts_server", { SERVERHOOK_CAN_PACKET_RECEIVE }) { }

        bool CanPacketReceive(WorldSession* session, WorldPacket const& packet) override
        {
            // refused silently: our client never sends it (its pet bar says why in the player's language)
            return !IsRefusedHandCast(session, packet);
        }
    };

    class emberfall_beasts_player : public PlayerScript
    {
    public:
        emberfall_beasts_player()
            : PlayerScript("emberfall_beasts_player", { PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_LOGOUT, PLAYERHOOK_ON_DELETE_FROM_DB,
                PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT }) { }

        void OnPlayerLogin(Player* player) override
        {
            if (!s_enable || player->getClass() != CLASS_HUNTER)
                return;
            // every stall open (the core saves the number in characters.stable_slots)
            PetStable& stable = player->GetOrInitPetStable();
            stable.MaxStabledPets = B::OpenStalls(stable.MaxStabledPets, MAX_PET_STABLES);
            uint32 const guid = player->GetGUID().GetCounter();
            if (QueryResult r = CharacterDatabase.Query("SELECT `tame_to` FROM `ef_character_beast_pref` WHERE `guid` = {}", guid))
            {
                std::lock_guard<std::mutex> guard(s_lock);
                s_pref[guid] = r->Fetch()[0].Get<uint8>() == uint8(B::TameTo::Stable) ? B::TameTo::Stable : B::TameTo::WithMe;
            }
        }

        void OnPlayerLogout(Player* player) override
        {
            std::lock_guard<std::mutex> guard(s_lock);
            s_pref.erase(player->GetGUID().GetCounter());
            s_pick.erase(player->GetGUID().GetCounter());
        }

        void OnPlayerDeleteFromDB(CharacterDatabaseTransaction trans, uint32 guid) override
        {
            trans->Append("DELETE FROM `ef_character_beast_pref` WHERE `guid` = {}", guid);
        }

        // EFB: every `EFB\t` addon whisper is consumed, only the player's own (to itself) answered.
        bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 language, std::string& msg, Player* receiver) override
        {
            if (type != CHAT_MSG_WHISPER || language != uint32(LANG_ADDON) || msg.compare(0, B::kPrefix.size(), B::kPrefix) != 0)
                return true;
            if (receiver == player && s_enable)
                HandleMessage(player, std::string_view(msg).substr(B::kPrefix.size()));
            return false;
        }
    };
}

namespace Emberfall::Beasts
{
    int FreeStall(PetStable const* stable)
    {
        if (!stable)
            return -1;
        std::vector<bool> used;
        for (auto const& p : stable->StabledPets)
            used.push_back(bool(p));
        return FirstFree(used, stable->MaxStabledPets);
    }

    bool PutAway(Player* player, int& stall)
    {
        stall = -1;
        PetStable* stable = player->GetPetStable();
        if (!stable || (!stable->CurrentPet && stable->UnslottedPets.empty() && !player->GetPet()))
            return true;
        int const slot = FreeStall(stable);
        if (slot < 0)
            return false;
        if (Pet* pet = player->GetPet())
        {
            if (pet->getPetType() != HUNTER_PET)
                return false;
            player->RemovePet(pet, PetSaveMode(PET_SAVE_FIRST_STABLE_SLOT + slot));
            std::swap(stable->StabledPets[slot], stable->CurrentPet);
            stall = slot;
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
        stall = slot;
        return true;
    }

    bool SwapWith(Player* player, int stall)
    {
        PetStable* stable = player->GetPetStable();
        Pet* old = player->GetPet();
        if (!stable || !old || !old->IsAlive() || !old->IsHunterPet() || stall < 0 || stall >= int(MAX_PET_STABLES)
            || !stable->StabledPets[stall])
            return false;
        uint32 const petId = stable->StabledPets[stall]->PetNumber;
        CreatureTemplate const* t = sObjectMgr->GetCreatureTemplate(stable->StabledPets[stall]->CreatureId);
        if (!t || !t->IsTameable(player->CanTameExoticPets()))
            return false;
        player->RemovePet(old, PetSaveMode(PET_SAVE_FIRST_STABLE_SLOT + stall));
        Pet* pet = new Pet(player, HUNTER_PET);
        if (!pet->LoadPetFromDB(player, 0, petId, false))
        {
            delete pet;
            if (stable->CurrentPet)
            {
                stable->UnslottedPets.push_back(std::move(*stable->CurrentPet));
                stable->CurrentPet.reset();
            }
            CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_UPD_CHAR_PET_SLOT_BY_ID);
            stmt->SetData(0, PET_SAVE_NOT_IN_SLOT);
            stmt->SetData(1, player->GetGUID().GetCounter());
            stmt->SetData(2, petId);
            CharacterDatabase.Execute(stmt);
            return false;
        }
        CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_UPD_CHAR_PET_SLOT_BY_ID);
        stmt->SetData(0, PET_SAVE_AS_CURRENT);
        stmt->SetData(1, player->GetGUID().GetCounter());
        stmt->SetData(2, petId);
        CharacterDatabase.Execute(stmt);
        return true;
    }

    bool HasCallPick(Player* player)
    {
        std::lock_guard<std::mutex> guard(s_lock);
        return s_pick.count(player->GetGUID().GetCounter()) != 0;
    }

    void OpenFieldList(Player* player) { player->GetSession()->SendStablePet(player->GetGUID()); }

    TameTo TamePref(Player* player) { return s_enable ? PrefOf(player->GetGUID().GetCounter()) : TameTo::WithMe; }

    int TakeCallPick(Player* player)
    {
        uint32 pet = 0;
        {
            std::lock_guard<std::mutex> guard(s_lock);
            auto it = s_pick.find(player->GetGUID().GetCounter());
            if (it == s_pick.end())
                return -1;
            pet = it->second;
            s_pick.erase(it);
        }
        return StallOf(player->GetPetStable(), pet);
    }
}

void AddEmberfallBeastsScripts()
{
    new emberfall_beasts_world();
    new emberfall_beasts_server();
    new emberfall_beasts_player();
}
