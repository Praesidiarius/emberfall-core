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

// Emberfall talents (docs/design/talents.md §3, §4; WPs M1-M5). See EfTalents.h.

#include "EfTalents.h"
#include "EfLoadouts.h"

#include "Chat.h"
#include "Config.h"
#include "CryptoHash.h"
#include "DataMap.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "Player.h"
#include "SharedDefines.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "WorldPacket.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <ctime>
#include <set>

using namespace ef::talents;
namespace eft = ef::eft;

namespace Emberfall::Talents
{
    namespace
    {
        bool s_enabled = false;
        std::shared_ptr<EfTalentModel const> s_data;
        uint32 s_generation = 0; // bumped by every Load(); a player's state follows on its next access

        constexpr char const* kStateKey = "emberfall_talents";
        constexpr uint32 kRequestsPerWindow = 20;
        constexpr uint64 kRequestWindowMs = 10000;
        constexpr uint64 kSpecWindowMs = 2000;
        constexpr char const* kDefaultLoadoutName = "Default";

        constexpr uint32 kStateFlagChoiceAvailable = 0x1;
        constexpr uint32 kStateFlagSwitchPending = 0x2;
        constexpr uint32 kStateFlagDisabled = 0x4;

        constexpr uint32 kSpecFlagStockWiped = 0x1; // ef_character_spec.flags bit 0 (§3.5)

        // Players with a switch cast running: a cheap filter for the per-tick hooks (OnUpdate, OnEnterCombat).
        std::atomic<uint32> s_pendingSwitches{0};

        uint64 NowMs()
        {
            return static_cast<uint64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        }

        std::string HashHex(std::string const& canonical)
        {
            auto const digest = Acore::Crypto::SHA256::GetDigestOf(reinterpret_cast<uint8 const*>(canonical.data()), canonical.size());
            static constexpr char hex[] = "0123456789abcdef";
            std::string out;
            for (size_t i = 0; i < 8; ++i)
            {
                out += hex[digest[i] >> 4];
                out += hex[digest[i] & 15];
            }
            return out;
        }

        struct Loadout
        {
            uint32 spec = 0;
            uint32 id = 0;
            std::string name;
            eft::Pairs pairs;
            std::string hash;
            bool active = false;
            uint32 updated = 0;
        };

        // A spec switch whose cast runs (§1.5, §3.2): started by SPEC, finished in OnUpdate once the cast went
        // off (OnSpellCast sets `cast`), failed when the cast ended without it (moved, interrupted, combat).
        struct PendingSwitch
        {
            uint32 spec = 0; // 0 = none
            uint32 seq = 0;
            uint32 spell = 0;
            bool cast = false;   // the switch spell went off
            bool combat = false; // cancelled by entering combat
        };

        // The per-character state, kept on the Player (DataMap) while it is online.
        class PlayerState : public DataMap::Base
        {
        public:
            PlayerState() = default;
            PlayerState(PlayerState const&) = default;
            PlayerState& operator=(PlayerState const&) = default;
            ~PlayerState() override
            {
                if (pending.spec)
                    --s_pendingSwitches;
            }

            bool loaded = false;
            uint32 generation = 0;
            uint32 activeSpec = 0;
            std::string dataHash;
            uint32 flags = 0;
            std::vector<Loadout> loadouts;
            std::map<uint32, uint8> granted;                       // spell -> GrantSource
            std::vector<std::pair<uint32, std::string>> resets;    // RESET notices for the next HELLO
            bool hello = false;                                    // the client said HELLO with our hash
            eft::Reassembly chunks;
            eft::RateLimit requests{kRequestsPerWindow, kRequestWindowMs};
            eft::RateLimit specRequests{1, kSpecWindowMs};
            uint64 throttleAnsweredAt = 0;
            PendingSwitch pending;
            std::map<uint32, std::vector<BarButton>> bars;         // spec -> bars of the inactive specs (§3.6)

            Loadout* Active(uint32 spec)
            {
                for (Loadout& l : loadouts)
                    if (l.spec == spec && l.active)
                        return &l;
                return nullptr;
            }

            Loadout* Find(uint32 spec, uint32 id)
            {
                for (Loadout& l : loadouts)
                    if (l.spec == spec && l.id == id)
                        return &l;
                return nullptr;
            }

            void StartSwitch(PendingSwitch const& p)
            {
                if (!pending.spec)
                    ++s_pendingSwitches;
                pending = p;
            }

            PendingSwitch EndSwitch()
            {
                PendingSwitch const p = pending;
                if (pending.spec)
                    --s_pendingSwitches;
                pending = PendingSwitch{};
                return p;
            }
        };

        // ---- persistence -----------------------------------------------------------------------------

        void SaveSpecRow(CharacterDatabaseTransaction trans, uint32 guid, PlayerState const& st)
        {
            trans->Append("REPLACE INTO `ef_character_spec` (`guid`, `active_spec`, `data_hash`, `flags`) VALUES ({}, {}, '{}', {})",
                guid, st.activeSpec, st.dataHash, st.flags);
        }

        void SaveLoadoutRow(CharacterDatabaseTransaction trans, uint32 guid, Loadout const& l)
        {
            std::string name = l.name;
            CharacterDatabase.EscapeString(name);
            trans->Append("REPLACE INTO `ef_character_loadout` (`guid`, `spec`, `loadout`, `name`, `ranks`, `data_hash`, `active`, `updated`) "
                "VALUES ({}, {}, {}, '{}', '{}', '{}', {}, {})", guid, l.spec, l.id, name, eft::encodePairs(l.pairs), l.hash,
                l.active ? 1 : 0, l.updated);
        }

        // The bars of an inactive spec (§3.6): ef_character_actions holds exactly them.
        void SaveBars(CharacterDatabaseTransaction trans, uint32 guid, uint32 spec, std::vector<BarButton> const& bars)
        {
            trans->Append("DELETE FROM `ef_character_actions` WHERE `guid` = {} AND `spec` = {}", guid, spec);
            for (BarButton const& b : bars)
                trans->Append("INSERT INTO `ef_character_actions` (`guid`, `spec`, `button`, `action`, `type`) VALUES ({}, {}, {}, {}, {})",
                    guid, spec, b.button, b.action, b.type);
        }

        // The player's current 144 buttons (the core's active set).
        std::vector<BarButton> CurrentBars(Player* player)
        {
            std::vector<BarButton> out;
            for (uint32 button = 0; button < kMaxActionButtons; ++button)
                if (ActionButton const* ab = player->GetActionButton(static_cast<uint8>(button)))
                    out.push_back(BarButton{button, ab->GetAction(), static_cast<uint32>(ab->GetType())});
            return out;
        }

        // Replaces the core's active set; addActionButton validates each button (a spell the player does not know
        // is skipped). The core saves the set into character_action at the next save.
        void SetBars(Player* player, std::vector<BarButton> const& bars)
        {
            for (uint32 button = 0; button < kMaxActionButtons; ++button)
                player->removeActionButton(static_cast<uint8>(button));
            for (BarButton const& b : bars)
                player->addActionButton(static_cast<uint8>(b.button), b.action, static_cast<uint8>(b.type));
        }

        // One transaction per request (§3.7); nothing to write, nothing sent to the database.
        void Commit(CharacterDatabaseTransaction trans)
        {
            if (trans->GetSize())
                CharacterDatabase.CommitTransaction(trans);
        }

        void SendBody(Player* player, std::string const& body)
        {
            std::string const text = std::string(eft::kPrefix) + body;
            WorldPacket data;
            ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player, player, text);
            player->SendDirectMessage(&data);
        }

        void SendOk(Player* player, uint32 seq) { SendBody(player, eft::body(0, "OK", {std::to_string(seq)})); }

        void SendErr(Player* player, uint32 seq, std::string_view code, std::string const& detail = {})
        {
            SendBody(player, eft::body(0, "ERR", {std::to_string(seq), std::string(code), detail}));
        }

        // ---- reconciliation (§3.3) -------------------------------------------------------------------

