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

// The hunter companion's size and growth (Emberfall docs/design/hunter-taming.md §18; feedback #571 "a normalized size
// when tamed, maybe defined by family ... bigger than their untamed relatives" and #572 "first its a young one ... then
// over time it grows ... visible to the hunter on the tooltip"). The rules are src/petsize/EfPetSizeLogic.h; here:
//
//   - the size: the core asks for a hunter pet's native scale and its reach (PATCHES.md #41, EmberfallPetSize.h); the
//     answer is its kind's size (ef_pet_size_kind by CreatureFamily) at its stage, over its display's own size
//     (ef_pet_size_display), so every path that scales a pet (creation, level up, scale auras, a transform's end) keeps
//     it; the reach and bounding radius follow the size within the rule's limits (ef_pet_size_rule);
//   - the growth: the seconds each companion is out with its hunter (alive, in the world, the hunter online), per pet
//     (ef_character_pet_growth, loaded with the hunter, written at his saves and logout); a new stage rescales it and
//     tells the client (GROW with kGrew: the client's chat line);
//   - GROW (EfTameWire.h) for every pet after the client's taming HELLO, when one comes out (seen in the hunter's update:
//     the core's OnPetAddToWorld does not come for a fresh tame), every minute it is out and when it grows: the stage and the time to the next one on its tooltip and the stable's row;
//   - `.efpet` (GM): `info`, `grow <hours>` (adds time out to the companion that is out), `stage <n>` (1 as tamed);
//   - the beast at heel (emberfall_journeys_hunter.cpp) stands at its own size and stage (ScaleForPet).
//
// Switch Emberfall.PetSize.Enable (default 1); without the world rows (a stock world) nothing changes.

#include "Chat.h"
#include "ChatCommand.h"
#include "CommandScript.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "EmberfallPetSize.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Pet.h"
#include "Player.h"
#include "PlayerScript.h"
#include "ScriptMgr.h"
#include "WorldScript.h"
#include "encounters/EfEncounterMgr.h"
#include "petsize/EfPetSize.h"
#include "petsize/EfPetSizeLogic.h"
#include "taming/EfTameWire.h"

#include <cmath>
#include <mutex>
#include <unordered_map>
#include <vector>

using namespace Acore::ChatCommands;

namespace
{
    namespace PS = Emberfall::PetSize;
    namespace E = Emberfall::Encounters;
    namespace W = eftame::wire;

    constexpr uint32 kAddEveryMs = 10000;     // the time out is added in steps of ten seconds
    constexpr uint32 kSyncEveryMs = 60000;    // GROW to the client every minute the companion is out
    constexpr char const* kTickKey = "ef_pet_growth";

    bool s_enable = true;

    // The world data: read at startup, read-only afterwards.
    bool s_ready = false;
    PS::Rule s_rule;
    std::unordered_map<uint32, float> s_kind;      // CreatureFamily -> full size (yd)
    std::unordered_map<uint32, float> s_display;   // display -> its side-on size at scale 1 (yd)

    struct Growth
    {
        uint32 owner = 0;
        uint32 seconds = 0;
        bool dirty = false;
    };
    std::mutex s_lock;
    std::unordered_map<uint32, Growth> s_growth;   // by pet number (character_pet.id)

    struct Tick : public DataMap::Base
    {
        uint32 ms = 0, sync = 0;
        uint32 pet = 0;   // the companion out last update (a new one: GROW at once)
    };

    void Load()
    {
        s_ready = false;
        s_rule = PS::Rule{};
        s_kind.clear();
        s_display.clear();
        if (QueryResult r = WorldDatabase.Query(
                "SELECT `reach_per_yard_permil`, `reach_min_milli`, `reach_max_milli`, `radius_per_yard_permil`, `radius_min_milli`, "
                "`radius_max_milli`, `scale_min_permil`, `scale_max_permil` FROM `ef_pet_size_rule` WHERE `id` = 1"))
        {
            Field* f = r->Fetch();
            float* fields[] = {&s_rule.reachPerYard, &s_rule.reachMin, &s_rule.reachMax, &s_rule.radiusPerYard, &s_rule.radiusMin,
                               &s_rule.radiusMax, &s_rule.scaleMin, &s_rule.scaleMax};
            for (std::size_t i = 0; i < std::size(fields); ++i)
                *fields[i] = float(f[i].Get<uint32>()) / 1000.0f;
        }
        if (QueryResult r = WorldDatabase.Query("SELECT `seconds`, `share_permil` FROM `ef_pet_size_stage` ORDER BY `stage`"))
            do
            {
                s_rule.stages.push_back({r->Fetch()[0].Get<uint32>(), r->Fetch()[1].Get<uint32>()});
            } while (r->NextRow());
        if (QueryResult r = WorldDatabase.Query("SELECT `family`, `size_milli` FROM `ef_pet_size_kind`"))
            do
            {
                s_kind[r->Fetch()[0].Get<uint32>()] = float(r->Fetch()[1].Get<uint32>()) / 1000.0f;
            } while (r->NextRow());
        if (QueryResult r = WorldDatabase.Query("SELECT `display`, `size_milli` FROM `ef_pet_size_display`"))
            do
            {
                s_display[r->Fetch()[0].Get<uint32>()] = float(r->Fetch()[1].Get<uint32>()) / 1000.0f;
            } while (r->NextRow());
        bool const valid = PS::Valid(s_rule);
        s_ready = s_enable && valid && !s_kind.empty() && !s_display.empty();
        if (!valid && !s_kind.empty())
            LOG_ERROR("server.loading", ">> Emberfall pet size: ef_pet_size_rule / ef_pet_size_stage do not make sense: off");
        LOG_INFO("server.loading", ">> Emberfall pet size: {} ({} kinds, {} stages, {} displays)", s_ready ? "on" : "off",
                 s_kind.size(), s_rule.stages.size(), s_display.size());
    }

