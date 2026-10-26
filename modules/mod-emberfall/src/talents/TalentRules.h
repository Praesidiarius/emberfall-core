// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 the Emberfall authors.
//
// TalentRules.h: the rules of the Emberfall spec and talent trees (docs/design/talents.md §1.3, §3.4, §8) as a
// small pure function over (tree data, rank string, level, budgets). Header-only, standard library only, no
// dependency on the client or the server.
//
// This one file is shared: the client uses it for previews (client/src/data/), and the server module
// mod-emberfall carries a verbatim copy for its authoritative validation (Apache-2.0 code may be included in the
// AGPL-3.0 module; never the other way round, SPEC §2.3). Both run the same JSON test vectors
// (tools/py/tests/data/talent_rules/*.json, written by `python -m emberfall_content.talents vectors`, whose Python
// twin is tools/py/emberfall_content/talents/rules.py), so preview and server cannot drift. Change all three
// together.
//
// Wire form (talents.md §4.3): a tree's rank string has one character per node in `ord` order, `0`-`9` the rank of
// a normal node, `0` or `a`-`d` the option of a choice node; a loadout is `<classRanks>.<specRanks>`.
//
// Points (T6): a node's ranks are **paid** unless the node is free for the spec (freeSpecMask has the spec's bit,
// `free_for_specs`) or granted (autoCount > 0: rank k once the tree's paid points reach autoAt[k-1]). Budgets,
// gates ("points spent in this tree first") and granted ranks all count paid points. A granted node with a required
// level is granted from that level on (the points the character has *earned*, not spent: the Wrathbringer's row
// abilities until 2026-10-26); below it its rank is 0, so the nodes it opens stay shut.
//
// Pick groups (2026-10-26, the Wrathbringer's row abilities; talents.md §1.3 rule 7): a node with a `group` (1-based into
// the tree's `groups`) is a **pick**: rank 0 or 1, never paid (no point, no budget counts it), but a player holds only as
// many picks of a group as its slots are open, slot k opening once the tree's paid points reach the group's k-th
// threshold. A final state is valid when some order of its picks reaches it: each pick made while its group has a free
// open slot, counting only the paid points of the nodes that do not hang on a later pick (Slot; detail: the first pick
// in `ord` order the largest set of picks reached, the first found depth-first, lacks).
//
// Checks, the first failure wins: Decode (length, characters, a letter on a normal node); then per node of the class
// tree and then of the spec tree, in `ord` order, for each invested paid node: Rank (above its max), Level (its
// required level), Path (it has parents and none is open: parent k opens it with parentRanks[k] points, 0 = when
// full), Gate (the paid points of the nodes with a lower gate are fewer than its gate; detail: the gate), Choice (a
// digit above 0 or an unknown option on a choice node); after each tree's nodes its Slot check; finally BudgetClass,
// BudgetSpec. Details are node IDs except for Gate. Picks get the per-node checks too (Rank: above 1).
//
// Free and granted nodes skip the per-node checks whatever the string says. A node's gate only counts nodes with a
// lower gate and edges never form a cycle (the compiler checks), so a final state is valid iff every invested node
// passes on its own: some purchase order reaches it (talents.md §1.3).

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ef::talents
{
    enum class NodeKind : uint8_t
    {
        Passive = 1,
        Active = 2,
        Choice = 3,
    };

    enum class RuleError : uint8_t
    {
        Ok,
        Decode,
        Rank,
        Level,
        Path,
        Gate,
        Choice,
        BudgetClass,
        BudgetSpec,
        Slot,
    };

    // The protocol's error codes (talents.md §4.5).
    constexpr const char* errorCode(RuleError e)
    {
        switch (e)
        {
        case RuleError::Ok: return "OK";
        case RuleError::Decode: return "DECODE";
        case RuleError::Rank: return "RANK";
        case RuleError::Level: return "LEVEL";
        case RuleError::Path: return "PATH";
        case RuleError::Gate: return "GATE";
        case RuleError::Choice: return "CHOICE";
        case RuleError::BudgetClass: return "BUDGET_C";
        case RuleError::BudgetSpec: return "BUDGET_S";
        case RuleError::Slot: return "SLOT";
        }
        return "?";
    }

    constexpr size_t kMaxParents = 4;
    constexpr uint8_t kMaxWireRank = 9;
    constexpr uint8_t kMaxOptions = 4; // a-d on the wire (the Wrathbringer's capstone row: one of four)
    constexpr size_t kMaxPicks = 16;   // pick nodes a Slot check orders (the compiler allows no more per tree)

    struct RuleNode
    {
        uint32_t id = 0;
        uint32_t row = 0;
        NodeKind kind = NodeKind::Passive;
        uint8_t maxRank = 1;      // 1 for a choice node
        uint8_t numOptions = 0;   // choice nodes: 2-4
        uint32_t requiredLevel = 0; // a paid node: bought from this level; a granted node: granted from this level
        uint32_t freeSpecMask = 0;
        uint32_t gate = 0;        // points spent in the tree first (its own or its row gate, the higher)
        std::array<int32_t, kMaxParents> parents{-1, -1, -1, -1}; // `ord` indices into the same tree, -1 = none
        std::array<uint8_t, kMaxParents> parentRanks{};           // the points parent k needs, 0 = full
        uint8_t autoCount = 0;                                    // a granted node's ranks (0: bought with points)
        std::array<uint32_t, kMaxWireRank> autoAt{};              // rank k at autoAt[k-1] paid points in the tree
        uint8_t group = 0;                                        // a pick node's group, 1-based into RuleTree::groups
    };

    struct RuleGate
    {
        uint32_t row = 0;
        uint32_t points = 0;
    };

    struct RuleTree
    {
        std::vector<RuleNode> nodes; // in `ord` order
        std::vector<RuleGate> gates; // the row gates (already folded into each node's `gate`; kept for the UI)
        std::vector<std::vector<uint32_t>> groups; // pick groups: [g-1] the points spent in the tree each slot opens at
    };

    // One node's entry in a rank string: the rank (a choice node: 1 when an option is picked) and the
    // option (1-4, choice nodes only; 0 = none).
    struct NodeValue
    {
        uint8_t rank = 0;
        uint8_t option = 0;
    };

    struct RuleResult
    {
        RuleError code = RuleError::Ok;
        uint32_t detail = 0;
        bool ok() const { return code == RuleError::Ok; }
    };

    // The values of a tree's rank string, or nothing when it is malformed.
    inline std::optional<std::vector<NodeValue>> decodeRanks(const RuleTree& tree, std::string_view text)
    {
        if (text.size() != tree.nodes.size())
            return std::nullopt;
        std::vector<NodeValue> out(text.size());
        for (size_t i = 0; i < text.size(); ++i)
        {
            const char c = text[i];
            if (c >= '0' && c <= '9')
                out[i].rank = static_cast<uint8_t>(c - '0');
            else if (c >= 'a' && c < static_cast<char>('a' + kMaxOptions) && tree.nodes[i].kind == NodeKind::Choice)
                out[i] = NodeValue{1, static_cast<uint8_t>(c - 'a' + 1)};
            else
                return std::nullopt;
        }
        return out;
    }

    // The rank string of a tree's values (the inverse of decodeRanks).
    inline std::string encodeRanks(const RuleTree& tree, std::span<const NodeValue> values)
    {
        std::string out(tree.nodes.size(), '0');
        for (size_t i = 0; i < tree.nodes.size() && i < values.size(); ++i)
        {
            if (tree.nodes[i].kind == NodeKind::Choice && values[i].option >= 1 && values[i].option <= kMaxOptions)
                out[i] = static_cast<char>('a' + values[i].option - 1);
            else
                out[i] = static_cast<char>('0' + (values[i].rank > kMaxWireRank ? kMaxWireRank : values[i].rank));
        }
        return out;
    }

    // A pick node (of a pick group): picked for free, as the group's open slots allow.
    inline bool isPick(const RuleNode& node) { return node.group != 0; }

    // Bought with points by this spec: not free for it, not granted, not a pick.
    inline bool isPaid(const RuleNode& node, uint32_t specBit)
    {
        return (node.freeSpecMask & specBit) == 0 && node.autoCount == 0 && !isPick(node);
    }

    // What a loadout stores for this spec: paid nodes and picks.
    inline bool isStored(const RuleNode& node, uint32_t specBit) { return isPaid(node, specBit) || isPick(node); }

    // How many picks group `group` (1-based) allows at `paid` points spent in the tree.
    inline uint32_t openSlots(const RuleTree& tree, uint32_t group, uint32_t paid)
    {
        if (group < 1 || group > tree.groups.size())
            return 0;
        uint32_t open = 0;
        for (const uint32_t at : tree.groups[group - 1])
            if (at <= paid)
                ++open;
        return open;
    }

    // The ranks a value buys (a choice node: 1 when anything is picked).
    inline uint32_t boughtRank(const RuleNode& node, NodeValue value)
    {
        if (node.kind == NodeKind::Choice)
            return value.option != 0 || value.rank != 0 ? 1u : 0u;
        return value.rank;
    }

    // Points paid in a tree.
    inline uint32_t spentPoints(const RuleTree& tree, std::span<const NodeValue> values, uint32_t specBit)
    {
        uint32_t sum = 0;
        for (size_t i = 0; i < tree.nodes.size() && i < values.size(); ++i)
            if (isPaid(tree.nodes[i], specBit))
                sum += boughtRank(tree.nodes[i], values[i]);
        return sum;
    }

    // Points paid in a tree on nodes whose gate is below `gate` (what a node with that gate can follow).
    inline uint32_t paidBefore(const RuleTree& tree, std::span<const NodeValue> values, uint32_t gate, uint32_t specBit)
    {
        uint32_t sum = 0;
        for (size_t i = 0; i < tree.nodes.size() && i < values.size(); ++i)
            if (isPaid(tree.nodes[i], specBit) && tree.nodes[i].gate < gate)
                sum += boughtRank(tree.nodes[i], values[i]);
        return sum;
    }

    // A granted node's rank at `paid` points spent in its tree for a character of `level` (0 below its required level).
    inline uint32_t grantedRank(const RuleNode& node, uint32_t paid, uint32_t level)
    {
        if (node.requiredLevel > level)
            return 0;
        uint32_t rank = 0;
        for (uint32_t k = 0; k < node.autoCount && k < kMaxWireRank; ++k)
            if (node.autoAt[k] <= paid)
                ++rank;
        return rank;
    }

    // The rank a node counts with: a free node its max rank, a granted node by the tree's paid points (`paid`) and
    // the character's level, a choice node 1 when anything is picked.
    inline uint32_t effectiveRank(const RuleNode& node, NodeValue value, uint32_t specBit, uint32_t paid, uint32_t level)
    {
        if ((node.freeSpecMask & specBit) != 0)
            return node.maxRank;
        if (node.autoCount != 0)
            return grantedRank(node, paid, level);
        return boughtRank(node, value);
    }

    inline bool isFull(const RuleNode& node, uint32_t effective) { return effective > 0 && effective >= node.maxRank; }

    // Whether parent k (`ord` index p) opens a node: with the points the edge asks for, else when full.
    inline bool opens(const RuleNode& parent, uint8_t need, uint32_t effective)
    {
        return effective > 0 && effective >= (need != 0 ? need : parent.maxRank);
    }

    // The per-node rules for one invested node (`index` in `ord` order).
    inline RuleResult checkNode(const RuleTree& tree, std::span<const NodeValue> values, size_t index, uint32_t level,
                                uint32_t specBit)
    {
        const RuleNode& node = tree.nodes[index];
        const NodeValue value = values[index];
        if (!isStored(node, specBit) || (value.rank == 0 && value.option == 0))
            return {};
        if (node.kind != NodeKind::Choice && value.rank > node.maxRank)
            return {RuleError::Rank, node.id};
        if (node.requiredLevel > level)
            return {RuleError::Level, node.id};
        const uint32_t paid = spentPoints(tree, values, specBit);
        bool hasParent = false, parentOpen = false;
        for (size_t k = 0; k < kMaxParents; ++k)
        {
            const int32_t p = node.parents[k];
            if (p < 0 || static_cast<size_t>(p) >= tree.nodes.size() || static_cast<size_t>(p) >= values.size())
                continue;
            hasParent = true;
            const size_t pi = static_cast<size_t>(p);
            if (opens(tree.nodes[pi], node.parentRanks[k], effectiveRank(tree.nodes[pi], values[pi], specBit, paid, level)))
                parentOpen = true;
        }
        if (hasParent && !parentOpen)
            return {RuleError::Path, node.id};
        if (paidBefore(tree, values, node.gate, specBit) < node.gate)
            return {RuleError::Gate, node.gate};
        if (node.kind == NodeKind::Choice && (value.option == 0 || value.option > node.numOptions))
            return {RuleError::Choice, node.id};
        return {};
    }

    namespace detail
    {
        // A state where of the picks only those in `have` (one flag per node, `ord` order) are made: which invested paid
        // nodes count (their level, path through counted nodes, made picks, free and granted nodes, and gate by the
        // counted nodes with a lower gate hold) and their paid points. The closure is unique: every condition only grows
        // with the counted set.
        struct Restricted
        {
            const RuleTree& tree;
            std::span<const NodeValue> values;
            uint32_t specBit, level;
            const std::vector<bool>& have;
            std::vector<bool> counted;
            uint32_t total = 0;

            uint32_t rank(size_t j) const
            {
                const RuleNode& n = tree.nodes[j];
                if (isPick(n))
                    return have[j] ? boughtRank(n, values[j]) : 0u;
                if ((n.freeSpecMask & specBit) != 0)
                    return n.maxRank;
                if (n.autoCount != 0)
                    return grantedRank(n, total, level);
                return counted[j] ? boughtRank(n, values[j]) : 0u;
            }

            // Level, path and gate of node i in this state.
            bool holds(size_t i) const
            {
                const RuleNode& n = tree.nodes[i];
                if (n.requiredLevel > level)
                    return false;
                bool hasParent = false, parentOpen = false;
                for (size_t k = 0; k < kMaxParents; ++k)
                {
                    const int32_t p = n.parents[k];
                    if (p < 0 || static_cast<size_t>(p) >= tree.nodes.size())
                        continue;
                    hasParent = true;
                    if (opens(tree.nodes[static_cast<size_t>(p)], n.parentRanks[k], rank(static_cast<size_t>(p))))
                        parentOpen = true;
                }
                if (hasParent && !parentOpen)
                    return false;
                uint32_t before = 0;
                for (size_t j = 0; j < tree.nodes.size(); ++j)
                    if (counted[j] && isPaid(tree.nodes[j], specBit) && tree.nodes[j].gate < n.gate)
                        before += boughtRank(tree.nodes[j], values[j]);
                return before >= n.gate;
            }

            void close()
            {
                counted.assign(tree.nodes.size(), false);
                total = 0;
                for (bool grew = true; grew;)
                {
                    grew = false;
                    for (size_t i = 0; i < tree.nodes.size(); ++i)
                    {
                        const RuleNode& n = tree.nodes[i];
                        if (counted[i] || !isPaid(n, specBit) || boughtRank(n, values[i]) == 0 || !holds(i))
                            continue;
                        counted[i] = true;
                        total += boughtRank(n, values[i]);
                        grew = true;
                    }
                }
            }
        };
    }

    // Slot: some order makes every pick the state holds, each while its group has a free open slot (depth-first over the
    // sets of picks made, in `ord` order; rules.py check_slots step for step).
    inline RuleResult checkSlots(const RuleTree& tree, std::span<const NodeValue> values, uint32_t level, uint32_t specBit)
    {
        std::vector<size_t> picks;
        for (size_t i = 0; i < tree.nodes.size() && i < values.size(); ++i)
            if (isPick(tree.nodes[i]) && boughtRank(tree.nodes[i], values[i]) > 0)
                picks.push_back(i);
        if (picks.empty())
            return {};
        if (picks.size() > kMaxPicks)
            return {RuleError::Slot, tree.nodes[picks[kMaxPicks]].id};
        const uint32_t full = (1u << picks.size()) - 1;
        std::vector<bool> seen(size_t{1} << picks.size(), false);
        uint32_t best = 0;
        const auto bits = [](uint32_t m) {
            uint32_t c = 0;
            for (; m; m &= m - 1)
                ++c;
            return c;
        };
        const auto dfs = [&](const auto& self, uint32_t mask) -> bool {
            if (seen[mask])
                return false;
            seen[mask] = true;
            if (bits(mask) > bits(best))
                best = mask;
            if (mask == full)
                return true;
            std::vector<bool> have(tree.nodes.size(), false);
            for (size_t b = 0; b < picks.size(); ++b)
                if ((mask >> b) & 1u)
                    have[picks[b]] = true;
            detail::Restricted state{tree, values, specBit, level, have, {}, 0};
            state.close();
            for (size_t b = 0; b < picks.size(); ++b)
            {
                if ((mask >> b) & 1u)
                    continue;
                const uint32_t group = tree.nodes[picks[b]].group;
                uint32_t used = 0;
                for (size_t j = 0; j < tree.nodes.size(); ++j)
                    if (have[j] && tree.nodes[j].group == group)
                        ++used;
                if (used < openSlots(tree, group, state.total) && state.holds(picks[b]))
                    if (self(self, mask | (1u << b)))
                        return true;
            }
            return false;
        };
        if (dfs(dfs, 0))
            return {};
        for (size_t b = 0; b < picks.size(); ++b)
            if (!((best >> b) & 1u))
                return {RuleError::Slot, tree.nodes[picks[b]].id};
        return {};
    }

    // Every node rule of one tree, in `ord` order, then its Slot check (no budget).
    inline RuleResult checkTree(const RuleTree& tree, std::span<const NodeValue> values, uint32_t level, uint32_t specBit)
    {
        if (values.size() != tree.nodes.size())
            return {RuleError::Decode, 0};
        for (size_t i = 0; i < tree.nodes.size(); ++i)
            if (const RuleResult r = checkNode(tree, values, i, level, specBit); !r.ok())
                return r;
        return checkSlots(tree, values, level, specBit);
    }

    // A whole loadout `<classRanks>.<specRanks>` for a character of `level` in the spec with `specBit`
    // (1 << (EfSpec sort - 1); 0 before a spec is chosen, with an empty spec tree) and the budgets of that level.
    inline RuleResult validateLoadout(const RuleTree& classTree, const RuleTree& specTree, std::string_view loadout, uint32_t level,
                                      uint32_t classBudget, uint32_t specBudget, uint32_t specBit)
    {
        const size_t dot = loadout.find('.');
        if (dot == std::string_view::npos || loadout.find('.', dot + 1) != std::string_view::npos)
            return {RuleError::Decode, 0};
        const auto classValues = decodeRanks(classTree, loadout.substr(0, dot));
        const auto specValues = decodeRanks(specTree, loadout.substr(dot + 1));
        if (!classValues || !specValues)
            return {RuleError::Decode, 0};
        if (const RuleResult r = checkTree(classTree, *classValues, level, specBit); !r.ok())
            return r;
        if (const RuleResult r = checkTree(specTree, *specValues, level, specBit); !r.ok())
            return r;
        if (spentPoints(classTree, *classValues, specBit) > classBudget)
            return {RuleError::BudgetClass, 0};
        if (spentPoints(specTree, *specValues, specBit) > specBudget)
            return {RuleError::BudgetSpec, 0};
        return {};
    }
}