        // A spell with a stock talent cost (Talent.dbc: only the generated stock test trees have such spells) is
        // not erased by removeSpell: it stays in the spell map in no talent group (specMask 0), for the stock
        // dual-spec logic. Such a leftover still takes part in the core's rank logic: learning a lower rank of
        // its chain re-learns it (learnSpell) or marks the new rank inactive (addSpell). The stock talent
        // system is off for a class with trees, so the leftover is dropped the way removeSpell drops any other
        // spell (deleted from character_spell at the next save).
        void DropLeftover(Player* player, uint32 spell)
        {
            PlayerSpellMap& spells = player->GetSpellMap();
            auto it = spells.find(spell);
            if (it == spells.end() || it->second->State == PLAYERSPELL_REMOVED || it->second->specMask != 0)
                return;
            if (it->second->State == PLAYERSPELL_NEW || it->second->State == PLAYERSPELL_TEMPORARY)
            {
                delete it->second;
                spells.erase(it);
            }
            else
                it->second->State = PLAYERSPELL_REMOVED;
        }

        // The spells learnSpell(spell) may learn as well: the other ranks of its chain and the spells that
        // require it (Player::learnSpell re-learns those the spell map holds outside the current talent group).
        std::vector<uint32> Companions(uint32 spell)
        {
            std::vector<uint32> out;
            for (uint32 s = sSpellMgr->GetFirstSpellInChain(spell); s; s = sSpellMgr->GetNextSpellInChain(s))
            {
                if (s != spell)
                    out.push_back(s);
                auto const bounds = sSpellMgr->GetSpellsRequiringSpellBounds(s);
                for (auto it = bounds.first; it != bounds.second; ++it)
                    out.push_back(it->second);
            }
            return out;
        }

        // Teaches the expected spells the module has not granted yet and takes back the granted ones that are
        // no longer expected; never touches a spell the core or a GM taught. Removals first, so a rank change
        // within a stock spell chain does not take the new rank with the old one.
        void Reconcile(Player* player, PlayerState& st, EfTalentModel const& data, CharacterDatabaseTransaction trans)
        {
            uint32 const guid = player->GetGUID().GetCounter();
            SpellSet expected;
            if (EfSpec const* spec = data.Spec(st.activeSpec))
            {
                Loadout const* l = st.Active(spec->id);
                expected = data.Expected(*spec, l ? l->pairs : eft::Pairs{}, player->GetLevel());
            }
            std::set<uint32> removed;
            for (auto it = st.granted.begin(); it != st.granted.end();)
            {
                if (expected.count(it->first))
                {
                    ++it;
                    continue;
                }
                player->removeSpell(it->first, SPEC_MASK_ALL, false);
                DropLeftover(player, it->first);
                removed.insert(it->first);
                trans->Append("DELETE FROM `ef_character_granted` WHERE `guid` = {} AND `spell` = {}", guid, it->first);
                it = st.granted.erase(it);
            }
            if (!removed.empty())
            {
                // removeSpell drops the auras the player owns from the spell itself; also drop the ones those
                // spells triggered (the stock spec switch does the same, Player::ActivateSpec).
                player->RemoveOwnedAuras([&](Aura const* aura) {
                    if (removed.count(aura->GetId()))
                        return true;
                    SpellInfo const* by = aura->GetTriggeredByAuraSpellInfo();
                    return by && removed.count(by->Id);
                });
            }
            for (auto const& [spell, source] : expected)
            {
                bool const ours = st.granted.count(spell) != 0;
                if (player->HasSpell(spell))
                    continue; // known: ours already, or the core's / a GM's (never recorded as ours)
                if (!sSpellMgr->GetSpellInfo(spell))
                    continue;
                // Leftovers of its chain go first; whatever learnSpell teaches besides the spell (a companion
                // that was not known before and is not expected) is taken back right after.
                std::vector<uint32> const companions = Companions(spell);
                std::set<uint32> knownBefore;
                for (uint32 const c : companions)
                {
                    DropLeftover(player, c);
                    if (player->HasSpell(c))
                        knownBefore.insert(c);
                }
                player->learnSpell(spell);
                for (uint32 const c : companions)
                    if (!knownBefore.count(c) && !expected.count(c) && player->HasSpell(c))
                    {
                        player->removeSpell(c, SPEC_MASK_ALL, false);
                        DropLeftover(player, c);
                    }
                if (!ours && player->HasSpell(spell))
                {
                    st.granted[spell] = static_cast<uint8>(source);
                    trans->Append("REPLACE INTO `ef_character_granted` (`guid`, `spell`, `source`) VALUES ({}, {}, {})", guid, spell,
                        static_cast<uint32>(source));
                }
            }
        }

        // ---- state load and migration (§3.7) -------------------------------------------------------

        // Re-validates every stored loadout against the current data and the player's level: after a data
        // change (hash differs) through node IDs (dropped nodes, clamped ranks), and at every level; a loadout
        // that fails is reset (RESET DATA / LEVEL). Then reconciles the spells.
        void Revalidate(Player* player, PlayerState& st, EfTalentModel const& data, bool notify)
        {
            uint32 const guid = player->GetGUID().GetCounter();
            uint32 const level = player->GetLevel();
            CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
            bool specChanged = false;
            if (st.activeSpec && !data.Spec(st.activeSpec))
            {
                st.resets.emplace_back(st.activeSpec, "DATA");
                st.activeSpec = 0;
                specChanged = true;
            }
            for (auto it = st.loadouts.begin(); it != st.loadouts.end();)
            {
                Loadout& l = *it;
                EfSpec const* spec = data.Spec(l.spec);
                if (!spec || spec->classId != player->getClass())
                {
                    trans->Append("DELETE FROM `ef_character_loadout` WHERE `guid` = {} AND `spec` = {} AND `loadout` = {}", guid, l.spec, l.id);
                    st.resets.emplace_back(l.spec, "DATA");
                    it = st.loadouts.erase(it);
                    continue;
                }
                std::string reason;
                if (l.hash != data.dataHash)
                {
                    bool changed = false;
                    l.pairs = data.Remap(*spec, l.pairs, changed);
                    if (changed)
                        reason = "DATA";
                    if (!data.Validate(*spec, data.LoadoutString(*spec, l.pairs), level).ok())
                    {
                        l.pairs.clear();
                        reason = "DATA";
                    }
                    l.hash = data.dataHash;
                    SaveLoadoutRow(trans, guid, l);
                }
                if (!data.Validate(*spec, data.LoadoutString(*spec, l.pairs), level).ok())
                {
                    l.pairs.clear();
                    reason = "LEVEL";
                    SaveLoadoutRow(trans, guid, l);
                }
                if (!reason.empty())
                {
                    LOG_INFO("module", "Emberfall talents: {} ({}): loadout {} of spec {} reset or changed ({})", player->GetName(), guid,
                        l.id, l.spec, reason);
                    st.resets.emplace_back(l.spec, reason);
                }
                ++it;
            }
            if (st.dataHash != data.dataHash || specChanged)
            {
                st.dataHash = data.dataHash;
                SaveSpecRow(trans, guid, st);
            }
            Reconcile(player, st, data, trans);
            Commit(trans);
            if (notify && st.hello)
            {
                for (auto const& [spec, reason] : st.resets)
                    SendBody(player, eft::body(0, "RESET", {std::to_string(spec), reason}));
                st.resets.clear();
            }
        }

