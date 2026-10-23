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

// Emberfall profession specialisations (Emberfall docs/design/professions.md §10): the loaded trees and the pure
// functions over them, with no dependency on the core, so tests/profspec_tests.cpp runs them standalone.
//
//   trees      three per profession (ef_prof_tree.sort 1-3), nodes in `ord` order; a node opens when every rank of a
//              parent is bought and the tree has `gate` points spent on the nodes of the rows above (SV's tiers)
//   rules      the talent system's TalentRules.h (checkTree: rank, path, gate) per tree, then one budget for the
//              profession's trees: skill / points_per_skill; and a stored rank never goes down (SV gathering-specs.md
//              §2: points come back only with a whole tree, at the trainer)
//   wire       a profession's ranks as `<tree 1>.<tree 2>.<tree 3>`, one base-36 digit (0-9, a-z) per node in `ord`
//              order, so a node may hold up to 35 ranks
//   effects    a node's effect, per_rank and targets (items, families, spells); Sum() adds per_rank x rank over the
//              invested nodes of one kind that apply, only for nodes that work with today's content (`active`)

#pragma once

#include "../talents/TalentRules.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace ef::prof
{
    enum class Effect : uint8_t
    {
        None = 0,
        Yield = 1,
        MinimapRange = 2,
        RarestFirst = 3,
        GatherSpeed = 4,
        CraftExtra = 5,
        CraftBatch = 6,
        Duration = 7,
        Discovery = 8,
    };

    // ef_prof_node_target.kind
    constexpr uint8_t kTargetItem = 1, kTargetFamily = 2, kTargetSpell = 3;
    constexpr uint32_t kMaxRank = 35;

    struct Node
    {
        uint32_t id = 0, tree = 0, ord = 0, row = 0, maxRank = 1, gate = 0;
        std::vector<uint32_t> parents; // node IDs
        Effect effect = Effect::None;
        uint32_t perRank = 0, extra = 0;
        bool active = false;
        std::vector<uint32_t> spells;  // rank 1..maxRank: the passive spell known at that rank
        std::set<uint32_t> items, families, targetSpells;
    };

    struct Tree
    {
        uint32_t id = 0, skill = 0, sort = 0;
        std::string name;
        std::vector<Node> nodes; // in `ord` order

        talents::RuleTree Rules() const
        {
            talents::RuleTree out;
            std::map<uint32_t, int32_t> ords;
            for (Node const& n : nodes)
                ords[n.id] = static_cast<int32_t>(n.ord);
            for (Node const& n : nodes)
            {
                talents::RuleNode r;
                r.id = n.id;
                r.row = n.row;
                r.kind = talents::NodeKind::Passive;
                r.maxRank = static_cast<uint8_t>(std::min<uint32_t>(n.maxRank, kMaxRank));
                r.gate = n.gate;
                for (size_t k = 0; k < n.parents.size() && k < talents::kMaxParents; ++k)
                {
                    auto const it = ords.find(n.parents[k]);
                    r.parents[k] = it == ords.end() ? -1 : it->second;
                }
                out.nodes.push_back(r);
            }
            return out;
        }
    };

    // node ID -> rank (invested nodes only)
    using Ranks = std::map<uint32_t, uint32_t>;

    inline int DigitValue(char c)
    {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'z')
            return c - 'a' + 10;
        return -1;
    }

    inline char Digit(uint32_t v) { return v < 10 ? static_cast<char>('0' + v) : static_cast<char>('a' + std::min<uint32_t>(v, kMaxRank) - 10); }

    struct Check
    {
        std::string code; // empty: valid; else DECODE, RANK, PATH, GATE, BUDGET, LOWER, SKILL
        uint32_t detail = 0;
        bool ok() const { return code.empty(); }
    };

    class Model
    {
    public:
        std::string hash;
        uint32_t pointsPerSkill = 10, feePerPoint = 0;
        std::map<uint32_t, Tree> trees;
        struct Recipe
        {
            uint32_t skill = 0, family = 0, item = 0;
        };
        std::map<uint32_t, Recipe> recipes;                       // spell -> what it is
        std::map<uint32_t, uint32_t> trainers;                    // creature entry -> skill line

        // Builds the indexes; call after filling `trees` (nodes sorted by ord).
        void Index()
        {
            m_bySkill.clear();
            m_nodes.clear();
            for (auto& [id, t] : trees)
            {
                std::sort(t.nodes.begin(), t.nodes.end(), [](Node const& a, Node const& b) { return a.ord < b.ord; });
                m_bySkill[t.skill].push_back(id);
                for (size_t i = 0; i < t.nodes.size(); ++i)
                    m_nodes[t.nodes[i].id] = {id, i};
            }
            for (auto& [skill, ids] : m_bySkill)
                std::sort(ids.begin(), ids.end(), [&](uint32_t a, uint32_t b) { return trees.at(a).sort < trees.at(b).sort; });
        }

        bool HasSkill(uint32_t skill) const { return m_bySkill.count(skill) != 0; }
        std::vector<uint32_t> Skills() const
        {
            std::vector<uint32_t> out;
            for (auto const& [skill, ids] : m_bySkill)
                out.push_back(skill);
            return out;
        }
        std::vector<Tree const*> TreesOf(uint32_t skill) const
        {
            std::vector<Tree const*> out;
            if (auto const it = m_bySkill.find(skill); it != m_bySkill.end())
                for (uint32_t id : it->second)
                    out.push_back(&trees.at(id));
            return out;
        }
        Node const* NodeById(uint32_t id) const
        {
            auto const it = m_nodes.find(id);
            return it == m_nodes.end() ? nullptr : &trees.at(it->second.first).nodes[it->second.second];
        }
        Tree const* TreeOfNode(uint32_t id) const
        {
            auto const it = m_nodes.find(id);
            return it == m_nodes.end() ? nullptr : &trees.at(it->second.first);
        }
        uint32_t SkillOfNode(uint32_t id) const
        {
            Tree const* t = TreeOfNode(id);
            return t ? t->skill : 0;
        }

        uint32_t Budget(uint32_t skillValue) const { return pointsPerSkill ? skillValue / pointsPerSkill : 0; }

        // Points spent in one profession (or one tree).
        uint32_t Spent(uint32_t skill, Ranks const& ranks) const
        {
            uint32_t sum = 0;
            for (auto const& [node, rank] : ranks)
                if (SkillOfNode(node) == skill)
                    sum += rank;
            return sum;
        }
        uint32_t SpentInTree(uint32_t tree, Ranks const& ranks) const
        {
            uint32_t sum = 0;
            for (auto const& [node, rank] : ranks)
                if (Tree const* t = TreeOfNode(node); t && t->id == tree)
                    sum += rank;
            return sum;
        }

        // The wire string of a profession's ranks.
        std::string Encode(uint32_t skill, Ranks const& ranks) const
        {
            std::string out;
            std::vector<Tree const*> const list = TreesOf(skill);
            for (size_t i = 0; i < list.size(); ++i)
            {
                Tree const* t = list[i];
                if (i)
                    out += '.';
                for (Node const& n : t->nodes)
                {
                    auto const it = ranks.find(n.id);
                    out += Digit(it == ranks.end() ? 0 : it->second);
                }
            }
            return out;
        }

        // A wire string -> the profession's ranks (only invested nodes), or nothing when malformed.
        std::optional<Ranks> Decode(uint32_t skill, std::string_view text) const
        {
            std::vector<Tree const*> const list = TreesOf(skill);
            if (list.empty())
                return std::nullopt;
            Ranks out;
            size_t pos = 0;
            for (size_t i = 0; i < list.size(); ++i)
            {
                size_t const end = i + 1 < list.size() ? text.find('.', pos) : text.size();
                if (end == std::string_view::npos)
                    return std::nullopt;
                std::string_view const part = text.substr(pos, end - pos);
                if (part.size() != list[i]->nodes.size())
                    return std::nullopt;
                for (size_t k = 0; k < part.size(); ++k)
                {
                    int const v = DigitValue(part[k]);
                    if (v < 0)
                        return std::nullopt;
                    if (v > 0)
                        out[list[i]->nodes[k].id] = static_cast<uint32_t>(v);
                }
                pos = end + 1;
            }
            if (pos != text.size() + 1)
                return std::nullopt;
            return out;
        }

        // The rules for a profession's ranks at `skillValue` (TalentRules per tree, then the budget); with `stored`, no
        // node may be lower than it was (LOWER, detail the node).
        Check Validate(uint32_t skill, Ranks const& ranks, uint32_t skillValue, Ranks const* stored = nullptr) const
        {
            if (!HasSkill(skill))
                return {"SKILL", skill};
            for (auto const& [node, rank] : ranks)
                if (SkillOfNode(node) != skill)
                    return {"DECODE", node};
            if (stored)
                for (auto const& [node, rank] : *stored)
                    if (SkillOfNode(node) == skill)
                    {
                        auto const it = ranks.find(node);
                        if (it == ranks.end() || it->second < rank)
                            return {"LOWER", node};
                    }
            for (Tree const* t : TreesOf(skill))
            {
                talents::RuleTree const rules = t->Rules();
                std::vector<talents::NodeValue> values(t->nodes.size());
                for (size_t i = 0; i < t->nodes.size(); ++i)
                    if (auto const it = ranks.find(t->nodes[i].id); it != ranks.end())
                    {
                        if (it->second > kMaxRank)
                            return {"RANK", t->nodes[i].id};
                        values[i].rank = static_cast<uint8_t>(it->second);
                    }
                talents::RuleResult const r = talents::checkTree(rules, values, 0xFFFFFFFFu, 0);
                if (!r.ok())
                    return {talents::errorCode(r.code), r.detail};
            }
            if (Spent(skill, ranks) > Budget(skillValue))
                return {"BUDGET", Budget(skillValue)};
            return {};
        }

        // One profession's part of a character's ranks (they hold every profession's nodes).
        Ranks Of(uint32_t skill, Ranks const& all) const
        {
            Ranks out;
            for (auto const& [node, rank] : all)
                if (SkillOfNode(node) == skill)
                    out[node] = rank;
            return out;
        }

        // A character's stored ranks of one profession still hold at `skillValue` (the login check). Validate alone
        // answers DECODE for every other profession's node in `all`: the login wiped the first profession (Alchemy,
        // 171) whenever a second one (Herbalism, Mining) had ranks.
        Check ValidateStored(uint32_t skill, Ranks const& all, uint32_t skillValue) const
        {
            return Validate(skill, Of(skill, all), skillValue);
        }

        // The spells the ranks teach: rank r of a node teaches its r-th spell only.
        std::set<uint32_t> Expected(Ranks const& ranks) const
        {
            std::set<uint32_t> out;
            for (auto const& [node, rank] : ranks)
                if (Node const* n = NodeById(node); n && rank >= 1 && rank <= n->spells.size())
                    out.insert(n->spells[rank - 1]);
            return out;
        }

        // per_rank x rank over the invested nodes of `effect` (of `skill`; 0: any) that work today and whose targets
        // accept `match` (nullptr: every node of the effect).
        template <class Match>
        uint32_t Sum(Ranks const& ranks, Effect effect, uint32_t skill, Match const& match) const
        {
            uint32_t sum = 0;
            for (auto const& [node, rank] : ranks)
            {
                Node const* n = NodeById(node);
                if (!n || n->effect != effect || !n->active || !rank)
                    continue;
                if (skill && SkillOfNode(node) != skill)
                    continue;
                if (match(*n))
                    sum += n->perRank * std::min(rank, n->maxRank);
            }
            return sum;
        }
        uint32_t Sum(Ranks const& ranks, Effect effect, uint32_t skill = 0) const
        {
            return Sum(ranks, effect, skill, [](Node const&) { return true; });
        }

        // The ranks with the nodes of one tree taken out (a reset), and the points it takes back.
        uint32_t WipeTree(uint32_t tree, Ranks& ranks) const
        {
            uint32_t back = 0;
            for (auto it = ranks.begin(); it != ranks.end();)
            {
                Tree const* t = TreeOfNode(it->first);
                if (t && t->id == tree)
                {
                    back += it->second;
                    it = ranks.erase(it);
                }
                else
                    ++it;
            }
            return back;
        }

        // Stored ranks after a data change: unknown nodes dropped, ranks clamped to their node.
        bool Remap(Ranks& ranks) const
        {
            bool changed = false;
            for (auto it = ranks.begin(); it != ranks.end();)
            {
                Node const* n = NodeById(it->first);
                if (!n || !it->second)
                {
                    it = ranks.erase(it);
                    changed = true;
                    continue;
                }
                if (it->second > n->maxRank)
                {
                    it->second = n->maxRank;
                    changed = true;
                }
                ++it;
            }
            return changed;
        }

        // Structural problems of loaded data (empty: fine).
        std::vector<std::string> Problems() const
        {
            std::vector<std::string> out;
            for (auto const& [skill, ids] : m_bySkill)
            {
                for (size_t i = 0; i < ids.size(); ++i)
                    if (trees.at(ids[i]).sort != i + 1)
                        out.push_back("skill " + std::to_string(skill) + ": tree sorts are not 1..n");
            }
            for (auto const& [id, t] : trees)
                for (size_t i = 0; i < t.nodes.size(); ++i)
                {
                    Node const& n = t.nodes[i];
                    if (n.ord != i)
                        out.push_back("tree " + std::to_string(id) + ": node " + std::to_string(n.id) + " has ord " + std::to_string(n.ord));
                    if (n.maxRank < 1 || n.maxRank > kMaxRank || n.spells.size() != n.maxRank)
                        out.push_back("node " + std::to_string(n.id) + ": " + std::to_string(n.spells.size()) + " rank spells for " +
                                      std::to_string(n.maxRank) + " ranks");
                    for (uint32_t p : n.parents)
                        if (Tree const* pt = TreeOfNode(p); !pt || pt->id != id)
                            out.push_back("node " + std::to_string(n.id) + ": parent " + std::to_string(p) + " is not in its tree");
                    if (n.parents.size() > talents::kMaxParents)
                        out.push_back("node " + std::to_string(n.id) + ": more than 4 parents");
                }
            return out;
        }

    private:
        std::map<uint32_t, std::vector<uint32_t>> m_bySkill;              // skill -> tree IDs by sort
        std::map<uint32_t, std::pair<uint32_t, size_t>> m_nodes;          // node -> (tree, index)
    };

    // "nodeID:rank,..." (the characters DB form of the GM dump).
    inline std::string FormatRanks(Ranks const& ranks)
    {
        std::string out;
        for (auto const& [node, rank] : ranks)
            out += (out.empty() ? "" : ",") + std::to_string(node) + ":" + std::to_string(rank);
        return out;
    }
}
