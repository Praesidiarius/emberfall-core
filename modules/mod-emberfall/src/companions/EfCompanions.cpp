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

#include "EfCompanions.h"

#include "Chat.h"
#include "Config.h"
#include "Creature.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "Group.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "TemporarySummon.h"
#include "ThreatManager.h"
#include "WorldPacket.h"

#include <algorithm>
#include <mutex>
#include <set>

namespace Emberfall::Companions
{
    namespace
    {
        constexpr char const* kRosterKey = "ef_companions";
        constexpr uint32 kSummonProperties = 61; // SummonProperties: ally, guardian, slot 0 (several at once)
        constexpr char const* kAddonPrefix = "EFC\t";

        struct Data
        {
            std::map<uint8, Archetype> archetypes;
        };

        Config s_cfg;
        std::mutex s_dataLock;
        std::shared_ptr<Data const> s_data = std::make_shared<Data>();

        std::shared_ptr<Data const> GetData()
        {
            std::lock_guard<std::mutex> lock(s_dataLock);
            return s_data;
        }

        bool TablesExist()
        {
            QueryResult r = WorldDatabase.Query("SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() "
                                                "AND TABLE_NAME IN ('ef_companion_archetype', 'ef_companion_spell')");
            return r && r->Fetch()[0].Get<uint64>() == 2;
        }

        float Pct(uint32 cur, uint32 max) { return max ? 100.0f * float(cur) / float(max) : 0.0f; }

        std::string Label(Player* owner, Unit const* u)
        {
            if (u == owner)
                return "owner";
            if (Creature const* c = u->ToCreature())
                if (CompanionAIBase* ai = AIOf(const_cast<Creature*>(c)))
                    if (Archetype const* a = FindArchetype(ai->ArchetypeId()))
                        return a->name;
            return u->GetName();
        }

        Creature* Summon(Player* owner, Archetype const& arch, uint32 index, Position const* at)
        {
            SummonPropertiesEntry const* props = sSummonPropertiesStore.LookupEntry(kSummonProperties);
            if (!props || !sObjectMgr->GetCreatureTemplate(arch.entry))
                return nullptr;
            Position pos;
            if (at)
                pos = *at;
            else
                pos = FormationPoint(owner, index);
            TempSummon* summon = owner->SummonCreature(arch.entry, pos, TEMPSUMMON_MANUAL_DESPAWN, 0, 0, props);
            if (!summon)
                return nullptr;
            ApplyScaling(summon, arch, owner->GetLevel());
            if (CompanionAIBase* ai = AIOf(summon))
                ai->Configure(&arch, index, owner->GetGUID());
            return summon;
        }

        Member* FindMember(Roster* r, ObjectGuid guid)
        {
            if (!r)
                return nullptr;
            for (Member& m : r->members)
                if (m.guid == guid)
                    return &m;
            return nullptr;
        }

        void SendFrame(Player* owner, std::string const& body)
        {
            WorldPacket data;
            ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, owner, owner, std::string(kAddonPrefix) + body);
            owner->SendDirectMessage(&data);
        }

        void Track(Player* owner, Roster& r, uint32 dt)
        {
            Unit* tank = Tank(owner);
            std::vector<Unit*> const enemies = EngagedEnemies(owner);
            bool const fight = !enemies.empty();
            if (fight && !r.inFight)
            {
                r.inFight = true;
                r.fightStartMs = r.clockMs;
                ++r.stats.fights;
            }
            else if (!fight && r.inFight)
            {
                r.inFight = false;
                r.stats.fightMs += r.clockMs - r.fightStartMs;
            }
            std::set<ObjectGuid> seen;
            for (Unit* mob : enemies)
            {
                MobTrack& t = r.mobs[mob->GetGUID()];
                if (!t.firstSeenMs)
                {
                    t.firstSeenMs = std::max<uint32>(r.clockMs, 1);
                    t.name = mob->GetName();
                }
                seen.insert(mob->GetGUID());
                Unit* victim = mob->GetVictim();
                r.stats.mobMs += dt;
                if (tank && victim == tank)
                {
                    t.tankHeldMs += dt;
                    r.stats.mobMsOnTank += dt;
                    // The tank's lead: its threat over the next-highest threat on the mob.
                    float const mine = mob->GetThreatMgr().GetThreat(tank);
                    float second = 0;
                    for (ThreatReference const* ref : mob->GetThreatMgr().GetUnsortedThreatList())
                        if (ref->GetVictim() != tank)
                            second = std::max(second, ref->GetThreat());
                    if (second > 0 && mine > 0)
                    {
                        float const lead = mine / second;
                        r.stats.minLead = r.stats.leadSamples ? std::min(r.stats.minLead, lead) : lead;
                        r.stats.leadSum += lead;
                        ++r.stats.leadSamples;
                    }
                }
                else
                {
                    if (tank && t.lastVictim == tank->GetGUID() && victim && InGroup(owner, victim))
                    {
                        ++r.stats.aggroLosses;
                        ++r.stats.lossesTo[Label(owner, victim)];
                    }
                    t.tankHeldMs = 0;
                }
                t.lastVictim = victim ? victim->GetGUID() : ObjectGuid::Empty;
            }
            for (auto it = r.mobs.begin(); it != r.mobs.end();)
            {
                if (seen.count(it->first))
                {
                    ++it;
                    continue;
                }
                Creature* c = ObjectAccessor::GetCreature(*owner, it->first);
                if (c && c->isDead())
                {
                    ++r.stats.kills;
                    r.stats.killMsSum += r.clockMs - it->second.firstSeenMs;
                    LOG_INFO("module", "Emberfall companions: {} killed {} after {:.1f} s", owner->GetName(), it->second.name,
                             float(r.clockMs - it->second.firstSeenMs) / 1000.0f);
                }
                it = r.mobs.erase(it);
            }
            if (fight)
                for (Creature* c : LiveCompanions(owner))
                    if (CompanionAIBase* ai = AIOf(c))
                        if (Archetype const* a = FindArchetype(ai->ArchetypeId()); a && a->role == Role::Healer && c->GetMaxPower(POWER_MANA))
                            r.stats.healerManaMin = std::min(r.stats.healerManaMin, Pct(c->GetPower(POWER_MANA), c->GetMaxPower(POWER_MANA)));
        }