        // §3.5: the first time a character of a class with trees is loaded under the system, its stock talents go
        // (free of charge, in every talent group) and a second talent group is dropped (stock dual specialization;
        // the core's ActivateSpec/UpdateSpecCount). Glyphs stay. Once per character (ef_character_spec.flags bit
        // 0); runs before the module's own spells are reconciled, as ActivateSpec swaps talent-group spells.
        void MigrateStock(Player* player, PlayerState& st)
        {
            if (st.flags & kSpecFlagStockWiped)
                return;
            uint8 const groups = player->GetSpecsCount();
            uint8 const wasActive = player->GetActiveSpec();
            size_t talentsBefore = 0; // stock talent entries in any group
            for (auto const& [spell, talent] : player->GetTalentMap())
                if (talent->State != PLAYERSPELL_REMOVED)
                    ++talentsBefore;
            bool reset = false;
            for (uint8 group = 0; group < groups; ++group)
            {
                player->ActivateSpec(group);
                reset = player->resetTalents(true) || reset;
            }
            if (player->GetActiveSpec() != 0)
                player->ActivateSpec(0);
            if (groups > 1)
                player->UpdateSpecCount(1);
            st.flags |= kSpecFlagStockWiped;
            CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
            SaveSpecRow(trans, player->GetGUID().GetCounter(), st);
            Commit(trans);
            if (reset || groups > 1)
                player->SendTalentsInfoData(false); // the login sent the talents before this reset
            if (reset || groups > 1)
                LOG_INFO("module", "Emberfall talents: {} ({}): stock talents reset ({} talent groups, group {} was active, {} talent "
                    "entries) and one talent group kept", player->GetName(), player->GetGUID().GetCounter(), groups, wasActive, talentsBefore);
        }

        void LoadState(Player* player, PlayerState& st, EfTalentModel const& data)
        {
            uint32 const guid = player->GetGUID().GetCounter();
            st = PlayerState{};
            if (QueryResult r = CharacterDatabase.Query("SELECT `active_spec`, `data_hash`, `flags` FROM `ef_character_spec` WHERE `guid` = {}", guid))
            {
                Field* f = r->Fetch();
                st.activeSpec = f[0].Get<uint16>();
                st.dataHash = f[1].Get<std::string>();
                st.flags = f[2].Get<uint32>();
            }
            if (QueryResult r = CharacterDatabase.Query("SELECT `spec`, `loadout`, `name`, `ranks`, `data_hash`, `active`, `updated` "
                "FROM `ef_character_loadout` WHERE `guid` = {} ORDER BY `spec`, `loadout`", guid))
            {
                do
                {
                    Field* f = r->Fetch();
                    Loadout l;
                    l.spec = f[0].Get<uint16>();
                    l.id = f[1].Get<uint8>();
                    l.name = f[2].Get<std::string>();
                    std::string const ranks = f[3].Get<std::string>();
                    l.hash = f[4].Get<std::string>();
                    l.active = f[5].Get<uint8>() != 0;
                    l.updated = f[6].Get<uint32>();
                    if (auto pairs = eft::decodePairs(ranks))
                        l.pairs = std::move(*pairs);
                    else
                    {
                        LOG_ERROR("module", "Emberfall talents: {} ({}): loadout {} of spec {} has an unreadable `ranks` '{}'; reset",
                            player->GetName(), guid, l.id, l.spec, ranks);
                        l.hash.clear(); // revalidated (and saved) below
                    }
                    st.loadouts.push_back(std::move(l));
                } while (r->NextRow());
            }
            if (QueryResult r = CharacterDatabase.Query("SELECT `spell`, `source` FROM `ef_character_granted` WHERE `guid` = {}", guid))
            {
                do
                {
                    Field* f = r->Fetch();
                    st.granted[f[0].Get<uint32>()] = f[1].Get<uint8>();
                } while (r->NextRow());
            }
            if (QueryResult r = CharacterDatabase.Query("SELECT `spec`, `button`, `action`, `type` FROM `ef_character_actions` WHERE `guid` = {} "
                "ORDER BY `spec`, `button`", guid))
            {
                do
                {
                    Field* f = r->Fetch();
                    uint32 const spec = f[0].Get<uint16>();
                    if (spec != st.activeSpec) // the active spec's bars are the core's (character_action)
                        st.bars[spec].push_back(BarButton{f[1].Get<uint8>(), f[2].Get<uint32>(), f[3].Get<uint8>()});
                } while (r->NextRow());
            }
            st.loaded = true;
            st.generation = s_generation;
            MigrateStock(player, st);
            Revalidate(player, st, data, false);
        }

        // The player's state, loaded on first use and re-validated after a data reload. Null when the system is
        // off or the class has no trees.
        PlayerState* State(Player* player)
        {
            std::shared_ptr<EfTalentModel const> data = Data();
            if (!data || !data->HasClass(player->getClass()))
                return nullptr;
            PlayerState* st = player->CustomData.GetDefault<PlayerState>(kStateKey);
            if (!st->loaded)
                LoadState(player, *st, *data);
            else if (st->generation != s_generation)
            {
                st->generation = s_generation;
                Revalidate(player, *st, *data, true);
            }
            return st;
        }

        // ---- protocol answers ------------------------------------------------------------------------

        void SendState(Player* player, PlayerState const* st, EfTalentModel const* data)
        {
            uint32 const level = player->GetLevel();
            uint32 flags = 0;
            uint32 classBudget = 0, specBudget = 0, active = 0;
            std::string hash = data && !data->dataHash.empty() ? data->dataHash : "0000000000000000";
            if (!st || !data)
                flags |= kStateFlagDisabled;
            else
            {
                active = st->activeSpec;
                if (st->pending.spec)
                    flags |= kStateFlagSwitchPending;
                if (active)
                    std::tie(classBudget, specBudget) = data->Budget(player->getClass(), level);
                else if (level >= data->specLevel)
                    flags |= kStateFlagChoiceAvailable;
            }
            SendBody(player, eft::body(0, "STATE", {hash, std::to_string(active), std::to_string(level), std::to_string(classBudget),
                std::to_string(specBudget), std::to_string(flags)}));
        }

        void SendLoadout(Player* player, EfTalentModel const& data, Loadout const& l)
        {
            EfSpec const* spec = data.Spec(l.spec);
            if (!spec)
                return;
            for (std::string const& body : eft::chunked(0, "LOADOUT", {std::to_string(l.spec), std::to_string(l.id), l.active ? "1" : "0",
                eft::encodeName(l.name)}, data.LoadoutString(*spec, l.pairs)))
                SendBody(player, body);
        }

        // Player state checks of §3.4: no switch cast running (BUSY, checked first: the switch cast is a cast), alive,
        // not in combat, not casting, not on a taxi.
        char const* StateError(Player* player, PlayerState const& st)
        {
            if (st.pending.spec)
                return "BUSY";
            if (!player->IsAlive())
                return "STATE";
            if (player->IsInCombat())
                return "COMBAT";
            if (player->IsNonMeleeSpellCast(false) || player->IsInFlight())
                return "STATE";
            return nullptr;
        }

        Loadout& ActiveLoadout(PlayerState& st, uint32 spec)
        {
            if (Loadout* l = st.Active(spec))
                return *l;
            uint32 id = 1;
            for (Loadout const& l : st.loadouts)
                if (l.spec == spec)
                    id = std::max(id, l.id + 1);
            Loadout l;
            l.spec = spec;
            l.id = id;
            l.name = kDefaultLoadoutName;
            l.active = true;
            st.loadouts.push_back(l);
            return st.loadouts.back();
        }

