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

// Emberfall's dungeons (Emberfall docs/design/dungeon-barrows.md): the Barrows of Cairnmoor's instance script and the
// test realm's dungeon warden.
//
//   instance_ef_barrows      map 751 (instance_template.script): the three seals. The barrow bosses report their deaths
//                            with SmartAI set_inst_data 1-3 (content/data/barrows/creatures.yaml); when all three are
//                            broken the First Interred (97015) wakes (selectable, no longer immune to players, its yell)
//                            and summons its honour guard (97008, three) in the Honour Hall, who come up the vault
//                            stair at the group. The seals are saved with the instance.
//   npc_ef_dungeon_warden    the dev warden (97050 in Millbrook, 97051 inside the mound door): "Take me to the Barrows
//                            of Cairnmoor", "Take me back to Millbrook", "Take me to the mound door on Cairnmoor",
//                            "Make me level 60", "Reset the Barrows of Cairnmoor". Only with Emberfall.TestTools.Enable
//                            on (the test tools' switch, emberfall_testtools.cpp): off, the warden leaves the world on its
//                            first update and on every respawn, and its gossip does nothing, so a live realm never shows
//                            it. The menu and the gating are src/dungeons/EfDungeonLogic.h (tests/dungeon_tests.cpp).

#include "Chat.h"
#include "Config.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "CreatureScript.h"
#include "Group.h"
#include "InstanceMapScript.h"
#include "InstanceScript.h"
#include "Log.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptedGossip.h"
#include "ScriptMgr.h"
#include "TemporarySummon.h"
#include "World.h"

#include "EfDungeonLogic.h"

#include <algorithm>
#include <sstream>

using namespace Emberfall::Dungeons;

namespace
{
    // The test tools' switch, read when asked (a config reload takes effect at once).
    bool TestToolsOn()
    {
        return sConfigMgr->GetOption<bool>("Emberfall.TestTools.Enable", false, false);
    }