        void PushFrame(Player* owner, Roster& r, bool force)
        {
            std::vector<FrameEntry> entries;
            for (Member const& m : r.members)
            {
                FrameEntry e;
                Archetype const* a = FindArchetype(m.archetype);
                e.role = a ? a->role : Role::None;
                e.archetype = a ? a->name : "?";
                Creature* c = ObjectAccessor::GetCreature(*owner, m.guid);
                e.guid = m.guid.GetRawValue();
                if (m.dead || !c || !c->IsAlive())
                {
                    e.state = 'D';
                    e.hpPct = 0;
                    e.manaPct = a && a->usesMana ? 0 : -1;
                }
                else
                {
                    e.hpPct = uint32(std::ceil(Pct(c->GetHealth(), c->GetMaxHealth())));
                    e.manaPct = c->GetMaxPower(POWER_MANA) ? int32(Pct(c->GetPower(POWER_MANA), c->GetMaxPower(POWER_MANA))) : -1;
                    CompanionAIBase* ai = AIOf(c);
                    e.state = ai ? ai->FrameState() : 'F';
                }
                entries.push_back(e);
            }
            std::string const body = FrameBody(entries);
            if (!force && body == r.lastFrame)
                return;
            r.lastFrame = body;
            r.frameHeartbeat = 0;
            SendFrame(owner, body);
        }
    }

    Position FormationPoint(Player* owner, uint32 index)
    {
        // The owner's own height, not the ground's: in an instance without vmaps (the profiles run without them) the
        // map height under a WMO-only dungeon is no floor, and a summon there falls through the world.
        Slot const s = FormationSlot(index);
        float const angle = owner->GetOrientation() + s.angle;
        float const x = owner->GetPositionX() + std::cos(angle) * s.dist;
        float const y = owner->GetPositionY() + std::sin(angle) * s.dist;
        float const z = owner->GetPositionZ() + 0.5f;
        if (!owner->IsWithinLOS(x, y, z))
            return Position(owner->GetPositionX(), owner->GetPositionY(), z, owner->GetOrientation());
        return Position(x, y, z, owner->GetOrientation());
    }

    // ---- kinds ---------------------------------------------------------------------------------------------------
    Kind ParseKind(std::string const& s)
    {
        static std::map<std::string, Kind> const kinds = {
            { "taunt", Kind::Taunt }, { "aoe_threat", Kind::AoeThreat }, { "threat", Kind::Threat }, { "nuke", Kind::Nuke },
            { "aoe", Kind::Aoe }, { "behind", Kind::Behind }, { "shoot", Kind::Shoot }, { "dot", Kind::Dot },
            { "heal_fast", Kind::HealFast }, { "heal_big", Kind::HealBig }, { "heal_small", Kind::HealSmall }, { "hot", Kind::Hot },
            { "shield", Kind::Shield }, { "dispel", Kind::Dispel }, { "res", Kind::Res },
        };
        auto it = kinds.find(Lower(s));
        return it == kinds.end() ? Kind::None : it->second;
    }

    char const* KindName(Kind k)
    {
        switch (k)
        {
            case Kind::Taunt: return "taunt";
            case Kind::AoeThreat: return "aoe_threat";
            case Kind::Threat: return "threat";
            case Kind::Nuke: return "nuke";
            case Kind::Aoe: return "aoe";
            case Kind::Behind: return "behind";
            case Kind::Shoot: return "shoot";
            case Kind::Dot: return "dot";
            case Kind::HealFast: return "heal_fast";
            case Kind::HealBig: return "heal_big";
            case Kind::HealSmall: return "heal_small";
            case Kind::Hot: return "hot";
            case Kind::Shield: return "shield";
            case Kind::Dispel: return "dispel";
            case Kind::Res: return "res";
            default: return "none";
        }
    }

    // ---- config and data --------------------------------------------------------------------------------------------
    void ReadConfig(bool /*reload*/)
    {
        Config c;
        c.enable = sConfigMgr->GetOption<bool>("Emberfall.Companions.Enable", true, false);
        c.security = sConfigMgr->GetOption<uint32>("Emberfall.Companions.Security", 2, false);
        c.maxPerPlayer = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Emberfall.Companions.MaxPerPlayer", 8, false), 1, 16);
        c.gate.waitMs = sConfigMgr->GetOption<uint32>("Emberfall.Companions.DpsWaitMs", 2000, false);
        c.gate.waitThreat = sConfigMgr->GetOption<float>("Emberfall.Companions.DpsWaitThreat", 250.0f, false);
        c.gate.threatCap = sConfigMgr->GetOption<float>("Emberfall.Companions.ThreatCap", 0.80f, false);
        c.leash = sConfigMgr->GetOption<float>("Emberfall.Companions.Leash", 40.0f, false);
        c.frameMs = std::max<uint32>(sConfigMgr->GetOption<uint32>("Emberfall.Companions.FrameMs", 500, false), 100);
        c.noProximityAggro = sConfigMgr->GetOption<bool>("Emberfall.Companions.NoProximityAggro", true, false);
        s_cfg = c;
    }

    Config const& Cfg() { return s_cfg; }

    bool Enabled() { return s_cfg.enable && !GetData()->archetypes.empty(); }

    bool Load()
    {
        auto data = std::make_shared<Data>();
        if (!s_cfg.enable)
        {
            LOG_INFO("server.loading", ">> Emberfall companions: off (Emberfall.Companions.Enable = 0)");
            std::lock_guard<std::mutex> lock(s_dataLock);
            s_data = data;
            return false;
        }
        if (!TablesExist())
        {
            LOG_ERROR("server.loading", ">> Emberfall companions: the ef_companion tables are missing (modules/mod-emberfall/data/sql/db-world); "
                                        "the companions stay off");
            std::lock_guard<std::mutex> lock(s_dataLock);
            s_data = data;
            return false;
        }
        uint32 problems = 0;
        if (QueryResult r = WorldDatabase.Query("SELECT `id`, `name`, `role`, `entry`, `unit_class`, `health_mod`, `mana_mod`, `armor_mod`, "
                                                "`damage_mod`, `ranged_mod`, `attack_ms`, `threat_mod`, `free_cost`, `uses_mana`, `range` "
                                                "FROM `ef_companion_archetype` ORDER BY `id`"))
        {
            do
            {
                Field* f = r->Fetch();
                Archetype a;
                a.id = f[0].Get<uint8>();
                a.name = Lower(f[1].Get<std::string>());
                a.role = Role(f[2].Get<uint8>());
                a.entry = f[3].Get<uint32>();
                a.unitClass = f[4].Get<uint8>();
                a.mods.health = f[5].Get<float>();
                a.mods.mana = f[6].Get<float>();
                a.mods.armor = f[7].Get<float>();
                a.mods.damage = f[8].Get<float>();
                a.mods.rangedDamage = f[9].Get<float>();
                a.mods.attackMs = f[10].Get<uint32>();
                a.threatMod = f[11].Get<float>();
                a.freeCost = f[12].Get<uint8>() != 0;
                a.usesMana = f[13].Get<uint8>() != 0;
                a.range = f[14].Get<float>();
                CreatureTemplate const* ct = sObjectMgr->GetCreatureTemplate(a.entry);
                if (a.role == Role::None || uint8(a.role) > uint8(Role::Ranged))
                {
                    LOG_ERROR("server.loading", ">> Emberfall companions: archetype {} ({}) has no valid role ({})", a.id, a.name, uint32(a.role));
                    ++problems;
                    continue;
                }
                if (!ct)
                {
                    LOG_ERROR("server.loading", ">> Emberfall companions: archetype {} ({}): creature_template {} does not exist", a.id, a.name, a.entry);
                    ++problems;
                    continue;
                }
                if (!sObjectMgr->GetCreatureBaseStats(1, a.unitClass))
                {
                    LOG_ERROR("server.loading", ">> Emberfall companions: archetype {} ({}): no creature_classlevelstats for class {}", a.id, a.name, a.unitClass);
                    ++problems;
                    continue;
                }
                data->archetypes[a.id] = a;
            } while (r->NextRow());
        }
        if (QueryResult r = WorldDatabase.Query("SELECT `archetype`, `spell`, `kind`, `priority`, `min_level`, `max_level`, `cooldown_ms`, `min_targets` "
                                                "FROM `ef_companion_spell` ORDER BY `archetype`, `priority`, `min_level`"))
        {
            do
            {
                Field* f = r->Fetch();
                uint8 const arch = f[0].Get<uint8>();
                SpellRow row;
                row.spell = f[1].Get<uint32>();
                row.kind = ParseKind(f[2].Get<std::string>());
                row.priority = f[3].Get<uint8>();
                row.minLevel = f[4].Get<uint8>();
                row.maxLevel = f[5].Get<uint8>();
                row.cooldownMs = f[6].Get<uint32>();
                row.minTargets = f[7].Get<uint8>();
                auto it = data->archetypes.find(arch);
                if (it == data->archetypes.end())
                {
                    LOG_ERROR("server.loading", ">> Emberfall companions: ef_companion_spell names archetype {} without a (valid) archetype row", arch);
                    ++problems;
                    continue;
                }
                if (row.kind == Kind::None)
                {
                    LOG_ERROR("server.loading", ">> Emberfall companions: archetype {}: spell {} has an unknown kind '{}'", it->second.name, row.spell, f[2].Get<std::string>());
                    ++problems;
                    continue;
                }
                if (!sSpellMgr->GetSpellInfo(row.spell))
                {
                    LOG_ERROR("server.loading", ">> Emberfall companions: archetype {}: spell {} does not exist", it->second.name, row.spell);
                    ++problems;
                    continue;
                }
                it->second.spells.push_back(row);
            } while (r->NextRow());
        }
        std::string list;
        for (auto const& [id, a] : data->archetypes)
            list += Acore::StringFormat("{}{} ({}, {} spells)", list.empty() ? "" : ", ", a.name, RoleName(a.role), a.spells.size());
        if (data->archetypes.empty())
            LOG_INFO("server.loading", ">> Emberfall companions: no archetypes (ef_companion_archetype is empty): `.efparty` stays off");
        else
            LOG_INFO("server.loading", ">> Emberfall companions: {} archetypes: {}", data->archetypes.size(), list);
        if (problems)
            LOG_ERROR("server.loading", ">> Emberfall companions: {} problem(s) in the ef_companion tables (rows skipped)", problems);
        std::lock_guard<std::mutex> lock(s_dataLock);
        s_data = data;
        return !s_data->archetypes.empty();
    }

    Archetype const* FindArchetype(uint8 id)
    {
        // The pointer stays valid while the data is current; a `.efparty reload` swaps it between world updates of the
        // command's own map only, and the AIs copy their archetype (Configure).
        std::shared_ptr<Data const> d = GetData();
        auto it = d->archetypes.find(id);
        return it == d->archetypes.end() ? nullptr : &it->second;
    }

    Archetype const* FindArchetype(std::string const& name)
    {
        std::shared_ptr<Data const> d = GetData();
        std::string const n = Lower(name);
        for (auto const& [id, a] : d->archetypes)
            if (a.name == n)
                return &a;
        return nullptr;
    }

    std::vector<Archetype const*> Archetypes()
    {
        std::vector<Archetype const*> out;
        for (auto const& [id, a] : GetData()->archetypes)
            out.push_back(&a);
        return out;
    }

    uint32 RankFor(uint32 spell, uint8 level)
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spell);
        if (!info)
            return 0;
        std::vector<std::pair<uint32, uint32>> chain;
        for (SpellInfo const* s = info; s; s = s->GetNextRankSpell())
            chain.emplace_back(s->Id, s->SpellLevel);
        return RankForLevel(chain, level);
    }

    // ---- roster -----------------------------------------------------------------------------------------------------
    Roster* GetRoster(Player* owner, bool create)
    {
        return create ? owner->CustomData.GetDefault<Roster>(kRosterKey) : owner->CustomData.Get<Roster>(kRosterKey);
    }

    CompanionAIBase* AIOf(Creature* c) { return c ? dynamic_cast<CompanionAIBase*>(c->AI()) : nullptr; }

    Player* OwnerOf(Creature const* c)
    {
        if (!c)
            return nullptr;
        return ObjectAccessor::GetPlayer(*c, c->GetOwnerGUID());
    }

    std::vector<Creature*> LiveCompanions(Player* owner)
    {
        std::vector<Creature*> out;
        if (Roster* r = GetRoster(owner, false))
            for (Member const& m : r->members)
                if (Creature* c = ObjectAccessor::GetCreature(*owner, m.guid); c && c->IsAlive() && c->IsInWorld())
                    out.push_back(c);
        return out;
    }

    std::vector<Unit*> GroupUnits(Player* owner, float range)
    {
        std::vector<Unit*> out;
        out.push_back(owner);
        for (Creature* c : LiveCompanions(owner))
            out.push_back(c);
        if (::Group* grp = owner->GetGroup())
            for (GroupReference* itr = grp->GetFirstMember(); itr; itr = itr->next())
                if (Player* p = itr->GetSource(); p && p != owner && p->IsInMap(owner) && p->IsWithinDistInMap(owner, range))
                    out.push_back(p);
        return out;
    }

    bool InGroup(Player* owner, Unit const* u)
    {
        if (!u)
            return false;
        if (u == owner)
            return true;
        if (Creature const* c = u->ToCreature())
            return c->GetOwnerGUID() == owner->GetGUID() && FindMember(GetRoster(owner, false), c->GetGUID());
        if (Player const* p = u->ToPlayer())
            return owner->GetGroup() && p->GetGroup() == owner->GetGroup();
        return false;
    }

    Unit* Tank(Player* owner)
    {
        for (Creature* c : LiveCompanions(owner))
            if (CompanionAIBase* ai = AIOf(c))
                if (Archetype const* a = FindArchetype(ai->ArchetypeId()); a && a->role == Role::Tank)
                    return c;
        return owner->IsAlive() ? owner : nullptr;
    }

    bool OwnerIsTank(Player* owner) { return Tank(owner) == owner; }

    std::vector<Unit*> EngagedEnemies(Player* owner)
    {
        std::vector<Unit*> out;
        std::set<Unit*> seen;
        auto consider = [&](Unit* mob)
        {
            if (!mob || !mob->IsAlive() || !mob->IsInWorld() || mob->GetMap() != owner->GetMap() || !mob->ToCreature())
                return;
            if (InGroup(owner, mob) || !mob->IsWithinDistInMap(owner, 60.0f))
                return;
            if (seen.insert(mob).second)
                out.push_back(mob);
        };
        for (Unit* u : GroupUnits(owner))
        {
            for (auto const& [guid, ref] : u->GetThreatMgr().GetThreatenedByMeList())
                consider(ref->GetOwner());
            for (Unit* a : u->getAttackers())
                if (a && a->IsHostileTo(owner))
                    consider(a);
        }
        return out;
    }

    uint32 TankHeldMs(Player* owner, Unit const* mob)
    {
        Roster* r = GetRoster(owner, false);
        if (!r || !mob)
            return 0;
        auto it = r->mobs.find(mob->GetGUID());
        return it == r->mobs.end() ? 0 : it->second.tankHeldMs;
    }

    bool IsCrowdControlled(Unit const* mob)
    {
        // What damage would break (polymorph, sap, Stasis-like stuns, fears, disorients) and the classic hard CCs.
        // Roots are fair game (frost novas and the Frostguard's own slows).
        if (mob->HasBreakableByDamageAuraType(SPELL_AURA_MOD_CONFUSE) || mob->HasBreakableByDamageAuraType(SPELL_AURA_MOD_FEAR) ||
            mob->HasBreakableByDamageAuraType(SPELL_AURA_MOD_STUN) || mob->HasBreakableByDamageAuraType(SPELL_AURA_TRANSFORM))
            return true;
        uint64 const mask = (1ULL << MECHANIC_POLYMORPH) | (1ULL << MECHANIC_SAPPED) | (1ULL << MECHANIC_BANISH) |
                            (1ULL << MECHANIC_SLEEP) | (1ULL << MECHANIC_SHACKLE) | (1ULL << MECHANIC_HORROR);
        return mob->HasAuraWithMechanic(mask);
    }

    // ---- scaling ----------------------------------------------------------------------------------------------------
    void ApplyScaling(Creature* c, Archetype const& arch, uint8 level)
    {
        CreatureBaseStats const* b = sObjectMgr->GetCreatureBaseStats(level, arch.unitClass);
        CreatureBaseStats const* pal = sObjectMgr->GetCreatureBaseStats(level, CLASS_PALADIN);
        if (!b || !pal)
            return;
        uint8 const exp = level > 70 ? 2 : level > 60 ? 1 : 0;
        BaseStats bs;
        bs.health = b->BaseHealth[exp] > 1 ? b->BaseHealth[exp] : b->BaseHealth[0];
        bs.mana = b->BaseMana;
        bs.armor = b->BaseArmor;
        bs.damage = b->BaseDamage[exp] > 0 ? b->BaseDamage[exp] : b->BaseDamage[0];
        bs.attackPower = b->AttackPower;
        Scaled const s = Scale(bs, arch.mods, arch.usesMana ? pal->BaseMana : 0);

        c->SetLevel(level);
        // The stats the guardian formulas add on top stay at their create values: no bonus from them.
        for (uint8 i = STAT_STRENGTH; i < MAX_STATS; ++i)
            c->SetCreateStat(Stats(i), 10.0f);
        c->SetCreateHealth(s.health);
        c->SetStatFlatModifier(UNIT_MOD_HEALTH, BASE_VALUE, float(s.health));
        c->SetStatFlatModifier(UNIT_MOD_ARMOR, BASE_VALUE, float(s.armor));
        c->setPowerType(POWER_MANA);
        c->SetCreateMana(s.createMana);
        c->SetStatFlatModifier(UNIT_MOD_MANA, BASE_VALUE, float(s.mana));
        c->SetAttackTime(BASE_ATTACK, arch.mods.attackMs);
        c->SetAttackTime(OFF_ATTACK, arch.mods.attackMs);
        c->SetAttackTime(RANGED_ATTACK, 2500);
        c->SetBaseWeaponDamage(BASE_ATTACK, MINDAMAGE, s.minDamage);
        c->SetBaseWeaponDamage(BASE_ATTACK, MAXDAMAGE, s.maxDamage);
        c->SetBaseWeaponDamage(RANGED_ATTACK, MINDAMAGE, s.minRanged);
        c->SetBaseWeaponDamage(RANGED_ATTACK, MAXDAMAGE, s.maxRanged);
        c->UpdateAllStats();
        c->UpdateMaxHealth();
        c->UpdateMaxPower(POWER_MANA);
        c->UpdateArmor();
        c->UpdateDamagePhysical(BASE_ATTACK);
        // Guardian::UpdateDamagePhysical leaves the ranged fields alone: set them directly.
        c->SetStatFloatValue(UNIT_FIELD_MINRANGEDDAMAGE, s.minRanged);
        c->SetStatFloatValue(UNIT_FIELD_MAXRANGEDDAMAGE, s.maxRanged);
        c->SetFullHealth();
        c->SetPower(POWER_MANA, c->GetMaxPower(POWER_MANA));
        c->SetRegeneratingHealth(false); // the AI eats between pulls
    }

    // ---- operations -------------------------------------------------------------------------------------------------
    std::vector<std::string> Add(Player* owner, AddArgs const& args)
    {
        std::vector<std::string> lines;
        if (!Enabled())
        {
            lines.push_back("Companions are off (Emberfall.Companions.Enable, or no ef_companion_archetype rows on this world).");
            return lines;
        }
        Roster* r = GetRoster(owner, true);
        std::vector<Archetype const*> choices;
        if (!args.archetype.empty())
        {
            Archetype const* a = FindArchetype(args.archetype);
            if (!a)
            {
                std::string names;
                for (Archetype const* x : Archetypes())
                    names += (names.empty() ? "" : ", ") + x->name + " (" + RoleName(x->role) + ")";
                lines.push_back("Unknown archetype '" + args.archetype + "'. Archetypes: " + names + ".");
                return lines;
            }
            if (!Fits(args.role, a->role))
            {
                lines.push_back("The archetype " + a->name + " is a " + RoleName(a->role) + ", not that role.");
                return lines;
            }
            choices.push_back(a);
        }
        else
            for (Archetype const* a : Archetypes())
                if (Fits(args.role, a->role))
                    choices.push_back(a);
        if (choices.empty())
        {
            lines.push_back("No archetype has that role.");
            return lines;
        }
        // Without an archetype: the role's archetypes in turn, continuing from what is already out.
        size_t turn = 0;
        for (Member const& m : r->members)
            for (Archetype const* a : choices)
                if (a->id == m.archetype)
                    ++turn;
        for (uint32 i = 0; i < args.count; ++i)
        {
            if (r->members.size() >= s_cfg.maxPerPlayer)
            {
                lines.push_back(Acore::StringFormat("At most {} companions (Emberfall.Companions.MaxPerPlayer).", s_cfg.maxPerPlayer));
                break;
            }
            Archetype const& a = *choices[(turn + i) % choices.size()];
            uint32 const index = uint32(r->members.size());
            Creature* c = Summon(owner, a, index, nullptr);
            if (!c)
            {
                lines.push_back("Could not summon a " + a.name + " (creature " + std::to_string(a.entry) + ").");
                break;
            }
            Member m;
            m.guid = c->GetGUID();
            m.archetype = a.id;
            r->members.push_back(m);
            lines.push_back(Acore::StringFormat("#{} {} ({}), level {}: {} health, {} mana.", index + 1, a.name, RoleName(a.role), c->GetLevel(),
                                                c->GetMaxHealth(), c->GetMaxPower(POWER_MANA)));
        }
        PushFrame(owner, *r, true);
        return lines;
    }

    std::vector<std::string> List(Player* owner)
    {
        std::vector<std::string> lines;
        Roster* r = GetRoster(owner, false);
        if (!r || r->members.empty())
        {
            lines.push_back("No companions. `.efparty add <tank|healer|dps|melee|ranged> [archetype] [count]`.");
            return lines;
        }
        Unit* tank = Tank(owner);
        lines.push_back(Acore::StringFormat("{} companions, {}, assist {}; the tank is {}; you at ({:.1f}, {:.1f}, {:.1f}).", r->members.size(),
                                            r->stay ? "staying" : "following", r->assist ? "on" : "off",
                                            !tank ? "nobody (dead)" : tank == owner ? "you" : "the tank companion", owner->GetPositionX(),
                                            owner->GetPositionY(), owner->GetPositionZ()));
        for (size_t i = 0; i < r->members.size(); ++i)
        {
            Member const& m = r->members[i];
            Archetype const* a = FindArchetype(m.archetype);
            Creature* c = ObjectAccessor::GetCreature(*owner, m.guid);
            std::string const name = a ? a->name : "?";
            if (m.dead || !c || !c->IsAlive())
            {
                lines.push_back(Acore::StringFormat("#{} {} ({}): dead", i + 1, name, a ? RoleName(a->role) : "?"));
                continue;
            }
            CompanionAIBase* ai = AIOf(c);
            Unit* v = c->GetVictim();
            lines.push_back(Acore::StringFormat("#{} {} ({}) L{}: {}/{} hp, {}/{} mana, state {}, target {}; dealt {}, healed {}; {:.0f} yd, {}", i + 1,
                                                name, a ? RoleName(a->role) : "?", c->GetLevel(), c->GetHealth(), c->GetMaxHealth(),
                                                c->GetPower(POWER_MANA), c->GetMaxPower(POWER_MANA), ai ? ai->FrameState() : '?', v ? v->GetName() : "-",
                                                m.damage, m.healing, c->GetDistance(owner), ai ? ai->Debug() : ""));
        }
        return lines;
    }

    std::vector<std::string> Remove(Player* owner, std::string const& which)
    {
        std::vector<std::string> lines;
        Roster* r = GetRoster(owner, false);
        if (!r || r->members.empty())
        {
            lines.push_back("No companions.");
            return lines;
        }
        if (Lower(which) == "all")
        {
            size_t const n = r->members.size();
            DespawnAll(owner, true);
            lines.push_back(Acore::StringFormat("Removed {} companions.", n));
            PushFrame(owner, *r, true);
            return lines;
        }
        std::optional<uint32_t> n = ParseCount(which);
        if (!n || *n < 1 || *n > r->members.size())
        {
            lines.push_back(Acore::StringFormat("Which one? 1 to {}, or all.", r->members.size()));
            return lines;
        }
        Member const m = r->members[*n - 1];
        if (Creature* c = ObjectAccessor::GetCreature(*owner, m.guid))
            if (TempSummon* s = c->ToTempSummon())
                s->UnSummon();
        r->members.erase(r->members.begin() + (*n - 1));
        // The others close up: their formation slots follow the new order.
        for (size_t i = 0; i < r->members.size(); ++i)
            if (Creature* c = ObjectAccessor::GetCreature(*owner, r->members[i].guid))
                if (CompanionAIBase* ai = AIOf(c))
                    if (Archetype const* a = FindArchetype(r->members[i].archetype))
                        ai->Configure(a, uint32(i), owner->GetGUID());
        Archetype const* a = FindArchetype(m.archetype);
        lines.push_back(Acore::StringFormat("Removed #{} {}.", *n, a ? a->name : "?"));
        PushFrame(owner, *r, true);
        return lines;
    }

    void SetStay(Player* owner, bool stay)
    {
        if (Roster* r = GetRoster(owner, true))
            r->stay = stay;
    }

    void SetAssist(Player* owner, bool assist)
    {
        if (Roster* r = GetRoster(owner, true))
            r->assist = assist;
    }

    uint32 Resync(Player* owner)
    {
        uint32 n = 0;
        for (Creature* c : LiveCompanions(owner))
            if (CompanionAIBase* ai = AIOf(c))
                if (Archetype const* a = FindArchetype(ai->ArchetypeId()))
                {
                    ApplyScaling(c, *a, owner->GetLevel());
                    ++n;
                }
        return n;
    }

    void DespawnAll(Player* owner, bool forget)
    {
        Roster* r = GetRoster(owner, false);
        if (!r)
            return;
        for (Member const& m : r->members)
            if (Creature* c = ObjectAccessor::GetCreature(*owner, m.guid))
                if (TempSummon* s = c->ToTempSummon())
                    s->UnSummon();
        if (forget)
        {
            r->members.clear();
            r->mobs.clear();
        }
    }

    void Resummon(Player* owner)
    {
        Roster* r = GetRoster(owner, false);
        if (!r || r->members.empty())
            return;
        r->mobs.clear();
        r->inFight = false;
        uint32 n = 0;
        for (size_t i = 0; i < r->members.size(); ++i)
        {
            Member& m = r->members[i];
            if (Creature* old = ObjectAccessor::GetCreature(*owner, m.guid); old && old->IsAlive())
                continue; // still here (a near teleport)
            Archetype const* a = FindArchetype(m.archetype);
            if (!a)
                continue;
            if (Creature* c = Summon(owner, *a, uint32(i), nullptr))
            {
                m.guid = c->GetGUID();
                m.dead = false;
                ++n;
            }
        }
        LOG_DEBUG("module", "Emberfall companions: {} resummoned {} companions on map {}", owner->GetName(), n, owner->GetMapId());
        PushFrame(owner, *r, true);
    }

    Creature* Revive(Player* owner, ObjectGuid guid, float hpPct)
    {
        Roster* r = GetRoster(owner, false);
        Member* m = FindMember(r, guid);
        if (!m)
            return nullptr;
        Archetype const* a = FindArchetype(m->archetype);
        if (!a)
            return nullptr;
        Position const at = m->deathPos;
        if (Creature* old = ObjectAccessor::GetCreature(*owner, m->guid))
            if (TempSummon* s = old->ToTempSummon())
                s->UnSummon();
        uint32 const index = uint32(m - r->members.data());
        Creature* c = Summon(owner, *a, index, &at);
        if (!c)
            return nullptr;
        m->guid = c->GetGUID();
        m->dead = false;
        c->SetHealth(std::max<uint32>(1, uint32(float(c->GetMaxHealth()) * hpPct)));
        c->SetPower(POWER_MANA, uint32(float(c->GetMaxPower(POWER_MANA)) * hpPct));
        PushFrame(owner, *r, true);
        return c;
    }

    std::vector<std::pair<ObjectGuid, Position>> DeadMembers(Player* owner)
    {
        std::vector<std::pair<ObjectGuid, Position>> out;
        if (Roster* r = GetRoster(owner, false))
            for (Member const& m : r->members)
                if (m.dead)
                    out.emplace_back(m.guid, m.deathPos);
        return out;
    }

    std::vector<std::string> StatsLines(Player* owner)
    {
        std::vector<std::string> lines;
        Roster* r = GetRoster(owner, false);
        if (!r)
        {
            lines.push_back("No companion stats yet.");
            return lines;
        }
        FightStats const& s = r->stats;
        uint32 const fightMs = s.fightMs + (r->inFight ? r->clockMs - r->fightStartMs : 0);
        lines.push_back(Acore::StringFormat("EFPARTY stats fights {} kills {} fight_s {:.1f} avg_kill_s {:.1f} on_tank_pct {:.1f} aggro_losses {} "
                                            "lead_min {:.2f} lead_avg {:.2f} companion_deaths {} owner_deaths {} healer_mana_min {:.0f}",
                                            s.fights, s.kills, fightMs / 1000.0f, s.kills ? s.killMsSum / 1000.0f / s.kills : 0.0f,
                                            s.mobMs ? 100.0 * double(s.mobMsOnTank) / double(s.mobMs) : 0.0, s.aggroLosses, s.minLead,
                                            s.leadSamples ? s.leadSum / s.leadSamples : 0.0, s.companionDeaths, s.ownerDeaths,
                                            s.healerManaMin > 100 ? 100.0f : s.healerManaMin));
        for (auto const& [who, n] : s.lossesTo)
            lines.push_back(Acore::StringFormat("EFPARTY lost_to {} {}", who, n));
        for (size_t i = 0; i < r->members.size(); ++i)
        {
            Member const& m = r->members[i];
            Archetype const* a = FindArchetype(m.archetype);
            lines.push_back(Acore::StringFormat("EFPARTY member {} {} damage {} healing {} dps {:.1f}", i + 1, a ? a->name : "?", m.damage, m.healing,
                                                fightMs ? m.damage * 1000.0 / fightMs : 0.0));
        }
        return lines;
    }

    void ResetStats(Player* owner)
    {
        if (Roster* r = GetRoster(owner, false))
        {
            r->stats = FightStats();
            r->fightStartMs = r->clockMs;
            for (Member& m : r->members)
                m.damage = m.healing = 0;
        }
    }

    std::vector<std::string> ThreatLines(Player* owner)
    {
        std::vector<std::string> lines;
        Unit* tank = Tank(owner);
        for (Unit* mob : EngagedEnemies(owner))
        {
            std::string line = Acore::StringFormat("EFPARTY threat {} (victim {}):", mob->GetName(), mob->GetVictim() ? Label(owner, mob->GetVictim()) : "-");
            for (ThreatReference const* ref : mob->GetThreatMgr().GetSortedThreatList())
                line += Acore::StringFormat(" {}{}={:.0f}", Label(owner, ref->GetVictim()), ref->GetVictim() == tank ? "*" : "", ref->GetThreat());
            lines.push_back(line);
        }
        if (lines.empty())
            lines.push_back("EFPARTY threat: no engaged mobs.");
        return lines;
    }

    // ---- hooks ------------------------------------------------------------------------------------------------------
    void OnOwnerUpdate(Player* owner, uint32 diff)
    {
        Roster* r = GetRoster(owner, false);
        if (!r || r->members.empty())
            return;
        r->clockMs += diff;
        if (r->resummonPending)
        {
            r->resummonPending = false;
            Resummon(owner);
        }
        r->frameTimer += diff;
        r->frameHeartbeat += diff;
        r->trackTimer += diff;
        if (r->trackTimer >= 100)
        {
            Track(owner, *r, r->trackTimer);
            r->trackTimer = 0;
        }
        if (r->frameTimer >= s_cfg.frameMs)
        {
            r->frameTimer = 0;
            PushFrame(owner, *r, r->frameHeartbeat >= 5000);
        }
    }

    void OnCompanionDied(Creature* c)
    {
        Player* owner = OwnerOf(c);
        if (!owner)
            return;
        Roster* r = GetRoster(owner, false);
        if (Member* m = FindMember(r, c->GetGUID()))
        {
            m->dead = true;
            m->deathPos = c->GetPosition();
            ++r->stats.companionDeaths;
            Archetype const* a = FindArchetype(m->archetype);
            LOG_INFO("module", "Emberfall companions: {}'s {} died", owner->GetName(), a ? a->name : "?");
            PushFrame(owner, *r, true);
        }
    }

    void OnDamage(Creature* c, uint32 damage)
    {
        if (Player* owner = OwnerOf(c))
            if (Member* m = FindMember(GetRoster(owner, false), c->GetGUID()))
                m->damage += damage;
    }

    void OnHeal(Creature* c, uint32 healing)
    {
        if (Player* owner = OwnerOf(c))
            if (Member* m = FindMember(GetRoster(owner, false), c->GetGUID()))
                m->healing += healing;
    }

    void OnOwnerDied(Player* owner)
    {
        if (Roster* r = GetRoster(owner, false); r && !r->members.empty())
        {
            ++r->stats.ownerDeaths;
            // A player's death unsummons its guardians (Unit::setDeathState -> RemoveAllControlled): the group fights on
            // without its owner, so they come back at once (the healer resurrects the owner after the fight).
            r->resummonPending = true;
            LOG_INFO("module", "Emberfall companions: {} (the owner) died", owner->GetName());
        }
    }
}
