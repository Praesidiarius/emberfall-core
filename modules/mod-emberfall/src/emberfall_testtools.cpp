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

// Emberfall test tools (Emberfall docs/design/class-kit-pipeline.md §8.3): a test realm's players try a class kit at any
// level without a GM account. Off unless `Emberfall.TestTools.Enable` = 1 (AC_EMBERFALL_TEST_TOOLS_ENABLE); a test
// realm sets it, a live realm never does. Every command acts on the player's own character only.
//
//   .eftest                  the list
//   .eftest level N          your level (1 .. MaxPlayerLevel), XP 0, full health and mana; the talent system gives the
//                            level's points (its own level hook)
//   .eftest zone Z [L]       you as a player arriving at zone Z (at level L): the level, the quests of the zones before
//                            it done (its breadcrumbs in the log), pool gear, flight nodes, money, at its main hub
//                            (src/emberfall_testzone.cpp); `.eftest zone list` (or no zone): the zones in route order
//   .eftest foe [L] [N] [idle] [hpF]  N (1-5) training foes of level L (default: yours) in front of you; they
//                            attack you (idle: they stand at your feet until engaged; hpF: health x F, e.g. hp1.6) and
//                            are gone after 5 minutes or when dead (creature Emberfall.TestTools.FoeEntry)
//   .eftest dummy [L]        a training dummy of level L: it stands still, never hits back and never dies (it keeps 1
//                            health); gone after 10 minutes (creature Emberfall.TestTools.DummyEntry)
//   .eftest clear            your foes and dummies gone
//   .eftest refresh          full health and mana, your spell cooldowns ended
//   .eftest power N          your spec power at N points (0 too), its maximum untouched (`.modify runicpower` sets the
//                            slot's maximum as well: the Earthwarden's 3 Seismic Charges would be capped at N)
//
// Config: Emberfall.TestTools.Enable (0), .Security (0: any account; the commands are registered for players and
// refuse an account below it), .FoeEntry and .DummyEntry (0: that command is off; Emberfall's own data names its
// creatures, content/data/creatures.yaml). A summon's level comes from the AllCreatureScript hook before the core picks
// a level from the template, so its health, damage and armour are the creature_classlevelstats row of that level.

#include "Chat.h"
#include "ChatCommand.h"
#include "CommandScript.h"
#include "Config.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "Player.h"
#include "PlayerScript.h"
#include "ScriptMgr.h"
#include "TemporarySummon.h"
#include "UnitScript.h"
#include "AllCreatureScript.h"
#include "World.h"
#include "WorldScript.h"
#include "WorldSession.h"
#include "testzone/EfTestZone.h"
#include "classkit/EfClassKit.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <unordered_set>

using namespace Acore::ChatCommands;

namespace
{
    struct Config
    {
        bool enable = false;
        uint32 security = 0;
        uint32 foeEntry = 0;
        uint32 dummyEntry = 0;
    };

    Config s_cfg;
    // The level the next summon of ours gets (0: none); set and cleared around SummonCreature in the world thread.
    thread_local uint8 t_pendingLevel = 0;
    // Our dummies: they keep 1 health (UnitScript::OnDamage).
    std::unordered_set<ObjectGuid> s_dummies;
    // Our foes: they never leash back to where they were summoned while they fight (the core's CreatureLeashRadius counts
    // from the summon point; a foe placed with `idle` 30+ yards off would turn back once it goes 12 s without damage).
    std::unordered_set<ObjectGuid> s_foes;

    constexpr uint32 kFoeMs = 5 * MINUTE * IN_MILLISECONDS;
    constexpr uint32 kDummyMs = 10 * MINUTE * IN_MILLISECONDS;
    constexpr uint32 kDummyHealthFactor = 200;

    void ReadConfig()
    {
        s_cfg.enable = sConfigMgr->GetOption<bool>("Emberfall.TestTools.Enable", false, false);
        s_cfg.security = sConfigMgr->GetOption<uint32>("Emberfall.TestTools.Security", 0, false);
        s_cfg.foeEntry = sConfigMgr->GetOption<uint32>("Emberfall.TestTools.FoeEntry", 0, false);
        s_cfg.dummyEntry = sConfigMgr->GetOption<uint32>("Emberfall.TestTools.DummyEntry", 0, false);
    }