    uint8 MaxLevel() { return uint8(std::clamp<uint32>(sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL), 1, 255)); }

    // --- the barrows ------------------------------------------------------------------------------------------------
    class instance_ef_barrows : public InstanceMapScript
    {
    public:
        instance_ef_barrows() : InstanceMapScript("instance_ef_barrows", kBarrowsMap) { }

        struct instance_ef_barrows_InstanceScript : public InstanceScript
        {
            explicit instance_ef_barrows_InstanceScript(Map* map) : InstanceScript(map) { SetHeaders("EFB"); }

            void OnCreatureCreate(Creature* creature) override
            {
                if (creature->GetEntry() != kFirstInterred)
                    return;
                _interred = creature->GetGUID();
                if (AllSealsBroken(_seals))   // a saved instance whose seals are already broken: awake, no second wave
                    Wake(creature, false);
            }

            void SetData(uint32 type, uint32 data) override
            {
                if (type == DATA_FIRST_INTERRED || type == DATA_POST_KEEPER)
                {
                    if (data == kEncounterDone)
                    {
                        (type == DATA_FIRST_INTERRED ? _interredDone : _keeperDone) = true;
                        LOG_INFO("module", "Emberfall barrows: {} dead (instance {})",
                                 type == DATA_FIRST_INTERRED ? "the First Interred" : "the Post-Keeper", instance->GetInstanceId());
                        SaveToDB();
                    }
                    return;
                }
                SealResult const r = BreakSeal(_seals, type, data);
                if (r.mask == _seals)
                    return;
                _seals = r.mask;
                LOG_INFO("module", "Emberfall barrows: seal {} broken, {} of 3 (instance {})", type, SealsBroken(_seals),
                         instance->GetInstanceId());
                SaveToDB();
                if (r.wakes)
                    if (Creature* interred = instance->GetCreature(_interred))
                        Wake(interred, true);
            }

            uint32 GetData(uint32 type) const override
            {
                switch (type)
                {
                    case DATA_SEALS_BROKEN: return SealsBroken(_seals);
                    case DATA_FIRST_INTERRED: return _interredDone ? kEncounterDone : 0;
                    case DATA_POST_KEEPER: return _keeperDone ? kEncounterDone : 0;
                    default: return SealBit(type) && (_seals & SealBit(type)) ? kEncounterDone : 0;
                }
            }

            void ReadSaveDataMore(std::istringstream& data) override
            {
                uint32 seals = 0, interred = 0, keeper = 0;
                data >> seals >> interred >> keeper;
                _seals = seals & 7u;
                _interredDone = interred != 0;
                _keeperDone = keeper != 0;
            }

            void WriteSaveDataMore(std::ostringstream& data) override
            {
                data << _seals << ' ' << (_interredDone ? 1 : 0) << ' ' << (_keeperDone ? 1 : 0);
            }

        private:
            void Wake(Creature* interred, bool wave)
            {
                if (!interred->IsAlive())
                    return;
                interred->RemoveUnitFlag(UNIT_FLAG_NOT_SELECTABLE);
                interred->SetImmuneToPC(false);
                LOG_INFO("module", "Emberfall barrows: the seals are broken, the First Interred wakes (instance {}{})",
                         instance->GetInstanceId(), wave ? ", its honour guard comes" : "");
                if (!wave)
                    return;
                if (interred->IsAIEnabled && interred->AI())
                    interred->AI()->Talk(0);
                for (Spot const& s : { kHonourGuard1, kHonourGuard2, kHonourGuard3 })
                    if (TempSummon* guard = interred->SummonCreature(kHonourGuard, s.x, s.y, s.z, s.o, TEMPSUMMON_CORPSE_TIMED_DESPAWN, 300000))
                        guard->SetInCombatWithZone();
            }

            uint32 _seals = 0;
            bool _interredDone = false, _keeperDone = false;
            ObjectGuid _interred;
        };

        InstanceScript* GetInstanceScript(InstanceMap* map) const override
        {
            return new instance_ef_barrows_InstanceScript(map);
        }
    };

    // --- the dev warden ---------------------------------------------------------------------------------------------
    constexpr uint32 kActionBase = 1000;
    constexpr Seconds kAwayRespawn = Seconds(DAY);

    class npc_ef_dungeon_warden : public CreatureScript
    {
    public:
        npc_ef_dungeon_warden() : CreatureScript("npc_ef_dungeon_warden") { }

        struct npc_ef_dungeon_wardenAI : public CreatureAI
        {
            explicit npc_ef_dungeon_wardenAI(Creature* creature) : CreatureAI(creature) { }

            // Off: out of the world at once (and again on every respawn, a day later); on: an ordinary passive NPC.
            void UpdateAI(uint32 /*diff*/) override
            {
                if (WardenStays(TestToolsOn()))
                    return;
                LOG_INFO("module", "Emberfall dungeon warden {}: the test tools are off (Emberfall.TestTools.Enable), "
                         "it leaves the world", me->GetSpawnId());
                me->ReplaceAllNpcFlags(UNIT_NPC_FLAG_NONE);
                me->SetVisible(false);
                me->DespawnOrUnsummon(0ms, kAwayRespawn);
            }

            void AttackStart(Unit* /*who*/) override { }
            void MoveInLineOfSight(Unit* /*who*/) override { }
        };

        CreatureAI* GetAI(Creature* creature) const override { return new npc_ef_dungeon_wardenAI(creature); }

        static WardenAsk Ask(Player* player)
        {
            WardenAsk a;
            a.testToolsOn = TestToolsOn();
            a.map = player->GetMapId();
            a.level = player->GetLevel();
            a.maxLevel = MaxLevel();
            a.inCombat = player->IsInCombat();
            return a;
        }

        bool OnGossipHello(Player* player, Creature* creature) override
        {
            ClearGossipMenuFor(player);
            WardenAsk const ask = Ask(player);
            if (!ask.testToolsOn)
            {
                CloseGossipMenuFor(player);   // a live realm: nothing, ever
                return true;
            }
            for (WardenOption o : WardenOptions(ask))
                AddGossipItemFor(player, GOSSIP_ICON_CHAT, WardenOptionText(o), GOSSIP_SENDER_MAIN, kActionBase + uint32(o));
            SendGossipMenuFor(player, creature->GetEntry(), creature->GetGUID());   // npc_text = the warden's entry
            return true;
        }

        bool OnGossipSelect(Player* player, Creature* /*creature*/, uint32 /*sender*/, uint32 action) override
        {
            CloseGossipMenuFor(player);
            WardenAsk const ask = Ask(player);
            WardenOption const o = WardenOption(action - kActionBase);
            if (!Offers(ask, o))   // tools off, in combat, or a stale menu: nothing
                return true;
            switch (o)
            {
                case WardenOption::ToBarrows:
                    if (player->GetLevel() < kBarrowsMinLevel)
                    {
                        ChatHandler(player->GetSession()).PSendSysMessage("The Barrows of Cairnmoor admit level {} and above: "
                                                                          "ask me for level 60 first.", kBarrowsMinLevel);
                        return true;
                    }
                    player->TeleportTo(kBarrowsInside.map, kBarrowsInside.x, kBarrowsInside.y, kBarrowsInside.z, kBarrowsInside.o);
                    break;
                case WardenOption::ToDoor:
                    player->TeleportTo(kBarrowsDoor.map, kBarrowsDoor.x, kBarrowsDoor.y, kBarrowsDoor.z, kBarrowsDoor.o);
                    break;
                case WardenOption::ToMillbrook:
                    if (GameTele const* tele = sObjectMgr->GetGameTele(kMillbrookTele, true))
                        player->TeleportTo(tele->mapId, tele->position_x, tele->position_y, tele->position_z, tele->orientation);
                    else
                        player->TeleportTo(kMillbrook.map, kMillbrook.x, kMillbrook.y, kMillbrook.z, kMillbrook.o);
                    break;
                case WardenOption::Level60:
                {
                    uint8 const level = std::min(kTestLevel, MaxLevel());
                    if (player->GetLevel() < level)
                    {
                        player->GiveLevel(level);
                        player->InitTalentForLevel();
                    }
                    player->SetUInt32Value(PLAYER_XP, 0);
                    player->SetFullHealth();
                    player->SetPower(POWER_MANA, player->GetMaxPower(POWER_MANA));
                    ChatHandler(player->GetSession()).PSendSysMessage("EFWARDEN level {}", player->GetLevel());
                    break;
                }
                case WardenOption::ResetBarrows:
                    if (Group* group = player->GetGroup(); group && group->IsLeader(player->GetGUID()))
                        group->ResetInstances(INSTANCE_RESET_ALL, false, player);
                    else
                        Player::ResetInstances(player->GetGUID(), INSTANCE_RESET_ALL, false);
                    ChatHandler(player->GetSession()).SendSysMessage("EFWARDEN reset");
                    break;
            }
            return true;
        }
    };
}

void AddEmberfallDungeonsScripts()
{
    new instance_ef_barrows();
    new npc_ef_dungeon_warden();
}
