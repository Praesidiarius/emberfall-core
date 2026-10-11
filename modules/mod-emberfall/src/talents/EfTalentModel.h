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

// Emberfall talents (docs/design/talents.md §2.6, §3): the tree data as the module holds it after loading
// the ef_* world tables, and the pure functions over it: budgets, the TalentRules.h view of a tree, rank
// strings <-> stored pairs, the spells a loadout teaches, the structural checks and the canonical JSON the
// data hash is taken over (the same as Emberfall's talents/compile.py structure() and the client's
// EfTalents.cpp). No dependency on the core (tests/ builds it standalone).

#pragma once

#include "EftProtocol.h"
#include "TalentRules.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace ef::talents
{
    struct EfNode
    {
        uint32_t id = 0;
        uint32_t tree = 0;
        uint32_t ord = 0;
        uint32_t row = 0;
        float col = 1.0f;
        NodeKind kind = NodeKind::Passive;
        uint32_t maxRank = 1;
        uint32_t requiredLevel = 0;
        uint32_t replaces = 0;
        std::vector<uint32_t> parents;              // node IDs
        std::vector<std::vector<uint32_t>> ranks;   // [rank - 1] -> the complete spell set (normal nodes)
        std::vector<std::vector<uint32_t>> options; // [option - 1] -> spells (choice nodes)
        uint32_t freeSpecMask = 0;                  // bit (spec sort - 1)
        std::vector<uint32_t> freeSpecs;            // spec IDs (ef_talent_free)
    };

    struct EfTree
    {
        uint32_t id = 0;
        uint32_t classId = 0;
        uint32_t specId = 0;                        // 0 = the class tree
        std::vector<EfNode> nodes;                  // in `ord` order
        std::vector<RuleGate> gates;
        RuleTree rules;                             // TalentRules.h view (parents as `ord` indices)
    };

    struct EfSpec
    {
        uint32_t id = 0;
        uint32_t classId = 0;
        std::string key;
        uint32_t role = 3;
        uint32_t sort = 1;
        uint32_t availableLevel = 0;
        uint32_t skillLine = 0;
        std::vector<std::pair<uint32_t, uint32_t>> baseline; // (spell, level)
        uint32_t treeId = 0;
    };

    struct EfClass
    {
        uint32_t classId = 0;
        uint32_t classTree = 0;
        std::vector<uint32_t> specs;                // spec IDs by sort
    };

    // Spell -> where it comes from: 1 baseline spec spell, 2 a node.
    enum class GrantSource : uint8_t
    {
        Baseline = 1,
        Node = 2,
    };
    using SpellSet = std::map<uint32_t, GrantSource>;

    class EfTalentModel
    {
    public:
        // Meta (ef_talent_meta)
        std::string dataHash;                       // 16 hex digits as stored
        uint32_t schema = 1, specLevel = 10, maxRank = 9, maxLoadouts = 5, switchCastMs = 5000, switchSpell = 0;
        // class (0 = default) -> level -> (class points, spec points)
        std::map<uint32_t, std::map<uint32_t, std::pair<uint32_t, uint32_t>>> points;
        std::map<uint32_t, EfSpec> specs;           // by ID
        std::map<uint32_t, EfTree> trees;           // by ID
        std::map<uint32_t, EfClass> classes;        // by class ID
        std::map<uint32_t, std::pair<uint32_t, uint32_t>> nodeIndex; // node ID -> (tree, ord)

        bool HasClass(uint32_t classId) const { return classes.count(classId) != 0; }
        const EfClass* Class(uint32_t classId) const { auto it = classes.find(classId); return it == classes.end() ? nullptr : &it->second; }
        const EfSpec* Spec(uint32_t id) const { auto it = specs.find(id); return it == specs.end() ? nullptr : &it->second; }
        const EfTree* Tree(uint32_t id) const { auto it = trees.find(id); return it == trees.end() ? nullptr : &it->second; }
        const EfNode* Node(uint32_t id) const
        {
            auto it = nodeIndex.find(id);
            if (it == nodeIndex.end())
                return nullptr;
            const EfTree* t = Tree(it->second.first);
            return t && it->second.second < t->nodes.size() ? &t->nodes[it->second.second] : nullptr;
        }

        uint32_t SpecBit(const EfSpec& spec) const { return spec.sort >= 1 && spec.sort <= 32 ? 1u << (spec.sort - 1) : 0u; }

        // (class points, spec points) at a level: the class's own schedule if it has one, else the default;
        // the last row at or below the level.
        std::pair<uint32_t, uint32_t> Budget(uint32_t classId, uint32_t level) const
        {
            auto it = points.find(classId);
            if (it == points.end() || it->second.empty())
                it = points.find(0);
            if (it == points.end())
                return {0, 0};
            std::pair<uint32_t, uint32_t> best{0, 0};
            for (const auto& [lvl, b] : it->second)
            {
                if (lvl > level)
                    break;
                best = b;
            }
            return best;
        }

        // The rank string of one tree from stored pairs (nodes of other trees are ignored).
        static std::string TreeString(const EfTree& tree, const eft::Pairs& pairs)
        {
            std::vector<NodeValue> values(tree.nodes.size());
            for (size_t i = 0; i < tree.nodes.size(); ++i)
                if (auto it = pairs.find(tree.nodes[i].id); it != pairs.end())
                    values[i] = NodeValue{it->second.rank, it->second.option};
            return encodeRanks(tree.rules, values);
        }

        // `<classRanks>.<specRanks>` of a spec's loadout.
        std::string LoadoutString(const EfSpec& spec, const eft::Pairs& pairs) const
        {
            const EfClass* cls = Class(spec.classId);
            const EfTree* ct = cls ? Tree(cls->classTree) : nullptr;
            const EfTree* st = Tree(spec.treeId);
            if (!ct || !st)
                return ".";
            return TreeString(*ct, pairs) + "." + TreeString(*st, pairs);
        }

        // A wire loadout -> pairs (only after validateLoadout accepted it). Free nodes of the spec are left
        // out: they are full for it whatever the string says.
        std::optional<eft::Pairs> PairsOf(const EfSpec& spec, std::string_view loadout) const
        {
            const uint32_t bit = SpecBit(spec);
            const EfClass* cls = Class(spec.classId);
            const EfTree* ct = cls ? Tree(cls->classTree) : nullptr;
            const EfTree* st = Tree(spec.treeId);
            const size_t dot = loadout.find('.');
            if (!ct || !st || dot == std::string_view::npos)
                return std::nullopt;
            eft::Pairs out;
            for (const auto& [tree, text] : {std::pair{ct, loadout.substr(0, dot)}, std::pair{st, loadout.substr(dot + 1)}})
            {
                const auto values = decodeRanks(tree->rules, text);
                if (!values)
                    return std::nullopt;
                for (size_t i = 0; i < values->size(); ++i)
                    if (((*values)[i].rank != 0 || (*values)[i].option != 0) && !(tree->nodes[i].freeSpecMask & bit))
                        out[tree->nodes[i].id] = eft::Pair{(*values)[i].rank, (*values)[i].option};
            }
            return out;
        }

        // Validates a spec's wire loadout at a level with TalentRules.h.
        RuleResult Validate(const EfSpec& spec, std::string_view loadout, uint32_t level) const
        {
            const EfClass* cls = Class(spec.classId);
            const EfTree* ct = cls ? Tree(cls->classTree) : nullptr;
            const EfTree* st = Tree(spec.treeId);
            if (!ct || !st)
                return {RuleError::Decode, 0};
            const auto [classBudget, specBudget] = Budget(spec.classId, level);
            return validateLoadout(ct->rules, st->rules, loadout, level, classBudget, specBudget, SpecBit(spec));
        }

        // Stored pairs after a data change (§3.7): nodes that left the spec's trees dropped, ranks clamped to
        // the new max rank, options the node no longer has dropped. `changed` says whether anything was.
        eft::Pairs Remap(const EfSpec& spec, const eft::Pairs& pairs, bool& changed) const
        {
            changed = false;
            const EfClass* cls = Class(spec.classId);
            eft::Pairs out;
            for (const auto& [id, p] : pairs)
            {
                const EfNode* n = Node(id);
                if (!n || (n->tree != spec.treeId && (!cls || n->tree != cls->classTree)))
                {
                    changed = true;
                    continue;
                }
                eft::Pair q = p;
                if (n->kind == NodeKind::Choice)
                {
                    if (q.option == 0 || q.option > n->options.size())
                    {
                        changed = true;
                        continue;
                    }
                    q.rank = 1;
                }
                else
                {
                    if (q.option != 0)
                    {
                        changed = true;
                        continue;
                    }
                    if (q.rank > n->maxRank)
                    {
                        q.rank = static_cast<uint8_t>(n->maxRank);
                        changed = true;
                    }
                }
                if (q.rank != 0 || q.option != 0)
                    out[id] = q;
            }
            return out;
        }

        // The spells a character of `level` in `spec` with the loadout `pairs` knows from the system: the
        // spec's baseline spells up to the level, each invested node's current rank or option, free nodes at
        // their max rank (§3.3).
        SpellSet Expected(const EfSpec& spec, const eft::Pairs& pairs, uint32_t level) const
        {
            SpellSet out;
            for (const auto& [spell, lvl] : spec.baseline)
                if (level >= lvl)
                    out[spell] = GrantSource::Baseline;
            const EfClass* cls = Class(spec.classId);
            const uint32_t bit = SpecBit(spec);
            for (const uint32_t treeId : {cls ? cls->classTree : 0u, spec.treeId})
            {
                const EfTree* tree = Tree(treeId);
                if (!tree)
                    continue;
                for (const EfNode& n : tree->nodes)
                {
                    const auto it = pairs.find(n.id);
                    const eft::Pair p = it == pairs.end() ? eft::Pair{} : it->second;
                    const std::vector<uint32_t>* spells = nullptr;
                    if (n.kind == NodeKind::Choice)
                    {
                        if (p.option >= 1 && p.option <= n.options.size())
                            spells = &n.options[p.option - 1];
                    }
                    else
                    {
                        uint32_t rank = (n.freeSpecMask & bit) ? n.maxRank : p.rank;
                        rank = std::min<uint32_t>(rank, static_cast<uint32_t>(n.ranks.size()));
                        if (rank >= 1)
                            spells = &n.ranks[rank - 1];
                    }
                    if (spells)
                        for (const uint32_t s : *spells)
                            out.emplace(s, GrantSource::Node);
                }
            }
            return out;
        }

        // Fills `rules` of every tree, `nodeIndex` and the free masks after the rows are in; structural
        // problems are appended to `problems` (the module then keeps the system off).
        void Finish(std::vector<std::string>& problems)
        {
            nodeIndex.clear();
            for (auto& [id, tree] : trees)
            {
                std::sort(tree.nodes.begin(), tree.nodes.end(), [](const EfNode& a, const EfNode& b) { return a.ord < b.ord; });
                std::sort(tree.gates.begin(), tree.gates.end(), [](const RuleGate& a, const RuleGate& b) { return a.row < b.row; });
                if (tree.nodes.size() > 120)
                    problems.push_back("tree " + std::to_string(id) + ": more than 120 nodes");
                std::map<uint32_t, int32_t> ords;
                for (size_t i = 0; i < tree.nodes.size(); ++i)
                {
                    EfNode& n = tree.nodes[i];
                    if (n.ord != i)
                        problems.push_back("tree " + std::to_string(id) + ": node " + std::to_string(n.id) + " has ord " +
                                           std::to_string(n.ord) + ", expected " + std::to_string(i));
                    ords[n.id] = static_cast<int32_t>(i);
                    nodeIndex[n.id] = {id, static_cast<uint32_t>(i)};
                    n.freeSpecMask = 0;
                    for (const uint32_t s : n.freeSpecs)
                    {
                        const EfSpec* spec = Spec(s);
                        if (!spec || spec->classId != tree.classId || tree.specId != 0)
                            problems.push_back("node " + std::to_string(n.id) + ": free for spec " + std::to_string(s) +
                                               ", which is not a spec of the class of its class tree");
                        else
                            n.freeSpecMask |= SpecBit(*spec);
                    }
                }
                tree.rules = RuleTree{};
                tree.rules.gates = tree.gates;
                for (const EfNode& n : tree.nodes)
                {
                    RuleNode r;
                    r.id = n.id;
                    r.row = n.row;
                    r.kind = n.kind;
                    r.maxRank = static_cast<uint8_t>(std::min<uint32_t>(n.maxRank, 255));
                    r.numOptions = static_cast<uint8_t>(std::min<size_t>(n.options.size(), 255));
                    r.requiredLevel = n.requiredLevel;
                    r.freeSpecMask = n.freeSpecMask;
                    const std::string where = "node " + std::to_string(n.id) + ": ";
                    if (n.parents.size() > kMaxParents)
                        problems.push_back(where + "more than 4 parents");
                    for (size_t p = 0; p < n.parents.size() && p < kMaxParents; ++p)
                    {
                        const auto it = ords.find(n.parents[p]);
                        if (it == ords.end())
                            problems.push_back(where + "parent " + std::to_string(n.parents[p]) + " is not in the same tree");
                        else if (tree.nodes[static_cast<size_t>(it->second)].row >= n.row)
                            problems.push_back(where + "parent " + std::to_string(n.parents[p]) + " is not in a row above it");
                        else
                            r.parents[p] = it->second;
                    }
                    if (n.kind == NodeKind::Choice)
                    {
                        if (n.maxRank != 1 || n.options.size() < 2 || n.options.size() > kMaxOptions || !n.ranks.empty())
                            problems.push_back(where + "a choice node needs max rank 1 and 2-3 options");
                        for (const auto& o : n.options)
                            if (o.empty())
                                problems.push_back(where + "an option without spells");
                    }
                    else
                    {
                        if (n.maxRank < 1 || n.maxRank > kMaxWireRank || n.ranks.size() != n.maxRank || !n.options.empty())
                            problems.push_back(where + "max rank " + std::to_string(n.maxRank) + " does not match its " +
                                               std::to_string(n.ranks.size()) + " rank(s) of spells (1-9)");
                        for (const auto& rk : n.ranks)
                            if (rk.empty())
                                problems.push_back(where + "a rank without spells");
                    }
                    tree.rules.nodes.push_back(r);
                }
            }
            classes.clear();
            for (const auto& [id, tree] : trees)
            {
                EfClass& cls = classes[tree.classId];
                cls.classId = tree.classId;
                if (tree.specId == 0)
                {
                    if (cls.classTree)
                        problems.push_back("class " + std::to_string(tree.classId) + ": two class trees");
                    cls.classTree = id;
                }
                else
                {
                    const EfSpec* spec = Spec(tree.specId);
                    if (!spec || spec->classId != tree.classId)
                        problems.push_back("tree " + std::to_string(id) + ": spec " + std::to_string(tree.specId) + " is not of class " +
                                           std::to_string(tree.classId));
                }
            }
            for (auto& [id, spec] : specs)
            {
                spec.treeId = 0;
                for (const auto& [tid, tree] : trees)
                    if (tree.specId == id)
                    {
                        if (spec.treeId)
                            problems.push_back("spec " + std::to_string(id) + ": two spec trees");
                        spec.treeId = tid;
                    }
                if (!spec.treeId)
                    problems.push_back("spec " + std::to_string(id) + ": no spec tree");
                if (spec.sort < 1 || spec.sort > 32)
                    problems.push_back("spec " + std::to_string(id) + ": sort outside 1-32");
                EfClass& cls = classes[spec.classId];
                cls.classId = spec.classId;
                cls.specs.push_back(id);
            }
            for (auto& [cid, cls] : classes)
            {
                if (!cls.classTree)
                    problems.push_back("class " + std::to_string(cid) + ": no class tree");
                std::sort(cls.specs.begin(), cls.specs.end(), [&](uint32_t a, uint32_t b) { return specs[a].sort < specs[b].sort; });
            }
            if (!specs.empty() && points.find(0) == points.end())
                problems.push_back("no default point schedule (ef_talent_points class 0)");
            // Every spec's empty loadout must pass the rules at the cap (TalentRules.h on the loaded data).
            for (const auto& [id, spec] : specs)
            {
                const EfClass* cls = Class(spec.classId);
                const EfTree* ct = cls ? Tree(cls->classTree) : nullptr;
                const EfTree* st = Tree(spec.treeId);
                if (!ct || !st)
                    continue;
                const std::string empty = std::string(ct->nodes.size(), '0') + "." + std::string(st->nodes.size(), '0');
                const RuleResult r = Validate(spec, empty, 255);
                if (!r.ok())
                    problems.push_back("spec " + std::to_string(id) + ": the empty loadout fails the rules (" + errorCode(r.code) + " " +
                                       std::to_string(r.detail) + ")");
            }
        }

        // The canonical JSON the data hash is the SHA-256 of (talents/compile.py structure(): sorted keys, no
        // whitespace, every list sorted as there).
        std::string CanonicalJson() const
        {
            auto list = [](const std::vector<std::string>& items) {
                std::string out = "[";
                for (size_t i = 0; i < items.size(); ++i)
                    out += (i ? "," : "") + items[i];
                return out + "]";
            };
            auto object = [](const std::vector<std::pair<std::string, std::string>>& fields) {
                std::string out = "{";
                for (size_t i = 0; i < fields.size(); ++i)
                    out += (i ? ",\"" : "\"") + fields[i].first + "\":" + fields[i].second;
                return out + "}";
            };
            auto num = [](uint32_t v) { return std::to_string(v); };
            auto ints = [&](std::vector<uint32_t> v, bool sorted) {
                if (sorted)
                    std::sort(v.begin(), v.end());
                std::vector<std::string> s;
                for (const uint32_t x : v)
                    s.push_back(num(x));
                return list(s);
            };
            std::vector<std::pair<std::string, std::string>> schedules;
            for (const auto& [classId, levels] : points)
            {
                std::vector<std::string> rows;
                for (const auto& [level, b] : levels)
                    rows.push_back(ints({level, b.first, b.second}, false));
                schedules.emplace_back(num(classId), list(rows));
            }
            std::sort(schedules.begin(), schedules.end());
            std::vector<std::string> classList;
            std::set<uint32_t> classIds;
            for (const auto& [id, t] : trees)
                classIds.insert(t.classId);
            for (const uint32_t classId : classIds)
            {
                std::vector<std::string> specList;
                for (const auto& [id, s] : specs)
                {
                    if (s.classId != classId)
                        continue;
                    std::vector<std::vector<uint32_t>> pairs;
                    for (const auto& [spell, level] : s.baseline)
                        pairs.push_back({spell, level});
                    std::sort(pairs.begin(), pairs.end());
                    std::vector<std::string> baseline;
                    for (const auto& p : pairs)
                        baseline.push_back(ints(p, false));
                    specList.push_back(object({{"available_level", num(s.availableLevel)}, {"baseline", list(baseline)}, {"id", num(s.id)},
                                               {"order", num(s.sort)}, {"role", num(s.role)}, {"skill_line", num(s.skillLine)}}));
                }
                std::vector<std::string> treeList;
                for (const auto& [id, t] : trees)
                {
                    if (t.classId != classId)
                        continue;
                    std::vector<std::vector<uint32_t>> gatePairs;
                    for (const RuleGate& g : t.gates)
                        gatePairs.push_back({g.row, g.points});
                    std::sort(gatePairs.begin(), gatePairs.end());
                    std::vector<std::string> gates;
                    for (const auto& g : gatePairs)
                        gates.push_back(ints(g, false));
                    std::vector<const EfNode*> byId;
                    for (const EfNode& n : t.nodes)
                        byId.push_back(&n);
                    std::sort(byId.begin(), byId.end(), [](const EfNode* a, const EfNode* b) { return a->id < b->id; });
                    std::vector<std::string> nodes;
                    for (const EfNode* n : byId)
                    {
                        std::vector<std::string> ranks, options;
                        for (const auto& r : n->ranks)
                            ranks.push_back(ints(r, true));
                        for (const auto& o : n->options)
                            options.push_back(ints(o, true));
                        nodes.push_back(object({{"free", num(n->freeSpecMask)}, {"id", num(n->id)}, {"kind", num(static_cast<uint32_t>(n->kind))},
                                                {"max_rank", num(n->maxRank)}, {"options", list(options)}, {"ord", num(n->ord)},
                                                {"parents", ints(n->parents, true)}, {"ranks", list(ranks)}, {"replaces", num(n->replaces)},
                                                {"required_level", num(n->requiredLevel)}, {"row", num(n->row)}}));
                    }
                    treeList.push_back(object({{"gates", list(gates)}, {"id", num(t.id)}, {"nodes", list(nodes)}, {"spec", num(t.specId)}}));
                }
                classList.push_back(object({{"class", num(classId)}, {"specs", list(specList)}, {"trees", list(treeList)}}));
            }
            const std::string rules = object({{"max_loadouts", num(maxLoadouts)}, {"max_rank", num(maxRank)}, {"points", object(schedules)},
                                              {"spec_level", num(specLevel)}, {"switch_cast_ms", num(switchCastMs)},
                                              {"switch_spell", num(switchSpell)}});
            return object({{"classes", list(classList)}, {"rules", rules}, {"schema", num(schema)}});
        }
    };
}