    class emberfall_testtools_world : public WorldScript
    {
    public:
        emberfall_testtools_world() : WorldScript("emberfall_testtools_world", { WORLDHOOK_ON_AFTER_CONFIG_LOAD }) { }

        void OnAfterConfigLoad(bool /*reload*/) override
        {
            ReadConfig();
            if (s_cfg.enable)
                LOG_INFO("server.loading", ">> Emberfall test tools: on (.eftest; security {}, foe {}, dummy {})", s_cfg.security,
                         s_cfg.foeEntry, s_cfg.dummyEntry);
        }
    };

    class emberfall_testtools_creature : public AllCreatureScript
    {
    public:
        emberfall_testtools_creature() : AllCreatureScript("emberfall_testtools_creature") { }

        void OnBeforeCreatureSelectLevel(CreatureTemplate const* /*cinfo*/, Creature* /*creature*/, uint8& level) override
        {
            if (t_pendingLevel)
                level = t_pendingLevel;
        }

        void OnCreatureRemoveWorld(Creature* creature) override
        {
            if (!s_dummies.empty())
                s_dummies.erase(creature->GetGUID());
            if (!s_foes.empty())
                s_foes.erase(creature->GetGUID());
        }

        void OnAllCreatureUpdate(Creature* creature, uint32 /*diff*/) override
        {
            if (!s_foes.empty() && creature->IsInCombat() && s_foes.count(creature->GetGUID()))
                creature->UpdateLeashExtensionTime();
        }
    };

    // A player who dies takes their foes and dummies along: they never go on to fight the townsfolk.
    class emberfall_testtools_player : public PlayerScript
    {
    public:
        emberfall_testtools_player() : PlayerScript("emberfall_testtools_player", { PLAYERHOOK_ON_PLAYER_JUST_DIED }) { }

        void OnPlayerJustDied(Player* player) override
        {
            if (s_foes.empty() && s_dummies.empty())
                return;
            std::list<Creature*> found;
            for (uint32 entry : { s_cfg.foeEntry, s_cfg.dummyEntry })
                if (entry)
                    player->GetCreatureListWithEntryInGrid(found, entry, 120.0f);
            for (Creature* c : found)
                // never a companion gentled from a proving wolf (a hunter's pet is a summon of its player too)
                if (TempSummon* summon = c->ToTempSummon(); summon && !summon->IsPet() && summon->GetSummonerGUID() == player->GetGUID())
                    summon->UnSummon();
        }
    };

    class emberfall_testtools_unit : public UnitScript
    {
    public:
        emberfall_testtools_unit() : UnitScript("emberfall_testtools_unit", true, { UNITHOOK_ON_DAMAGE }) { }

        void OnDamage(Unit* /*attacker*/, Unit* victim, uint32& damage) override
        {
            if (s_dummies.empty() || !victim || !s_dummies.count(victim->GetGUID()))
                return;
            uint32 const health = victim->GetHealth();
            if (damage >= health)
                damage = health > 1 ? health - 1 : 0;
        }
    };

    class emberfall_testtools_commands : public CommandScript
    {
    public:
        emberfall_testtools_commands() : CommandScript("emberfall_testtools_commands") { }

        ChatCommandTable GetCommands() const override
        {
            static ChatCommandTable testTable =
            {
                { "level",   HandleLevel,   SEC_PLAYER, Console::No },
                { "zone",    HandleZone,    SEC_PLAYER, Console::No },
                { "foe",     HandleFoe,     SEC_PLAYER, Console::No },
                { "dummy",   HandleDummy,   SEC_PLAYER, Console::No },
                { "clear",   HandleClear,   SEC_PLAYER, Console::No },
                { "refresh", HandleRefresh, SEC_PLAYER, Console::No },
                { "power",   HandlePower,   SEC_PLAYER, Console::No },
                { "",        HandleList,    SEC_PLAYER, Console::No },
            };
            static ChatCommandTable commandTable =
            {
                { "eftest", testTable },
            };
            return commandTable;
        }

        // The player's own character, if the tools are on and the account may use them.
        static Player* Self(ChatHandler* handler)
        {
            WorldSession* session = handler->GetSession();
            Player* player = session ? session->GetPlayer() : nullptr;
            if (!player)
                return nullptr;
            if (!s_cfg.enable)
            {
                handler->SendSysMessage("The test tools are off on this realm (Emberfall.TestTools.Enable).");
                handler->SetSentErrorMessage(true);
                return nullptr;
            }
            if (uint32(session->GetSecurity()) < s_cfg.security)
            {
                handler->SendSysMessage("You may not use .eftest (Emberfall.TestTools.Security).");
                handler->SetSentErrorMessage(true);
                return nullptr;
            }
            return player;
        }