    uint32 SecondsOf(uint32 pet)
    {
        std::lock_guard<std::mutex> guard(s_lock);
        auto it = s_growth.find(pet);
        return it == s_growth.end() ? 0 : it->second.seconds;
    }

    float DisplaySize(uint32 native, uint32 current)
    {
        auto it = s_display.find(native);
        if (it == s_display.end())
            it = s_display.find(current);
        return it == s_display.end() ? 0.0f : it->second;
    }

    bool ScaleOf(uint32 family, float displaySize, uint32 seconds, float& scale)
    {
        auto kind = s_kind.find(family);
        if (!s_ready || kind == s_kind.end() || !(displaySize > 0.0f))
            return false;
        scale = PS::ScaleFor(s_rule, PS::SizeOf(s_rule, kind->second, PS::StageOf(s_rule, seconds)), displaySize);
        return true;
    }

    uint32 PetNumber(Pet const* pet) { return pet->GetUInt32Value(UNIT_FIELD_PETNUMBER); }

    // The core's two questions (EmberfallPetSize.h).
    bool HookScale(Pet const* pet, float& scale)
    {
        if (!s_ready || !pet || pet->getPetType() != HUNTER_PET || !pet->GetCreatureTemplate())
            return false;
        return ScaleOf(pet->GetCreatureTemplate()->family, DisplaySize(pet->GetNativeDisplayId(), pet->GetDisplayId()),
                       SecondsOf(PetNumber(pet)), scale);
    }

    bool HookReach(Pet const* pet, float scale, float& reach, float& radius)
    {
        if (!s_ready || !pet || pet->getPetType() != HUNTER_PET || !pet->GetCreatureTemplate() ||
            !s_kind.count(pet->GetCreatureTemplate()->family))
            return false;
        float const base = DisplaySize(pet->GetNativeDisplayId(), pet->GetDisplayId());
        if (!(base > 0.0f))
            return false;
        reach = PS::ReachFor(s_rule, base * scale);
        radius = PS::RadiusFor(s_rule, base * scale);
        return true;
    }

    W::Grow GrowOf(uint32 pet, uint32 seconds, uint32 flags)
    {
        return W::Grow{pet, PS::StageOf(s_rule, seconds), uint32(s_rule.stages.size()), seconds, PS::SecondsToNext(s_rule, seconds), flags};
    }

    void SendGrow(Player* player, uint32 pet, uint32 seconds, uint32 flags)
    {
        if (!s_ready || !player || !pet)
            return;
        std::string const body = W::EncodeGrow(E::NextSeq(player), GrowOf(pet, seconds, flags));
        if (!body.empty())
            E::Send(player, body);
    }

    // Adds `seconds` out to the hunter's companion that is out: rescaled and told when it grows.
    void AddTime(Player* player, Pet* pet, uint32 seconds, bool sync)
    {
        uint32 const number = PetNumber(pet);
        if (!number)
            return;
        uint32 before = 0, after = 0;
        {
            std::lock_guard<std::mutex> guard(s_lock);
            Growth& g = s_growth[number];
            g.owner = player->GetGUID().GetCounter();
            before = g.seconds;
            g.seconds = after = uint32(std::min<uint64>(uint64(g.seconds) + seconds, 0xFFFFFFF0ull));
            g.dirty = g.dirty || after != before;
        }
        if (PS::Grew(s_rule, before, after))
        {
            pet->RecalculateObjectScale();
            LOG_INFO("module", "Emberfall pet size: {} ({}, pet {}) of {} grew to stage {} after {} s out", pet->GetName(),
                     pet->GetEntry(), number, player->GetName(), PS::StageOf(s_rule, after) + 1, after);
            SendGrow(player, number, after, W::kGrew | W::kOut);
        }
        else if (sync)
            SendGrow(player, number, after, W::kOut);
    }

