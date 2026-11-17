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

// Veins worked in swings (Emberfall feedback #622, docs/design/professions.md §7 c, §30): a mining node is a chest
// (type 3) whose template's min_opens/max_opens (data4/data5) say how many swings it holds. Each swing is one Mining
// cast with its own loot; when a swing's loot is taken and the window closes (WorldSession::DoLootRelease sets the
// chest "just deactivated"), the vein is counted (the object's use count, which the core resets at respawn) and set
// ready again while it has swings left; after the last it goes and respawns as before. Anyone may take the next swing
// (the world is shared). Skill-ups stay one roll per player and vein (the core's skill-up list in
// Spell::EffectOpenLock: on the player's first swing), and the yield talents roll on the player's first swing too
// (EfProfSpecs.cpp, IsLaterSwing). The rule: veins/EfVeinSwingsLogic.h (tests/veins_tests.cpp).

#include "AllGameObjectScript.h"
#include "GameObject.h"
#include "Log.h"
#include "LootMgr.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerScript.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "veins/EfVeinSwings.h"
#include "veins/EfVeinSwingsLogic.h"

#include <algorithm>

namespace
{
    namespace VS = Emberfall::VeinSwings;

    // a vein's own state, kept on the object (WorldObject::CustomData: it goes with the object)
    struct VeinState : public DataMap::Base
    {
        bool armed = false;   // a swing's loot is out (opened by a player since the last release)
        uint32 total = 0;     // the swings this spawn holds (rolled when its first swing ends)
    };

    VS::Range RangeOf(GameObject const* go)
    {
        GameObjectTemplate const* t = go->GetGOInfo();
        return VS::Swings(t->chest.minSuccessOpens, t->chest.maxSuccessOpens);
    }

    class emberfall_vein_swings_objects : public AllGameObjectScript
    {
    public:
        emberfall_vein_swings_objects() : AllGameObjectScript("emberfall_vein_swings_objects") { }

        void OnGameObjectLootStateChanged(GameObject* go, uint32 state, Unit* unit) override
        {
            if (!VS::IsVein(go))
                return;
            VeinState* st = go->CustomData.GetDefault<VeinState>("ef_vein");
            if (state == GO_ACTIVATED)
            {
                // Player::SendLoot opened the swing's loot (or DoLootRelease left it open, not all taken)
                if (unit && unit->IsPlayer())
                    st->armed = true;
                return;
            }
            if (state != GO_JUST_DEACTIVATED)
                return;
            uint32 const before = go->GetUseCount();
            if (st->armed && before == 0)
            {
                VS::Range const r = RangeOf(go);
                st->total = urand(r.lo, r.hi);
            }
            VS::Release const what = VS::AfterRelease(st->armed, before, st->total);
            st->armed = false;
            if (what == VS::Release::PassOn)
                return;
            go->AddUse();
            LOG_DEBUG("module", "Emberfall veins: {} (spawn {}) swing {} of {}{}", go->GetEntry(), go->GetSpawnId(),
                      before + 1, st->total, what == VS::Release::Keep ? "" : ", spent");
            // ready for the next swing: DoLootRelease clears the taken loot after this, and the next cast fills a new
            // one (Player::SendLoot on a ready chest)
            if (what == VS::Release::Keep)
                go->SetLootState(GO_READY);
        }
    };

    class emberfall_vein_swings_player : public PlayerScript
    {
    public:
        emberfall_vein_swings_player()
            : PlayerScript("emberfall_vein_swings_player", { PLAYERHOOK_ON_BEFORE_SEND_LOOT }) { }

        void OnPlayerBeforeSendLoot(Player* player, ObjectGuid lootGuid, Loot* loot) override
        {
            if (!lootGuid.IsGameObject() || !loot)
                return;
            if (GameObject* go = ObjectAccessor::GetGameObject(*player, lootGuid))
                VS::MergeLoot(go, loot);
        }
    };
}

namespace Emberfall::VeinSwings
{
    bool IsVein(GameObject const* go)
    {
        return go && go->GetGoType() == GAMEOBJECT_TYPE_CHEST && VS::IsVein(go->GetGOInfo()->chest.maxSuccessOpens);
    }

    bool IsLaterSwing(GameObject const* go, Player const* player)
    {
        return IsVein(go) && player && go->IsInSkillupList(player->GetGUID());
    }

    void MergeLoot(GameObject const* go, Loot* loot)
    {
        if (!IsVein(go) || !loot || loot->items.size() < 2)
            return;
        std::vector<Stack> stacks;
        stacks.reserve(loot->items.size());
        for (LootItem const& li : loot->items)
        {
            Stack s;
            s.item = li.itemid;
            s.property = li.randomPropertyId;
            s.count = li.count;
            s.plain = !li.needs_quest && !li.freeforall && li.conditions.empty() && !li.randomSuffix;
            s.looted = li.is_looted;
            stacks.push_back(s);
        }
        uint32 const folded = Merge(stacks, [](uint32 item) {
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
            return proto ? std::max<uint32>(1, proto->GetMaxStackSize()) : 1u;
        });
        if (!folded)
            return;
        for (size_t i = 0; i < stacks.size(); ++i)
        {
            LootItem& li = loot->items[i];
            if (li.is_looted)
                continue;
            if (!stacks[i].looted)
            {
                li.count = stacks[i].count;
                continue;
            }
            li.is_looted = true;    // folded onto an earlier stack of the same ore: as if taken
            if (loot->unlootedCount)
                --loot->unlootedCount;
        }
    }
}

void AddEmberfallVeinSwingsScripts()
{
    new emberfall_vein_swings_objects();
    new emberfall_vein_swings_player();
}