        static uint8 MaxLevel() { return uint8(std::clamp<uint32>(sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL), 1, 255)); }

        static bool HandleList(ChatHandler* handler)
        {
            if (!Self(handler))
                return false;
            handler->SendSysMessage("Emberfall test tools: .eftest level N | zone <zone> [level] | zone list | foe [level] [count] [idle] [hpF] | dummy [level] | clear | refresh | power N");
            return true;
        }

        static bool HandleLevel(ChatHandler* handler, uint32 level)
        {
            Player* player = Self(handler);
            if (!player)
                return false;
            if (level < 1 || level > MaxLevel())
            {
                handler->PSendSysMessage("The level is 1 to {}.", MaxLevel());
                handler->SetSentErrorMessage(true);
                return false;
            }
            if (player->IsInCombat())
            {
                handler->SendSysMessage("Not in combat.");
                handler->SetSentErrorMessage(true);
                return false;
            }
            Emberfall::TestZone::SetLevel(player, uint8(level));
            handler->PSendSysMessage("EFTEST level {}", player->GetLevel());
            return true;
        }

        // `.eftest zone <zone> [level]`, `.eftest zone list` (src/emberfall_testzone.cpp)
        static bool HandleZone(ChatHandler* handler, Optional<std::string> zone, Optional<uint32> level)
        {
            Player* player = Self(handler);
            if (!player)
                return false;
            if (!zone || *zone == "list")
                return Emberfall::TestZone::List(handler);
            if (player->IsInCombat() || !player->IsAlive() || player->IsInFlight())
            {
                handler->SendSysMessage("Not in combat, dead or in flight.");
                handler->SetSentErrorMessage(true);
                return false;
            }
            if (level && (*level < 1 || *level > MaxLevel()))
            {
                handler->PSendSysMessage("The level is 1 to {}.", MaxLevel());
                handler->SetSentErrorMessage(true);
                return false;
            }
            return Emberfall::TestZone::Arrive(handler, player, *zone, level.value_or(0));
        }

        static Creature* Summon(Player* player, uint32 entry, uint8 level, float distance, float angle, uint32 ms)
        {
            float x, y, z;
            player->GetClosePoint(x, y, z, player->GetCombatReach(), distance, angle);
            t_pendingLevel = level;
            TempSummon* summon = player->SummonCreature(entry, x, y, z, player->GetOrientation() + float(M_PI),
                                                        TEMPSUMMON_TIMED_OR_DEAD_DESPAWN, ms);
            t_pendingLevel = 0;
            return summon;
        }

        static uint8 LevelArg(Player* player, Optional<uint32> level)
        {
            return uint8(std::clamp<uint32>(level.value_or(player->GetLevel()), 1, std::max<uint32>(MaxLevel(), 63)));
        }

        // `.eftest foe [level] [count] [idle] [hpF]`: `idle` leaves them standing where you are until something engages
        // them (for scripted checks that place a foe and walk away first, as `.npc add temp` does for a GM); `hpF` (e.g.
        // hp1.6) multiplies their health, as a creature template's HealthModifier would (0.1 - 20)
        static bool HandleFoe(ChatHandler* handler, Optional<uint32> level, Optional<uint32> count, Tail mode)
        {
            Player* player = Self(handler);
            if (!player)
                return false;
            if (!s_cfg.foeEntry)
            {
                handler->SendSysMessage("No foe on this realm (Emberfall.TestTools.FoeEntry).");
                handler->SetSentErrorMessage(true);
                return false;
            }
            std::string_view const rest(mode);
            bool const idle = rest.find("idle") != std::string_view::npos;
            float hp = 1.0f;
            if (std::size_t const at = rest.find("hp"); at != std::string_view::npos)
                hp = std::clamp(float(std::atof(std::string(rest.substr(at + 2)).c_str())), 0.1f, 20.0f);
            uint8 const lvl = LevelArg(player, level);
            uint32 const n = std::clamp<uint32>(count.value_or(1), 1, 5);
            uint32 made = 0;
            for (uint32 i = 0; i < n; ++i)
            {
                // a fan 10 yd ahead (idle: at your feet)
                float const angle = n == 1 ? 0.0f : (float(i) / float(n - 1) - 0.5f) * float(M_PI) * 0.5f;
                if (Creature* foe = Summon(player, s_cfg.foeEntry, lvl, idle ? 0.5f : 10.0f, angle, kFoeMs))
                {
                    ++made;
                    s_foes.insert(foe->GetGUID());
                    if (hp != 1.0f)
                    {
                        uint32 const health = std::max<uint32>(1, uint32(float(foe->GetMaxHealth()) * hp));
                        foe->SetCreateHealth(health);
                        foe->SetStatFlatModifier(UNIT_MOD_HEALTH, BASE_VALUE, float(health));
                        foe->SetMaxHealth(health);
                        foe->SetFullHealth();
                    }
                    if (!idle && foe->IsAIEnabled && foe->AI())
                        foe->AI()->AttackStart(player);
                }
            }
            if (!made)
            {
                handler->SendSysMessage("No foe could be summoned here.");
                handler->SetSentErrorMessage(true);
                return false;
            }
            handler->PSendSysMessage("EFTEST foe {} level {} health x{:.2f}", made, lvl, hp);
            return true;
        }