    // Puts the companion that is out at `seconds` (the GM command); rescaled and told.
    void SetTime(Player* player, Pet* pet, uint32 seconds)
    {
        uint32 const number = PetNumber(pet);
        uint32 before = 0;
        {
            std::lock_guard<std::mutex> guard(s_lock);
            Growth& g = s_growth[number];
            g.owner = player->GetGUID().GetCounter();
            before = g.seconds;
            g.seconds = seconds;
            g.dirty = true;
        }
        pet->RecalculateObjectScale();
        SendGrow(player, number, seconds, (PS::Grew(s_rule, before, seconds) ? W::kGrew : 0) | W::kOut);
    }

    void Flush(uint32 owner, bool forget)
    {
        std::vector<std::pair<uint32, uint32>> rows;
        {
            std::lock_guard<std::mutex> guard(s_lock);
            for (auto it = s_growth.begin(); it != s_growth.end();)
            {
                if (it->second.owner != owner)
                {
                    ++it;
                    continue;
                }
                if (it->second.dirty)
                    rows.emplace_back(it->first, it->second.seconds);
                it->second.dirty = false;
                it = forget ? s_growth.erase(it) : std::next(it);
            }
        }
        for (auto const& [pet, seconds] : rows)
            CharacterDatabase.Execute("REPLACE INTO `ef_character_pet_growth` (`pet`, `owner`, `seconds`) VALUES ({}, {}, {})",
                                      pet, owner, seconds);
    }

    class emberfall_petsize_world : public WorldScript
    {
    public:
        emberfall_petsize_world()
            : WorldScript("emberfall_petsize_world", { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_LOAD_CUSTOM_DATABASE_TABLE }) { }

        void OnAfterConfigLoad(bool /*reload*/) override
        {
            s_enable = sConfigMgr->GetOption<bool>("Emberfall.PetSize.Enable", true, false);
        }

        void OnLoadCustomDatabaseTable() override
        {
            Load();
            PS::Set(s_ready ? &HookScale : nullptr, s_ready ? &HookReach : nullptr);
        }
    };

    class emberfall_petsize_player : public PlayerScript
    {
    public:
        emberfall_petsize_player()
            : PlayerScript("emberfall_petsize_player", { PLAYERHOOK_ON_LOAD_FROM_DB, PLAYERHOOK_ON_UPDATE, PLAYERHOOK_ON_SAVE,
                PLAYERHOOK_ON_LOGOUT, PLAYERHOOK_ON_DELETE_FROM_DB }) { }

        // Before the companion is loaded (Player::LoadPet), so it comes into the world at its size.
        void OnPlayerLoadFromDB(Player* player) override
        {
            if (!s_ready || player->getClass() != CLASS_HUNTER)
                return;
            uint32 const owner = player->GetGUID().GetCounter();
            if (QueryResult r = CharacterDatabase.Query("SELECT `pet`, `seconds` FROM `ef_character_pet_growth` WHERE `owner` = {}", owner))
            {
                std::lock_guard<std::mutex> guard(s_lock);
                do
                {
                    s_growth[r->Fetch()[0].Get<uint32>()] = Growth{owner, r->Fetch()[1].Get<uint32>(), false};
                } while (r->NextRow());
            }
        }

        void OnPlayerUpdate(Player* player, uint32 diff) override
        {
            if (!s_ready || player->getClass() != CLASS_HUNTER)
                return;
            Pet* pet = player->GetPet();
            Tick* t = player->CustomData.GetDefault<Tick>(kTickKey);
            uint32 const number = pet && pet->IsHunterPet() && pet->IsInWorld() ? PetNumber(pet) : 0;
            if (number != t->pet)
            {
                // a companion came out (a tame, a call, a swap): its growth to the client at once
                t->pet = number;
                if (number)
                    SendGrow(player, number, SecondsOf(number), W::kOut);
            }
            if (!number || !pet->IsAlive())
                return;
            t->ms += diff;
            t->sync += diff;
            if (t->ms < kAddEveryMs)
                return;
            uint32 const seconds = t->ms / 1000;
            t->ms %= 1000;
            bool const sync = t->sync >= kSyncEveryMs;
            if (sync)
                t->sync = 0;
            AddTime(player, pet, seconds, sync);
        }

        void OnPlayerSave(Player* player) override { Flush(player->GetGUID().GetCounter(), false); }

