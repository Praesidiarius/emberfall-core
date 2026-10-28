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

// Emberfall (PATCHES.md, "Module"): the open world's leash as in Classic (Emberfall docs/design/leash-and-social-aggro.md,
// feedback #93). Upstream renews a creature's leash timer when it is hit, attacked or engaged, never when it hits: a
// second mob that keeps hitting you while you fight the first one evades home once its timer ran out (11-15 s) and you
// stand more than CreatureLeashRadius from its home. Here a creature's own landed melee or direct spell hit renews its
// timer too (src/leash/EfLeashLogic.h), so a mob leashes only once nobody has hit anybody for its leash time and its
// victim is away from its home (or unreachable: the core's 10 s evade timer, unchanged). Its linked pack shares the
// timer (the core's AssistDelayEvent shares the pointer with the mobs that came to its call).
// On with Emberfall.OwnData = 1 and Emberfall.Leash.MobHitsRenew (default 1). Dungeons never leash by distance
// (CanCreatureAttack returns before the check), so instances and their bosses' own rules are untouched. Social aggro needs no code: the core's CallAssistance (same faction template
// with the respond-to-call-for-help flag, CreatureFamilyAssistanceRadius) and Emberfall's faction data.

#include "Config.h"
#include "Creature.h"
#include "EmberfallOwnData.h"
#include "Log.h"
#include "UnitScript.h"
#include "WorldScript.h"
#include "leash/EfLeashLogic.h"

namespace
{
    bool s_mobHitsRenew = true;

    Emberfall::Leash::HitKind KindOf(DamageEffectType type)
    {
        switch (type)
        {
            case DIRECT_DAMAGE: return Emberfall::Leash::HitKind::Melee;
            case SPELL_DIRECT_DAMAGE: return Emberfall::Leash::HitKind::SpellDirect;
            case DOT: return Emberfall::Leash::HitKind::OverTime;
            default: return Emberfall::Leash::HitKind::Other;
        }
    }

    class emberfall_leash_world : public WorldScript
    {
    public:
        emberfall_leash_world() : WorldScript("emberfall_leash_world", { WORLDHOOK_ON_AFTER_CONFIG_LOAD }) { }

        void OnAfterConfigLoad(bool /*reload*/) override
        {
            s_mobHitsRenew = sConfigMgr->GetOption<bool>("Emberfall.Leash.MobHitsRenew", true, false);
            if (Emberfall::OwnData())
                LOG_INFO("server.loading", ">> Emberfall leash: a mob's own hits renew its leash: {}",
                         s_mobHitsRenew ? "on" : "off");
        }
    };

    class emberfall_leash_unit : public UnitScript
    {
    public:
        // DealDamage is called on every UnitScript (ScriptMgr::DealDamage walks them all); no hook list needed.
        emberfall_leash_unit() : UnitScript("emberfall_leash_unit", true) { }

        // Unit::DealDamage, first thing: a landed hit (melee misses, dodges and parries never get here; a fully absorbed
        // hit does). Only observes: the damage goes back unchanged.
        uint32 DealDamage(Unit* attacker, Unit* victim, uint32 damage, DamageEffectType type) override
        {
            if (!s_mobHitsRenew || !attacker || !victim || !Emberfall::OwnData())
                return damage;
            Creature* creature = attacker->ToCreature();
            if (!creature)
                return damage;

            Emberfall::Leash::Hit hit;
            hit.attackerIsCreature = true;
            hit.attackerPlayerControlled = creature->IsControlledByPlayer() || creature->GetOwnerGUID().IsPlayer();
            hit.attackerEngaged = creature->IsEngaged();
            hit.attackerEvading = creature->IsInEvadeMode();
            hit.selfHit = attacker == victim;
            hit.kind = KindOf(type);
            if (Emberfall::Leash::HitRenewsAttackerLeash(hit))
                creature->UpdateLeashExtensionTime();
            return damage;
        }
    };
}

void AddEmberfallLeashScripts()
{
    new emberfall_leash_world();
    new emberfall_leash_unit();
}