        static bool HandleDummy(ChatHandler* handler, Optional<uint32> level)
        {
            Player* player = Self(handler);
            if (!player)
                return false;
            if (!s_cfg.dummyEntry)
            {
                handler->SendSysMessage("No training dummy on this realm (Emberfall.TestTools.DummyEntry).");
                handler->SetSentErrorMessage(true);
                return false;
            }
            uint8 const lvl = LevelArg(player, level);
            Creature* dummy = Summon(player, s_cfg.dummyEntry, lvl, 8.0f, 0.0f, kDummyMs);
            if (!dummy)
            {
                handler->SendSysMessage("No dummy could be summoned here.");
                handler->SetSentErrorMessage(true);
                return false;
            }
            dummy->SetReactState(REACT_PASSIVE);
            dummy->SetControlled(true, UNIT_STATE_ROOT);
            dummy->SetRegeneratingHealth(false);
            uint32 const health = static_cast<uint32>(std::min<uint64>(uint64(dummy->GetMaxHealth()) * kDummyHealthFactor, 0x7FFFFFFF));
            dummy->SetCreateHealth(health);
            dummy->SetMaxHealth(health);
            dummy->SetFullHealth();
            s_dummies.insert(dummy->GetGUID());
            handler->PSendSysMessage("EFTEST dummy level {} health {}", lvl, health);
            return true;
        }

        static bool HandleClear(ChatHandler* handler)
        {
            Player* player = Self(handler);
            if (!player)
                return false;
            std::list<Creature*> found;
            for (uint32 entry : { s_cfg.foeEntry, s_cfg.dummyEntry })
                if (entry)
                    player->GetCreatureListWithEntryInGrid(found, entry, 100.0f);
            uint32 gone = 0;
            for (Creature* c : found)
            {
                TempSummon* summon = c->ToTempSummon();
                // never a companion gentled from a proving wolf (a hunter's pet is a summon of its player too)
                if (!summon || summon->IsPet() || summon->GetSummonerGUID() != player->GetGUID())
                    continue;
                summon->UnSummon();
                ++gone;
            }
            handler->PSendSysMessage("EFTEST clear {}", gone);
            return true;
        }

        static bool HandlePower(ChatHandler* handler, uint32 points)
        {
            Player* player = Self(handler);
            if (!player)
                return false;
            Emberfall::ClassKit::SetSpecPowerPoints(player, int32(points));
            handler->PSendSysMessage("EFTEST power {}", Emberfall::ClassKit::SpecPowerPoints(player));
            return true;
        }

        static bool HandleRefresh(ChatHandler* handler)
        {
            Player* player = Self(handler);
            if (!player)
                return false;
            player->SetFullHealth();
            player->SetPower(POWER_MANA, player->GetMaxPower(POWER_MANA));
            player->RemoveAllSpellCooldown();
            handler->SendSysMessage("EFTEST refresh");
            return true;
        }
    };
}

void AddEmberfallTestToolsScripts()
{
    new emberfall_testtools_world();
    new emberfall_testtools_creature();
    new emberfall_testtools_unit();
    new emberfall_testtools_player();
    new emberfall_testtools_commands();
}