        void OnPlayerLogout(Player* player) override { Flush(player->GetGUID().GetCounter(), true); }

        void OnPlayerDeleteFromDB(CharacterDatabaseTransaction trans, uint32 guid) override
        {
            trans->Append("DELETE FROM `ef_character_pet_growth` WHERE `owner` = {}", guid);
        }
    };

    class emberfall_petsize_commands : public CommandScript
    {
    public:
        emberfall_petsize_commands() : CommandScript("emberfall_petsize_commands") { }

        ChatCommandTable GetCommands() const override
        {
            static ChatCommandTable petTable =
            {
                { "info",  HandleInfo,  SEC_GAMEMASTER, Console::No },
                { "grow",  HandleGrow,  SEC_GAMEMASTER, Console::No },
                { "stage", HandleStage, SEC_GAMEMASTER, Console::No },
                { "",      HandleInfo,  SEC_GAMEMASTER, Console::No },
            };
            static ChatCommandTable commandTable =
            {
                { "efpet", petTable },
            };
            return commandTable;
        }

        static Pet* OutPet(ChatHandler* handler)
        {
            Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
            Pet* pet = player ? player->GetPet() : nullptr;
            if (!s_ready || !pet || !pet->IsHunterPet())
            {
                handler->SendSysMessage(s_ready ? "No companion out." : "Emberfall pet size is off (no world rows or Emberfall.PetSize.Enable 0).");
                handler->SetSentErrorMessage(true);
                return nullptr;
            }
            return pet;
        }

        static bool HandleInfo(ChatHandler* handler)
        {
            Pet* pet = OutPet(handler);
            if (!pet)
                return false;
            uint32 const seconds = SecondsOf(PetNumber(pet));
            uint32 const family = pet->GetCreatureTemplate()->family;
            float const base = DisplaySize(pet->GetNativeDisplayId(), pet->GetDisplayId());
            auto kind = s_kind.find(family);
            handler->PSendSysMessage("{} (pet {}, family {}, display {} of {:.2f} yd): stage {} of {}, {} s out, {} s to the next; "
                                     "kind {:.2f} yd, now {:.2f} yd at scale {:.2f}, reach {:.2f}, radius {:.2f}",
                                     pet->GetName(), PetNumber(pet), family, pet->GetNativeDisplayId(), base,
                                     PS::StageOf(s_rule, seconds) + 1, s_rule.stages.size(), seconds, PS::SecondsToNext(s_rule, seconds),
                                     kind == s_kind.end() ? 0.0f : kind->second, base * pet->GetObjectScale(), pet->GetObjectScale(),
                                     pet->GetCombatReach(), pet->GetFloatValue(UNIT_FIELD_BOUNDINGRADIUS));
            return true;
        }

        static bool HandleGrow(ChatHandler* handler, float hours)
        {
            Pet* pet = OutPet(handler);
            if (!pet || !(hours > 0.0f))
                return false;
            Player* player = handler->GetSession()->GetPlayer();
            AddTime(player, pet, uint32(std::lround(hours * 3600.0f)), true);
            return HandleInfo(handler);
        }

        static bool HandleStage(ChatHandler* handler, uint32 stage)
        {
            Pet* pet = OutPet(handler);
            if (!pet || stage < 1)
                return false;
            SetTime(handler->GetSession()->GetPlayer(), pet, PS::SecondsFor(s_rule, stage - 1));
            return HandleInfo(handler);
        }
    };
}

void Emberfall::PetSize::SendGrowth(Player* player)
{
    if (!s_ready || !player || player->getClass() != CLASS_HUNTER)
        return;
    PetStable const* stable = player->GetPetStable();
    if (!stable)
        return;
    Pet const* out = player->GetPet();
    uint32 const outNumber = out && out->IsHunterPet() ? PetNumber(out) : 0;
    std::vector<uint32> pets;
    if (stable->CurrentPet)
        pets.push_back(stable->CurrentPet->PetNumber);
    for (auto const& p : stable->StabledPets)
        if (p)
            pets.push_back(p->PetNumber);
    for (auto const& p : stable->UnslottedPets)
        if (p.Type == HUNTER_PET)
            pets.push_back(p.PetNumber);
    for (uint32 pet : pets)
        SendGrow(player, pet, SecondsOf(pet), pet == outNumber ? W::kOut : 0);
}

bool Emberfall::PetSize::ScaleForPet(uint32 family, uint32 display, uint32 pet, float& scale)
{
    return ScaleOf(family, DisplaySize(display, display), SecondsOf(pet), scale);
}

void AddEmberfallPetSizeScripts()
{
    new emberfall_petsize_world();
    new emberfall_petsize_player();
    new emberfall_petsize_commands();
}