        // Makes `spec` the active spec: the first choice, the end of a switch cast, or a GM's instant switch (§1.6,
        // §3.6). The old spec's bars go to ef_character_actions; the spells are reconciled (the old spec's baseline
        // and node spells go, the new spec's come); then the new spec's stored bars are loaded, or, the first time
        // the spec is entered, the current bars without the spells it does not know plus its baseline spells in the
        // empty buttons of the first bar. The client gets SMSG_ACTION_BUTTONS 2 (clear) and 1 (the new set).
        void EnterSpec(Player* player, PlayerState& st, EfTalentModel const& data, uint32 spec)
        {
            uint32 const guid = player->GetGUID().GetCounter();
            uint32 const old = st.activeSpec;
            CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
            std::vector<BarButton> const current = CurrentBars(player);
            if (old && old != spec)
            {
                st.bars[old] = current;
                SaveBars(trans, guid, old, current);
            }
            player->SendActionButtons(2);
            st.activeSpec = spec;
            st.dataHash = data.dataHash;
            Loadout& l = ActiveLoadout(st, spec);
            l.hash = data.dataHash;
            l.updated = static_cast<uint32>(std::time(nullptr));
            SaveSpecRow(trans, guid, st);
            SaveLoadoutRow(trans, guid, l);
            Reconcile(player, st, data, trans);
            std::vector<BarButton> next;
            auto stored = st.bars.find(spec);
            if (stored != st.bars.end())
            {
                next = stored->second;
                st.bars.erase(stored);
            }
            else if (EfSpec const* s = data.Spec(spec))
            {
                std::vector<std::pair<uint32, uint32>> baseline = s->baseline;
                std::stable_sort(baseline.begin(), baseline.end(), [](auto const& a, auto const& b) { return a.second < b.second; });
                std::vector<uint32> spells;
                for (auto const& [spell, level] : baseline)
                    if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spell); info && !info->IsPassive())
                        spells.push_back(spell);
                next = FirstBars(current, [player](uint32 spell) { return player->HasSpell(spell); }, spells);
            }
            trans->Append("DELETE FROM `ef_character_actions` WHERE `guid` = {} AND `spec` = {}", guid, spec);
            SetBars(player, next);
            player->SendActionButtons(1);
            Commit(trans);
            LOG_DEBUG("module", "Emberfall talents: {} ({}): spec {} -> {}, {} buttons", player->GetName(), guid, old, spec, next.size());
        }

        // Applies validated pairs to the active loadout of the active spec: save, reconcile.
        void ApplyPairs(Player* player, PlayerState& st, EfTalentModel const& data, eft::Pairs pairs)
        {
            uint32 const guid = player->GetGUID().GetCounter();
            CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
            Loadout& l = ActiveLoadout(st, st.activeSpec);
            l.pairs = std::move(pairs);
            l.hash = data.dataHash;
            l.updated = static_cast<uint32>(std::time(nullptr));
            if (st.dataHash != data.dataHash)
            {
                st.dataHash = data.dataHash;
                SaveSpecRow(trans, guid, st);
            }
            SaveLoadoutRow(trans, guid, l);
            Reconcile(player, st, data, trans);
            Commit(trans);
        }

        void HandleHello(Player* player, PlayerState* st, EfTalentModel const* data, eft::Request const& req)
        {
            if (req.fields.empty() || req.fields[0].size() != 16)
                return SendErr(player, req.seq, "DECODE");
            std::string hash = req.fields[0];
            std::transform(hash.begin(), hash.end(), hash.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (!st || !data)
            {
                // The system is on, but this class has no trees: the client keeps the stock talent UI.
                SendState(player, nullptr, data);
                return SendOk(player, req.seq);
            }
            if (hash != data->dataHash)
            {
                st->hello = false;
                LOG_DEBUG("module", "Emberfall talents: {} sent HELLO with data hash {}, the server has {}", player->GetName(), hash, data->dataHash);
                return SendErr(player, req.seq, "DATA");
            }
            st->hello = true;
            SendState(player, st, data);
            for (Loadout const& l : st->loadouts)
                SendLoadout(player, *data, l);
            for (auto const& [spec, reason] : st->resets)
                SendBody(player, eft::body(0, "RESET", {std::to_string(spec), reason}));
            st->resets.clear();
            SendOk(player, req.seq);
        }

        void HandleSpec(Player* player, PlayerState& st, EfTalentModel const& data, eft::Request const& req)
        {
            if (req.fields.empty())
                return SendErr(player, req.seq, "DECODE");
            auto const specId = eft::number(req.fields[0], 65535);
            if (!specId)
                return SendErr(player, req.seq, "DECODE");
            if (char const* code = StateError(player, st))
                return SendErr(player, req.seq, code);
            EfSpec const* spec = data.Spec(*specId);
            if (!spec || spec->classId != player->getClass())
                return SendErr(player, req.seq, "SPEC", std::to_string(*specId));
            uint32 const level = player->GetLevel();
            if (level < data.specLevel || level < spec->availableLevel)
                return SendErr(player, req.seq, "LEVEL", std::to_string(*specId));
            if (st.activeSpec == *specId)
                return SendOk(player, req.seq); // idempotent
            if (st.activeSpec == 0)
            {
                // The first choice: free and instant (§1.1).
                EnterSpec(player, st, data, *specId);
                SendState(player, &st, &data);
                if (Loadout const* l = st.Active(*specId))
                    SendLoadout(player, data, *l);
                return SendOk(player, req.seq);
            }
            // A switch: the switch cast (§1.5). SWITCH now; at its end STATE, LOADOUT, OK, or ERR when it fails.
            SpellInfo const* info = sSpellMgr->GetSpellInfo(data.switchSpell);
            if (!info)
                return SendErr(player, req.seq, "STATE");
            SpellCastResult const result = player->CastSpell(player, data.switchSpell, false);
            Spell const* cast = player->GetCurrentSpell(CURRENT_GENERIC_SPELL);
            if (result != SPELL_CAST_OK || !cast || cast->GetSpellInfo()->Id != data.switchSpell)
                return SendErr(player, req.seq, "STATE", std::to_string(static_cast<uint32>(result)));
            st.StartSwitch(PendingSwitch{*specId, req.seq, data.switchSpell, false, false});
            SendBody(player, eft::body(0, "SWITCH", {std::to_string(*specId), std::to_string(std::max(cast->GetCastTime(), 0))}));
        }

        // The end of a switch cast (OnUpdate): switch when the spell went off, else ERR COMBAT / STATE.
        void FinishSwitch(Player* player, PlayerState& st)
        {
            PendingSwitch const p = st.EndSwitch();
            std::shared_ptr<EfTalentModel const> data = Data();
            if (!p.cast)
                return SendErr(player, p.seq, p.combat || player->IsInCombat() ? "COMBAT" : "STATE");
            if (!data || !data->HasClass(player->getClass()))
                return SendErr(player, p.seq, "DISABLED");
            EfSpec const* spec = data->Spec(p.spec);
            if (!spec || spec->classId != player->getClass())
                return SendErr(player, p.seq, "SPEC", std::to_string(p.spec));
            if (char const* code = StateError(player, st))
                return SendErr(player, p.seq, code);
            EnterSpec(player, st, *data, p.spec);
            SendState(player, &st, data.get());
            if (Loadout const* l = st.Active(p.spec))
                SendLoadout(player, *data, *l);
            SendOk(player, p.seq);
        }

        // The checks SAVE, LOAD and DEL share: the client synced, a spec chosen, `specId` a spec of the class.
        EfSpec const* LoadoutSpec(Player* player, PlayerState& st, EfTalentModel const& data, eft::Request const& req, uint32 specId)
        {
            if (!st.hello)
                return SendErr(player, req.seq, "DATA"), nullptr;
            if (!st.activeSpec)
                return SendErr(player, req.seq, "NOSPEC"), nullptr;
            EfSpec const* spec = data.Spec(specId);
            if (!spec || spec->classId != player->getClass())
                return SendErr(player, req.seq, "SPEC", std::to_string(specId)), nullptr;
            return spec;
        }

        // SAVE;specId;loadoutId;name;part/total;ranks (§4.4): validated at the player's level, stored, not applied.
        // loadoutId 0 makes a new one (ERR LIMIT past max_loadouts per spec, the active one included; a spec's
        // first loadout becomes its active one); another ID overwrites that loadout's name and ranks. The active
        // loadout of the active spec is the one exception: SAVE to it is an APPLY plus the name.
        void HandleSave(Player* player, PlayerState& st, EfTalentModel const& data, eft::Request const& req)
        {
            if (req.fields.size() < 5)
                return SendErr(player, req.seq, "DECODE");
            auto const specId = eft::number(req.fields[0], 65535);
            auto const loadoutId = eft::number(req.fields[1], kMaxLoadoutId);
            auto const name = eft::decodeName(req.fields[2]);
            auto const part = eft::partOf(req.fields[3]);
            if (!specId || !loadoutId || !name)
                return SendErr(player, req.seq, "DECODE");
            if (!part)
                return SendErr(player, req.seq, "CHUNK");
            std::string loadout;
            switch (st.chunks.feed(req.seq, part->first, part->second, req.fields[4], NowMs()))
            {
                case eft::Reassembly::Status::Incomplete:
                    return;
                case eft::Reassembly::Status::Error:
                    return SendErr(player, req.seq, "CHUNK");
                case eft::Reassembly::Status::Complete:
                    loadout = st.chunks.payload();
                    break;
            }
            EfSpec const* spec = LoadoutSpec(player, st, data, req, *specId);
            if (!spec)
                return;
            if (!eft::validName(*name))
                return SendErr(player, req.seq, "NAME");
            uint32 const level = player->GetLevel();
            if (level < data.specLevel || level < spec->availableLevel)
                return SendErr(player, req.seq, "LEVEL", std::to_string(*specId));
            Loadout* target = nullptr;
            if (*loadoutId)
            {
                target = st.Find(*specId, *loadoutId);
                if (!target)
                    return SendErr(player, req.seq, "LOADOUT", std::to_string(*loadoutId));
            }
            bool const live = target && target->active && *specId == st.activeSpec;
            if (live)
                if (char const* code = StateError(player, st))
                    return SendErr(player, req.seq, code);
            RuleResult const result = data.Validate(*spec, loadout, level);
            if (!result.ok())
                return SendErr(player, req.seq, errorCode(result.code), result.code == RuleError::Decode || result.code == RuleError::BudgetClass ||
                    result.code == RuleError::BudgetSpec ? std::string() : std::to_string(result.detail));
            std::optional<eft::Pairs> pairs = data.PairsOf(*spec, loadout);
            if (!pairs)
                return SendErr(player, req.seq, "DECODE");
            if (!target)
            {
                std::set<uint32> used;
                for (Loadout const& l : st.loadouts)
                    if (l.spec == *specId)
                        used.insert(l.id);
                uint32 const id = NewLoadoutId(used, data.maxLoadouts);
                if (!id)
                    return SendErr(player, req.seq, "LIMIT", std::to_string(data.maxLoadouts));
                Loadout l;
                l.spec = *specId;
                l.id = id;
                l.active = st.Active(*specId) == nullptr;
                st.loadouts.push_back(l);
                target = &st.loadouts.back();
            }
            target->name = *name;
            if (live)
            {
                ApplyPairs(player, st, data, std::move(*pairs)); // saves the row, name included
                SendState(player, &st, &data);
            }
            else
            {
                target->pairs = std::move(*pairs);
                target->hash = data.dataHash;
                target->updated = static_cast<uint32>(std::time(nullptr));
                CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
                SaveLoadoutRow(trans, player->GetGUID().GetCounter(), *target);
                Commit(trans);
            }
            SendLoadout(player, data, *target);
            SendOk(player, req.seq);
        }

        // LOAD;specId;loadoutId: makes a saved loadout the spec's active one; for the active spec that is an APPLY
        // (player state checked, spells reconciled, STATE), for another spec only the mark for its next switch.
        // Answers: STATE (active spec), LOADOUT of the old and the new active loadout, OK.
        void HandleLoad(Player* player, PlayerState& st, EfTalentModel const& data, eft::Request const& req)
        {
            if (req.fields.size() < 2)
                return SendErr(player, req.seq, "DECODE");
            auto const specId = eft::number(req.fields[0], 65535);
            auto const loadoutId = eft::number(req.fields[1], kMaxLoadoutId);
            if (!specId || !loadoutId)
                return SendErr(player, req.seq, "DECODE");
            EfSpec const* spec = LoadoutSpec(player, st, data, req, *specId);
            if (!spec)
                return;
            Loadout* target = st.Find(*specId, *loadoutId);
            if (!target)
                return SendErr(player, req.seq, "LOADOUT", std::to_string(*loadoutId));
            if (target->active)
            {
                SendLoadout(player, data, *target);
                return SendOk(player, req.seq); // idempotent
            }
            uint32 const level = player->GetLevel();
            bool const live = *specId == st.activeSpec;
            if (live)
            {
                if (char const* code = StateError(player, st))
                    return SendErr(player, req.seq, code);
                RuleResult const result = data.Validate(*spec, data.LoadoutString(*spec, target->pairs), level);
                if (!result.ok())
                    return SendErr(player, req.seq, errorCode(result.code), std::to_string(result.detail));
            }
            uint32 const guid = player->GetGUID().GetCounter();
            CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
            Loadout* old = st.Active(*specId);
            if (old)
            {
                old->active = false;
                SaveLoadoutRow(trans, guid, *old);
            }
            target->active = true;
            target->hash = data.dataHash;
            target->updated = static_cast<uint32>(std::time(nullptr));
            SaveLoadoutRow(trans, guid, *target);
            if (live)
                Reconcile(player, st, data, trans);
            Commit(trans);
            if (live)
                SendState(player, &st, &data);
            if (old)
                SendLoadout(player, data, *old);
            SendLoadout(player, data, *target);
            SendOk(player, req.seq);
        }

        // DEL;specId;loadoutId: deletes a saved loadout, never a spec's active one (ERR LOADOUT). GONE, OK.
        void HandleDel(Player* player, PlayerState& st, EfTalentModel const& data, eft::Request const& req)
        {
            if (req.fields.size() < 2)
                return SendErr(player, req.seq, "DECODE");
            auto const specId = eft::number(req.fields[0], 65535);
            auto const loadoutId = eft::number(req.fields[1], kMaxLoadoutId);
            if (!specId || !loadoutId)
                return SendErr(player, req.seq, "DECODE");
            if (!LoadoutSpec(player, st, data, req, *specId))
                return;
            Loadout* target = st.Find(*specId, *loadoutId);
            if (!target || target->active)
                return SendErr(player, req.seq, "LOADOUT", std::to_string(*loadoutId));
            st.loadouts.erase(st.loadouts.begin() + (target - st.loadouts.data()));
            CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
            trans->Append("DELETE FROM `ef_character_loadout` WHERE `guid` = {} AND `spec` = {} AND `loadout` = {}", player->GetGUID().GetCounter(),
                *specId, *loadoutId);
            Commit(trans);
            SendBody(player, eft::body(0, "GONE", {std::to_string(*specId), std::to_string(*loadoutId)}));
            SendOk(player, req.seq);
        }

        // APPLY;specId;part/total;ranks and RESET;specId;c|s|cs.
        void HandleApply(Player* player, PlayerState& st, EfTalentModel const& data, eft::Request const& req, bool reset)
        {
            std::string loadout;
            uint32 specId = 0;
            if (reset)
            {
                if (req.fields.size() < 2)
                    return SendErr(player, req.seq, "DECODE");
                auto const id = eft::number(req.fields[0], 65535);
                std::string const& which = req.fields[1];
                if (!id || (which != "c" && which != "s" && which != "cs"))
                    return SendErr(player, req.seq, "DECODE");
                specId = *id;
            }
            else
            {
                if (req.fields.size() < 3)
                    return SendErr(player, req.seq, "DECODE");
                auto const id = eft::number(req.fields[0], 65535);
                auto const part = eft::partOf(req.fields[1]);
                if (!id)
                    return SendErr(player, req.seq, "DECODE");
                if (!part)
                    return SendErr(player, req.seq, "CHUNK");
                specId = *id;
                switch (st.chunks.feed(req.seq, part->first, part->second, req.fields[2], NowMs()))
                {
                    case eft::Reassembly::Status::Incomplete:
                        return; // answered with the last part
                    case eft::Reassembly::Status::Error:
                        return SendErr(player, req.seq, "CHUNK");
                    case eft::Reassembly::Status::Complete:
                        loadout = st.chunks.payload();
                        break;
                }
            }
            if (!st.hello)
                return SendErr(player, req.seq, "DATA");
            if (char const* code = StateError(player, st))
                return SendErr(player, req.seq, code);
            if (!st.activeSpec)
                return SendErr(player, req.seq, "NOSPEC");
            EfSpec const* spec = data.Spec(specId);
            if (!spec || spec->classId != player->getClass() || specId != st.activeSpec)
                return SendErr(player, req.seq, "SPEC", std::to_string(specId));
            uint32 const level = player->GetLevel();
            if (level < spec->availableLevel)
                return SendErr(player, req.seq, "LEVEL", std::to_string(specId));
            if (reset)
            {
                // The active loadout with the class tree, the spec tree or both emptied.
                Loadout const* current = st.Active(specId);
                eft::Pairs pairs = current ? current->pairs : eft::Pairs{};
                EfClass const* cls = data.Class(spec->classId);
                std::string const& which = req.fields[1];
                for (auto it = pairs.begin(); it != pairs.end();)
                {
                    EfNode const* n = data.Node(it->first);
                    bool const inClass = n && cls && n->tree == cls->classTree;
                    bool const drop = !n || (inClass ? which.find('c') != std::string::npos : which.find('s') != std::string::npos);
                    it = drop ? pairs.erase(it) : std::next(it);
                }
                loadout = data.LoadoutString(*spec, pairs);
            }
            RuleResult const result = data.Validate(*spec, loadout, level);
            if (!result.ok())
                return SendErr(player, req.seq, errorCode(result.code), result.code == RuleError::Decode || result.code == RuleError::BudgetClass ||
                    result.code == RuleError::BudgetSpec ? std::string() : std::to_string(result.detail));
            std::optional<eft::Pairs> pairs = data.PairsOf(*spec, loadout);
            if (!pairs)
                return SendErr(player, req.seq, "DECODE");
            ApplyPairs(player, st, data, std::move(*pairs));
            SendState(player, &st, &data);
            if (Loadout const* l = st.Active(specId))
                SendLoadout(player, data, *l);
            SendOk(player, req.seq);
        }
    }

    // ---- public ------------------------------------------------------------------------------------------

    bool Enabled() { return s_enabled; }

    void ReadConfig(bool reload)
    {
        bool const was = s_enabled;
        s_enabled = sConfigMgr->GetOption<bool>("Emberfall.Talents.Enable", false, false);
        // `.reload config` that turns the system on or off: (un)load the trees now (at startup Load() runs in
        // OnBeforeWorldInitialized). Online players follow at their next request (generation).
        if (reload && was != s_enabled)
        {
            LOG_INFO("server.loading", ">> Emberfall talents: Emberfall.Talents.Enable {} -> {} (config reload)", was ? 1 : 0, s_enabled ? 1 : 0);
            Load();
        }
    }

    std::shared_ptr<EfTalentModel const> Data() { return s_enabled ? s_data : nullptr; }

    bool Active(Player const* player)
    {
        return player && s_enabled && s_data && s_data->HasClass(player->getClass());
    }

    bool Load()
    {
        ++s_generation;
        s_data.reset();
        if (!s_enabled)
        {
            LOG_INFO("server.loading", ">> Emberfall talents: off (Emberfall.Talents.Enable = 0)");
            return false;
        }
        static char const* const tables[] = {"ef_talent_meta", "ef_talent_points", "ef_spec", "ef_spec_spell", "ef_talent_tree",
            "ef_talent_gate", "ef_talent_node", "ef_talent_node_spell", "ef_talent_edge", "ef_talent_free"};
        uint32 found = 0;
        if (QueryResult r = WorldDatabase.Query("SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME IN "
            "('ef_talent_meta', 'ef_talent_points', 'ef_spec', 'ef_spec_spell', 'ef_talent_tree', 'ef_talent_gate', 'ef_talent_node', "
            "'ef_talent_node_spell', 'ef_talent_edge', 'ef_talent_free')"))
            found = static_cast<uint32>(r->Fetch()[0].Get<uint64>());
        if (found != std::size(tables))
        {
            LOG_ERROR("server.loading", ">> Emberfall talents: {} of the {} ef_* world tables exist (modules/mod-emberfall/data/sql/db-world); "
                "the system stays off", found, std::size(tables));
            return false;
        }

        auto data = std::make_shared<EfTalentModel>();
        std::vector<std::string> problems;
        if (QueryResult r = WorldDatabase.Query("SELECT `name`, `value` FROM `ef_talent_meta`"))
        {
            do
            {
                Field* f = r->Fetch();
                std::string const name = f[0].Get<std::string>();
                std::string const value = f[1].Get<std::string>();
                if (name == "data_hash")
                {
                    data->dataHash = value;
                    continue;
                }
                auto const v = eft::number(value);
                if (!v)
                {
                    problems.push_back("ef_talent_meta " + name + ": '" + value + "' is not a number");
                    continue;
                }
                if (name == "schema") data->schema = *v;
                else if (name == "spec_level") data->specLevel = *v;
                else if (name == "max_rank") data->maxRank = *v;
                else if (name == "max_loadouts") data->maxLoadouts = *v;
                else if (name == "switch_cast_ms") data->switchCastMs = *v;
                else if (name == "switch_spell") data->switchSpell = *v;
            } while (r->NextRow());
        }
        if (QueryResult r = WorldDatabase.Query("SELECT `class`, `level`, `class_points`, `spec_points` FROM `ef_talent_points`"))
        {
            do
            {
                Field* f = r->Fetch();
                data->points[f[0].Get<uint8>()][f[1].Get<uint8>()] = {f[2].Get<uint16>(), f[3].Get<uint16>()};
            } while (r->NextRow());
        }
        if (QueryResult r = WorldDatabase.Query("SELECT `id`, `class`, `spec_key`, `role`, `sort`, `available_level`, `skill_line` FROM `ef_spec`"))
        {
            do
            {
                Field* f = r->Fetch();
                EfSpec s;
                s.id = f[0].Get<uint16>();
                s.classId = f[1].Get<uint8>();
                s.key = f[2].Get<std::string>();
                s.role = f[3].Get<uint8>();
                s.sort = f[4].Get<uint8>();
                s.availableLevel = f[5].Get<uint8>();
                s.skillLine = f[6].Get<uint16>();
                data->specs[s.id] = s;
            } while (r->NextRow());
        }
        if (QueryResult r = WorldDatabase.Query("SELECT `spec`, `spell`, `level` FROM `ef_spec_spell`"))
        {
            do
            {
                Field* f = r->Fetch();
                uint32 const spec = f[0].Get<uint16>();
                auto it = data->specs.find(spec);
                if (it == data->specs.end())
                    problems.push_back("ef_spec_spell: unknown spec " + std::to_string(spec));
                else
                    it->second.baseline.emplace_back(f[1].Get<uint32>(), f[2].Get<uint8>());
            } while (r->NextRow());
        }
        if (QueryResult r = WorldDatabase.Query("SELECT `id`, `class`, `spec` FROM `ef_talent_tree`"))
        {
            do
            {
                Field* f = r->Fetch();
                EfTree t;
                t.id = f[0].Get<uint16>();
                t.classId = f[1].Get<uint8>();
                t.specId = f[2].Get<uint16>();
                data->trees[t.id] = t;
            } while (r->NextRow());
        }
        if (QueryResult r = WorldDatabase.Query("SELECT `tree`, `row`, `points` FROM `ef_talent_gate`"))
        {
            do
            {
                Field* f = r->Fetch();
                uint32 const tree = f[0].Get<uint16>();
                auto it = data->trees.find(tree);
                if (it == data->trees.end())
                    problems.push_back("ef_talent_gate: unknown tree " + std::to_string(tree));
                else
                    it->second.gates.push_back(RuleGate{f[1].Get<uint8>(), f[2].Get<uint16>()});
            } while (r->NextRow());
        }
        std::map<uint32, EfNode> nodes;
        if (QueryResult r = WorldDatabase.Query("SELECT `id`, `tree`, `ord`, `row`, `col`, `kind`, `max_rank`, `required_level`, `replaces` FROM `ef_talent_node`"))
        {
            do
            {
                Field* f = r->Fetch();
                EfNode n;
                n.id = f[0].Get<uint32>();
                n.tree = f[1].Get<uint16>();
                n.ord = f[2].Get<uint16>();
                n.row = f[3].Get<uint8>();
                n.col = f[4].Get<float>();
                uint32 const kind = f[5].Get<uint8>();
                if (kind < 1 || kind > 3)
                    problems.push_back("ef_talent_node " + std::to_string(n.id) + ": kind " + std::to_string(kind));
                n.kind = static_cast<NodeKind>(kind);
                n.maxRank = f[6].Get<uint8>();
                n.requiredLevel = f[7].Get<uint8>();
                n.replaces = f[8].Get<uint32>();
                nodes[n.id] = n;
            } while (r->NextRow());
        }
        if (QueryResult r = WorldDatabase.Query("SELECT `node`, `option`, `rank`, `spell` FROM `ef_talent_node_spell`"))
        {
            do
            {
                Field* f = r->Fetch();
                uint32 const node = f[0].Get<uint32>(), option = f[1].Get<uint8>(), rank = f[2].Get<uint8>(), spell = f[3].Get<uint32>();
                auto it = nodes.find(node);
                if (it == nodes.end() || rank < 1 || rank > kMaxWireRank || option > kMaxOptions)
                {
                    problems.push_back("ef_talent_node_spell: node " + std::to_string(node) + " option " + std::to_string(option) + " rank " +
                                       std::to_string(rank) + " is not a node's rank or option");
                    continue;
                }
                auto& sets = option ? it->second.options : it->second.ranks;
                uint32 const index = option ? option : rank;
                if (sets.size() < index)
                    sets.resize(index);
                sets[index - 1].push_back(spell);
                if (!sSpellMgr->GetSpellInfo(spell))
                    problems.push_back("node " + std::to_string(node) + ": spell " + std::to_string(spell) + " does not exist");
            } while (r->NextRow());
        }
        if (QueryResult r = WorldDatabase.Query("SELECT `parent`, `child` FROM `ef_talent_edge`"))
        {
            do
            {
                Field* f = r->Fetch();
                uint32 const parent = f[0].Get<uint32>(), child = f[1].Get<uint32>();
                auto it = nodes.find(child);
                if (it == nodes.end() || !nodes.count(parent))
                    problems.push_back("ef_talent_edge: " + std::to_string(parent) + " -> " + std::to_string(child) + " names an unknown node");
                else
                    it->second.parents.push_back(parent);
            } while (r->NextRow());
        }
        if (QueryResult r = WorldDatabase.Query("SELECT `node`, `spec` FROM `ef_talent_free`"))
        {
            do
            {
                Field* f = r->Fetch();
                uint32 const node = f[0].Get<uint32>();
                auto it = nodes.find(node);
                if (it == nodes.end())
                    problems.push_back("ef_talent_free: unknown node " + std::to_string(node));
                else
                    it->second.freeSpecs.push_back(f[1].Get<uint16>());
            } while (r->NextRow());
        }
        for (auto& [id, n] : nodes)
        {
            auto it = data->trees.find(n.tree);
            if (it == data->trees.end())
                problems.push_back("ef_talent_node " + std::to_string(id) + ": unknown tree " + std::to_string(n.tree));
            else
                it->second.nodes.push_back(std::move(n));
        }
        for (auto const& [id, spec] : data->specs)
            for (auto const& [spell, level] : spec.baseline)
                if (!sSpellMgr->GetSpellInfo(spell))
                    problems.push_back("spec " + std::to_string(id) + ": baseline spell " + std::to_string(spell) + " does not exist");
        // The switch cast (§1.5): our content's spells/engine.yaml, or the stock-test profile's spell_dbc row.
        if (!data->specs.empty())
        {
            SpellInfo const* sw = data->switchSpell ? sSpellMgr->GetSpellInfo(data->switchSpell) : nullptr;
            if (!sw)
                problems.push_back("switch_spell " + std::to_string(data->switchSpell) + " does not exist (Spell.dbc or spell_dbc)");
            else if (sw->CastTimeEntry && static_cast<uint32>(std::max(sw->CastTimeEntry->CastTime, 0)) != data->switchCastMs)
                LOG_WARN("server.loading", ">> Emberfall talents: switch_spell {} casts {} ms, switch_cast_ms says {} (the spell's time is used)",
                    data->switchSpell, sw->CastTimeEntry->CastTime, data->switchCastMs);
        }
        data->Finish(problems);

        if (data->specs.empty() && data->trees.empty())
        {
            LOG_INFO("server.loading", ">> Emberfall talents: on, no trees in ef_* (no class uses the system; stock talents everywhere)");
            s_data = data;
            return true;
        }
        std::string const computed = HashHex(data->CanonicalJson());
        if (computed != data->dataHash)
            problems.push_back("the tables give data hash " + computed + ", ef_talent_meta says '" + data->dataHash + "'");
        if (!problems.empty())
        {
            for (std::string const& p : problems)
                LOG_ERROR("server.loading", ">> Emberfall talents: {}", p);
            LOG_ERROR("server.loading", ">> Emberfall talents: {} problem(s) in the ef_* tables; the system stays off", problems.size());
            return false;
        }
        for (auto const& [classId, cls] : data->classes)
        {
            EfTree const* ct = data->Tree(cls.classTree);
            std::string specs;
            for (uint32 s : cls.specs)
            {
                EfSpec const* spec = data->Spec(s);
                EfTree const* st = data->Tree(spec->treeId);
                specs += fmt::format("{}{} {} (tree {}, {} nodes, {} baseline)", specs.empty() ? "" : ", ", s, spec->key, spec->treeId,
                    st ? st->nodes.size() : 0, spec->baseline.size());
            }
            LOG_INFO("server.loading", ">> Emberfall talents: class {}: class tree {} ({} nodes, {} gates); specs {}", classId, cls.classTree,
                ct ? ct->nodes.size() : 0, ct ? ct->gates.size() : 0, specs);
        }
        LOG_INFO("server.loading", ">> Emberfall talents: {} classes, {} specs, {} trees, {} nodes, data hash {}; spec level {}",
            data->classes.size(), data->specs.size(), data->trees.size(), data->nodeIndex.size(), data->dataHash, data->specLevel);
        s_data = data;
        return true;
    }

    void OnLogin(Player* player)
    {
        State(player);
    }

    void OnLevelChanged(Player* player, uint8 oldLevel)
    {
        std::shared_ptr<EfTalentModel const> data = Data();
        PlayerState* st = State(player);
        if (!st || !data)
            return;
        // Revalidate resets what no longer fits a lower level (§1.5) and reconciles: baseline spells that
        // reached their level are learned.
        Revalidate(player, *st, *data, true);
        if (st->hello)
            SendState(player, st, data.get());
        LOG_DEBUG("module", "Emberfall talents: {} level {} -> {}", player->GetName(), oldLevel, player->GetLevel());
    }

    void HandleMessage(Player* player, std::string_view body)
    {
        eft::ParseResult const parsed = eft::parse(body);
        if (parsed.error == eft::ParseError::Version)
            return SendErr(player, parsed.seq, "VERSION");
        if (!s_enabled)
            return SendErr(player, parsed.seq, "DISABLED");
        std::shared_ptr<EfTalentModel const> data = Data();
        PlayerState* st = State(player);
        if (st)
        {
            uint64 const now = NowMs();
            if (!st->requests.allow(now))
            {
                if (now - st->throttleAnsweredAt >= kRequestWindowMs)
                {
                    st->throttleAnsweredAt = now;
                    SendErr(player, parsed.seq, "THROTTLE");
                }
                return;
            }
        }
        if (!parsed.ok())
            return SendErr(player, parsed.seq, "DECODE");
        eft::Request const& req = parsed.request;
        if (req.cmd == "HELLO")
            return HandleHello(player, st, data.get(), req);
        if (req.cmd != "SPEC" && req.cmd != "APPLY" && req.cmd != "RESET" && req.cmd != "SAVE" && req.cmd != "LOAD" && req.cmd != "DEL")
        {
            // Unknown commands get no answer (§4.5).
            LOG_DEBUG("module", "Emberfall talents: {} sent an unknown EFT command '{}'", player->GetName(), req.cmd);
            return;
        }
        if (!st || !data)
            return SendErr(player, req.seq, "DISABLED");
        if (req.cmd == "SPEC")
        {
            if (!st->specRequests.allow(NowMs()))
                return SendErr(player, req.seq, "THROTTLE");
            if (!st->hello)
                return SendErr(player, req.seq, "DATA");
            return HandleSpec(player, *st, *data, req);
        }
        if (req.cmd == "SAVE")
            return HandleSave(player, *st, *data, req);
        if (req.cmd == "LOAD")
            return HandleLoad(player, *st, *data, req);
        if (req.cmd == "DEL")
            return HandleDel(player, *st, *data, req);
        return HandleApply(player, *st, *data, req, req.cmd == "RESET");
    }

    void OnUpdate(Player* player)
    {
        if (!s_pendingSwitches.load(std::memory_order_relaxed))
            return;
        PlayerState* st = player->CustomData.Get<PlayerState>(kStateKey);
        if (!st || !st->pending.spec)
            return;
        if (!st->pending.cast)
        {
            Spell const* cast = player->GetCurrentSpell(CURRENT_GENERIC_SPELL);
            if (cast && cast->GetSpellInfo()->Id == st->pending.spell)
                return; // still casting
        }
        FinishSwitch(player, *st);
    }

    void OnSpellCast(Player* player, uint32 spellId)
    {
        if (!s_pendingSwitches.load(std::memory_order_relaxed))
            return;
        PlayerState* st = player->CustomData.Get<PlayerState>(kStateKey);
        if (st && st->pending.spec && st->pending.spell == spellId && !player->IsInCombat())
            st->pending.cast = true; // switched at the next OnUpdate, outside the spell's own processing
    }

    void OnEnterCombat(Player* player)
    {
        if (!s_pendingSwitches.load(std::memory_order_relaxed))
            return;
        PlayerState* st = player->CustomData.Get<PlayerState>(kStateKey);
        if (!st || !st->pending.spec || st->pending.cast)
            return;
        st->pending.combat = true;
        Spell const* cast = player->GetCurrentSpell(CURRENT_GENERIC_SPELL);
        if (cast && cast->GetSpellInfo()->Id == st->pending.spell)
            player->InterruptSpell(CURRENT_GENERIC_SPELL, false);
    }

    void Dump(Player* player, std::vector<std::string>& out)
    {
        std::shared_ptr<EfTalentModel const> data = Data();
        if (!s_enabled)
        {
            out.push_back("Emberfall talents: off (Emberfall.Talents.Enable = 0)");
            return;
        }
        PlayerState* st = State(player);
        if (!st || !data)
        {
            out.push_back(fmt::format("Emberfall talents: class {} of {} has no trees (stock talents)", player->getClass(), player->GetName()));
            return;
        }
        auto const [cb, sb] = data->Budget(player->getClass(), player->GetLevel());
        out.push_back(fmt::format("Emberfall talents: {} (guid {}), class {}, level {}: active spec {}, budgets class {} spec {}, data hash {} "
            "(stored {}), hello {}", player->GetName(), player->GetGUID().GetCounter(), player->getClass(), player->GetLevel(), st->activeSpec,
            cb, sb, data->dataHash, st->dataHash, st->hello ? "yes" : "no"));
        for (Loadout const& l : st->loadouts)
        {
            EfSpec const* spec = data->Spec(l.spec);
            out.push_back(fmt::format("  loadout spec {} #{} '{}'{}: {} [{}]", l.spec, l.id, l.name, l.active ? " (active)" : "",
                spec ? data->LoadoutString(*spec, l.pairs) : "?", eft::encodePairs(l.pairs)));
        }
        std::string granted;
        for (auto const& [spell, source] : st->granted)
            granted += fmt::format("{}{}{}", granted.empty() ? "" : ",", spell, source == 1 ? "b" : "");
        out.push_back(fmt::format("  granted ({}): {}", st->granted.size(), granted.empty() ? "-" : granted));
        // Auras the player owns from any spell of the class's trees or baselines (every spec), or triggered by one:
        // after an apply or a switch only the active loadout's may be left (§3.3).
        std::set<uint32> classSpells;
        if (EfClass const* cls = data->Class(player->getClass()))
        {
            std::vector<uint32> trees{cls->classTree};
            for (uint32 s : cls->specs)
                if (EfSpec const* spec = data->Spec(s))
                {
                    trees.push_back(spec->treeId);
                    for (auto const& [spell, level] : spec->baseline)
                        classSpells.insert(spell);
                }
            for (uint32 t : trees)
                if (EfTree const* tree = data->Tree(t))
                    for (EfNode const& n : tree->nodes)
                    {
                        for (auto const& set : n.ranks)
                            classSpells.insert(set.begin(), set.end());
                        for (auto const& set : n.options)
                            classSpells.insert(set.begin(), set.end());
                    }
        }
        std::set<uint32> auras;
        for (auto const& [id, aura] : player->GetOwnedAuras())
        {
            SpellInfo const* by = aura->GetTriggeredByAuraSpellInfo();
            if (classSpells.count(id) || (by && classSpells.count(by->Id)))
                auras.insert(id);
        }
        std::string auraList;
        for (uint32 id : auras)
            auraList += fmt::format("{}{}", auraList.empty() ? "" : ",", id);
        out.push_back(fmt::format("  auras ({}): {}", auras.size(), auraList.empty() ? "-" : auraList));
        std::string bars;
        for (auto const& [spec, buttons] : st->bars)
            bars += fmt::format("{}spec {} {} buttons", bars.empty() ? "" : ", ", spec, buttons.size());
        out.push_back(fmt::format("  flags {}, switch {}, stored bars: {}", st->flags,
            st->pending.spec ? fmt::format("to {} (seq {})", st->pending.spec, st->pending.seq) : std::string("none"), bars.empty() ? "-" : bars));
    }

    bool GmSetSpec(Player* player, uint32 specId, std::string& out)
    {
        std::shared_ptr<EfTalentModel const> data = Data();
        PlayerState* st = State(player);
        if (!st || !data)
        {
            out = "the system is off or the class has no trees";
            return false;
        }
        EfSpec const* spec = data->Spec(specId);
        if (!spec || spec->classId != player->getClass())
        {
            out = fmt::format("spec {} is not a spec of class {}", specId, player->getClass());
            return false;
        }
        if (st->pending.spec)
        {
            // A GM switch replaces a running switch cast (its request gets ERR STATE).
            PendingSwitch const p = st->EndSwitch();
            if (Spell const* cast = player->GetCurrentSpell(CURRENT_GENERIC_SPELL); cast && cast->GetSpellInfo()->Id == p.spell)
                player->InterruptSpell(CURRENT_GENERIC_SPELL, false);
            if (st->hello)
                SendErr(player, p.seq, "STATE");
        }
        EnterSpec(player, *st, *data, specId);
        if (st->hello)
        {
            SendState(player, st, data.get());
            if (Loadout const* l = st->Active(specId))
                SendLoadout(player, *data, *l);
        }
        out = fmt::format("{}: active spec {} ({})", player->GetName(), specId, spec->key);
        return true;
    }

    bool GmReset(Player* player, std::string& out)
    {
        std::shared_ptr<EfTalentModel const> data = Data();
        PlayerState* st = State(player);
        if (!st || !data || !st->activeSpec)
        {
            out = "no active spec";
            return false;
        }
        ApplyPairs(player, *st, *data, {});
        if (st->hello)
        {
            SendBody(player, eft::body(0, "RESET", {std::to_string(st->activeSpec), "GM"}));
            SendState(player, st, data.get());
            if (Loadout const* l = st->Active(st->activeSpec))
                SendLoadout(player, *data, *l);
        }
        out = fmt::format("{}: the active loadout of spec {} is reset", player->GetName(), st->activeSpec);
        return true;
    }
}
